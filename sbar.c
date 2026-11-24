#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "config.h"

#define MAX_TAGS 32

struct state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	int mango_fd;
	char mango_buf[8192];
	size_t mango_len;
	int ipc_tag_count;

	uint32_t width;
	uint32_t height;
	int configured;
	int running;
	int recreate;
	int dirty;
	int buffer_busy;

	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t pixels_size;

	uint32_t tags_occ;
	uint32_t tags_sel;
	uint32_t tags_foc;
	uint32_t tags_urg;

	uint64_t cpu_prev_total;
	uint64_t cpu_prev_idle;
	uint64_t last_metrics;
	int cpu_usage;
	int mem_usage;
	int volume;
	int muted;
};

static volatile sig_atomic_t sig_running = 1;

static void
sig_handler(int sig)
{
	(void)sig;
	sig_running = 0;
}

static void
die(const char *msg)
{
	fprintf(stderr, "sbar: %s\n", msg);
	exit(1);
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t
premultiply(uint32_t color)
{
	uint32_t a = (color >> 24) & 0xff;
	uint32_t r = ((color >> 16) & 0xff) * a / 255;
	uint32_t g = ((color >> 8) & 0xff) * a / 255;
	uint32_t b = (color & 0xff) * a / 255;

	return (a << 24) | (r << 16) | (g << 8) | b;
}

static void
fill_rect(struct state *s, int x, int y, int w, int h, uint32_t color)
{
	uint32_t sa, inv, src, sr, sg, sb;
	int yy, xx;

	if (w <= 0 || h <= 0)
		return;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > (int)s->width)
		w = (int)s->width - x;
	if (y + h > (int)s->height)
		h = (int)s->height - y;
	if (w <= 0 || h <= 0)
		return;

	sa = (color >> 24) & 0xff;
	inv = 255 - sa;
	src = premultiply(color);
	sr = (src >> 16) & 0xff;
	sg = (src >> 8) & 0xff;
	sb = src & 0xff;

	for (yy = y; yy < y + h; yy++) {
		uint32_t *row = s->pixels + (size_t)yy * s->width;

		for (xx = x; xx < x + w; xx++) {
			uint32_t dst, da, dr, dg, db, oa, or_, og, ob;

			if (sa == 255) {
				row[xx] = src;
				continue;
			}
			if (sa == 0)
				continue;

			dst = row[xx];
			da = (dst >> 24) & 0xff;
			dr = (dst >> 16) & 0xff;
			dg = (dst >> 8) & 0xff;
			db = dst & 0xff;

			oa = sa + da * inv / 255;
			or_ = sr + dr * inv / 255;
			og = sg + dg * inv / 255;
			ob = sb + db * inv / 255;

			row[xx] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
		}
	}
}

static void
draw_tags(struct state *s)
{
	int h = (int)s->height;
	int x = BAR_PADDING;
	int count = TAG_COUNT;
	int i;

	if (count <= 0)
		count = s->ipc_tag_count;
	if (count <= 0) {
		uint32_t mask = s->tags_occ | s->tags_sel |
		                s->tags_foc | s->tags_urg;

		count = 1;
		while (mask > 1) {
			mask >>= 1;
			count++;
		}
	}
	if (count > MAX_TAGS)
		count = MAX_TAGS;

	for (i = 0; i < count; i++) {
		uint32_t bit = 1u << i;
		uint32_t color;

		if (s->tags_urg & bit)
			color = COL_TAG_URGENT;
		else if (s->tags_sel & bit)
			color = COL_TAG_FOCUSED;
		else if (s->tags_occ & bit)
			color = COL_TAG_OCCUPIED;
		else
			color = COL_TAG_EMPTY;

		fill_rect(s, x, 0, TAG_BLOCK_W, h, color);
		x += TAG_BLOCK_W + BLOCK_GAP;
	}
}

static void
draw_clock(struct state *s)
{
	time_t now;
	struct tm tm;
	int span, minutes, block_minutes, total_w, x, h, i;
	int full, frac, partial_w;

	if (CLOCK_BLOCKS <= 0 || CLOCK_BLOCK_W <= 0)
		return;

	now = time(NULL);
	localtime_r(&now, &tm);

	span = CLOCK_SPAN_HOURS > 0 ? CLOCK_SPAN_HOURS : 12;
	minutes = (tm.tm_hour % span) * 60 + tm.tm_min;
	block_minutes = (span * 60) / CLOCK_BLOCKS;
	if (block_minutes <= 0)
		block_minutes = 1;

	full = minutes / block_minutes;
	if (full < 0)
		full = 0;
	if (full > CLOCK_BLOCKS)
		full = CLOCK_BLOCKS;

	frac = minutes % block_minutes;
	partial_w = CLOCK_BLOCK_W * frac / block_minutes;
	if (partial_w < 0)
		partial_w = 0;
	if (partial_w > CLOCK_BLOCK_W)
		partial_w = CLOCK_BLOCK_W;

	total_w = CLOCK_BLOCKS * CLOCK_BLOCK_W +
	          (CLOCK_BLOCKS - 1) * BLOCK_GAP;
	x = ((int)s->width - total_w) / 2;
	if (x < 0)
		x = 0;
	h = (int)s->height;

	for (i = 0; i < CLOCK_BLOCKS; i++) {
		if (i < full) {
			fill_rect(s, x, 0, CLOCK_BLOCK_W, h,
			          COL_CLOCK_FILLED);
		} else if (i == full) {
			if (partial_w > 0)
				fill_rect(s, x, 0, partial_w, h,
				          COL_CLOCK_FILLED);
			if (partial_w < CLOCK_BLOCK_W)
				fill_rect(s, x + partial_w, 0,
				          CLOCK_BLOCK_W - partial_w, h,
				          COL_CLOCK_EMPTY);
		} else {
			fill_rect(s, x, 0, CLOCK_BLOCK_W, h,
			          COL_CLOCK_EMPTY);
		}
		x += CLOCK_BLOCK_W + BLOCK_GAP;
	}
}

static int
draw_sound(struct state *s, int x)
{
	int h = (int)s->height;
	int width = SOUND_BAR_W;
	int fill_w;

	if (width <= 0)
		return x;

	fill_rect(s, x, 0, width, h, COL_SOUND_BG);

	fill_w = s->volume * width / 100;
	if (fill_w < 0)
		fill_w = 0;
	if (fill_w > width)
		fill_w = width;

	if (fill_w > 0)
		fill_rect(s, x, 0, fill_w, h,
		          s->muted ? COL_SOUND_MUTED : COL_SOUND_FG);

	return x + width;
}

static int
metric_width(void)
{
	if (METRIC_BLOCKS <= 0 || METRIC_BLOCK_W <= 0)
		return 0;
	return METRIC_BLOCKS * METRIC_BLOCK_W +
	       (METRIC_BLOCKS - 1) * METRIC_SEG_GAP;
}

static int
draw_metric(struct state *s, int x, int usage, uint32_t color)
{
	int h = (int)s->height;
	int total_w = metric_width();
	int lit, i;

	if (total_w <= 0)
		return x;

	if (usage < 0)
		usage = 0;
	if (usage > 100)
		usage = 100;

	lit = (usage * METRIC_BLOCKS + 99) / 100;
	if (lit > METRIC_BLOCKS)
		lit = METRIC_BLOCKS;

	for (i = 0; i < METRIC_BLOCKS; i++) {
		int sx = x + i * (METRIC_BLOCK_W + METRIC_SEG_GAP);
		uint32_t c = i < lit
		    ? (usage >= METRIC_HIGH_THRESHOLD ? COL_METRIC_HIGH : color)
		    : COL_METRIC_EMPTY;

		fill_rect(s, sx, 0, METRIC_BLOCK_W, h, c);
	}

	return x + total_w;
}

static void
draw_right(struct state *s)
{
	int sw = SOUND_BAR_W > 0 ? SOUND_BAR_W : 0;
	int mw = metric_width();
	int groups = 0;
	int total_w, x;

	if (sw > 0)
		groups++;
	if (mw > 0)
		groups += 2;

	total_w = sw + mw * 2;
	if (groups > 1)
		total_w += (groups - 1) * RIGHT_MODULE_GAP;

	x = (int)s->width - BAR_PADDING - total_w;
	if (x < 0)
		x = 0;

	if (sw > 0) {
		x = draw_sound(s, x);
		x += RIGHT_MODULE_GAP;
	}
	if (mw > 0) {
		x = draw_metric(s, x, s->cpu_usage, COL_CPU);
		x += RIGHT_MODULE_GAP;
	}
	if (mw > 0)
		draw_metric(s, x, s->mem_usage, COL_MEM);
}

static void
draw(struct state *s)
{
	if (!s->configured || !s->buffer || s->buffer_busy)
		return;

	memset(s->pixels, 0, s->pixels_size);
	fill_rect(s, 0, 0, (int)s->width, (int)s->height, COL_BG);
	draw_tags(s);
	draw_clock(s);
	draw_right(s);

	wl_surface_attach(s->surface, s->buffer, 0, 0);
	wl_surface_damage(s->surface, 0, 0,
	                  (int32_t)s->width, (int32_t)s->height);
	wl_surface_commit(s->surface);

	s->buffer_busy = 1;
	s->dirty = 0;
}

static int
read_cpu_usage(struct state *s)
{
	FILE *f;
	char line[256];
	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	unsigned long long total, idle_all, dt, di;
	int usage;

	f = fopen("/proc/stat", "r");
	if (!f)
		return s->cpu_usage;
	if (!fgets(line, sizeof line, f)) {
		fclose(f);
		return s->cpu_usage;
	}
	fclose(f);

	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	           &user, &nice, &system, &idle, &iowait, &irq,
	           &softirq, &steal) < 4)
		return s->cpu_usage;

	total = user + nice + system + idle + iowait + irq + softirq + steal;
	idle_all = idle + iowait;

	if (s->cpu_prev_total == 0) {
		s->cpu_prev_total = total;
		s->cpu_prev_idle = idle_all;
		return s->cpu_usage;
	}

	dt = total - s->cpu_prev_total;
	di = idle_all - s->cpu_prev_idle;
	s->cpu_prev_total = total;
	s->cpu_prev_idle = idle_all;

	if (dt == 0)
		return s->cpu_usage;

	usage = (int)((dt - di) * 100 / dt);
	if (usage < 0)
		usage = 0;
	if (usage > 100)
		usage = 100;

	return usage;
}

static int
read_mem_usage(void)
{
	FILE *f;
	char line[256];
	unsigned long long total = 0, avail = 0;
	int usage;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "MemTotal: %llu", &total) == 1)
			continue;
		if (sscanf(line, "MemAvailable: %llu", &avail) == 1)
			continue;
	}
	fclose(f);

	if (total == 0)
		return 0;

	usage = (int)((total - avail) * 100 / total);
	if (usage < 0)
		usage = 0;
	if (usage > 100)
		usage = 100;

	return usage;
}

static void
read_volume(struct state *s)
{
	FILE *p;
	char line[512];
	int volume = -1, muted = -1;

	p = popen("amixer sget Master < /dev/null 2>/dev/null", "r");
	if (!p)
		return;

	while (fgets(line, sizeof line, p)) {
		char *pct;
		int value;

		if (!strstr(line, "Playback"))
			continue;

		pct = strchr(line, '[');
		if (pct && sscanf(pct, "[%d%%", &value) == 1)
			volume = value;

		if (strstr(line, "[off]"))
			muted = 1;
		else if (strstr(line, "[on]"))
			muted = 0;
	}
	pclose(p);

	if (volume >= 0) {
		if (volume > 100)
			volume = 100;
		s->volume = volume;
	}
	if (muted >= 0)
		s->muted = muted;
}

static void
update_metrics(struct state *s)
{
	s->cpu_usage = read_cpu_usage(s);
	s->mem_usage = read_mem_usage();
	read_volume(s);
}

static const char *
mango_socket_path(void)
{
	const char *p = getenv("MANGO_INSTANCE_SIGNATURE");

	return (p && p[0]) ? p : NULL;
}

static void
mango_disconnect(struct state *s)
{
	if (s->mango_fd >= 0) {
		close(s->mango_fd);
		s->mango_fd = -1;
	}
	s->mango_len = 0;
}

static int
mango_connect(struct state *s)
{
	struct sockaddr_un addr;
	const char *path;
	const char *sub = "watch all-monitors\n";
	int fd, flags;

	if (s->mango_fd >= 0)
		return 1;

	path = mango_socket_path();
	if (!path)
		return 0;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return 0;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (send(fd, sub, strlen(sub), MSG_NOSIGNAL) < 0) {
		close(fd);
		return 0;
	}

	s->mango_fd = fd;
	s->mango_len = 0;
	return 1;
}

static const char *
json_skip_string(const char *p, const char *end)
{
	p++;
	while (p < end && *p != '"') {
		if (*p == '\\' && p + 1 < end)
			p++;
		p++;
	}
	return p < end ? p + 1 : end;
}

static const char *
json_match_brace(const char *p, const char *end)
{
	int depth = 0;

	while (p < end) {
		if (*p == '"') {
			p = json_skip_string(p, end);
			continue;
		}
		if (*p == '{')
			depth++;
		else if (*p == '}') {
			depth--;
			if (depth == 0)
				return p + 1;
		}
		p++;
	}
	return end;
}

static const char *
json_value(const char *obj, const char *obj_end, const char *key)
{
	size_t klen = strlen(key);
	const char *p;

	for (p = obj; p + klen + 3 <= obj_end; p++) {
		if (p[0] != '"' || strncmp(p + 1, key, klen) != 0 ||
		    p[klen + 1] != '"')
			continue;
		p += klen + 2;
		while (p < obj_end && (*p == ' ' || *p == ':'))
			p++;
		return p;
	}
	return NULL;
}

static int
json_bool(const char *obj, const char *obj_end, const char *key)
{
	const char *v = json_value(obj, obj_end, key);

	return v && strncmp(v, "true", 4) == 0;
}

static int
json_int(const char *obj, const char *obj_end, const char *key)
{
	const char *v = json_value(obj, obj_end, key);

	return v ? (int)strtol(v, NULL, 10) : -1;
}

static void
mango_event(struct state *s, const char *json)
{
	const char *p;
	uint32_t occ = 0, sel = 0, urg = 0;
	int tag_count = 0, parsed = 0;
	int changed;

	p = strstr(json, "\"monitors\"");
	if (!p || !(p = strchr(p, '[')))
		return;
	p++;

	while (*p && *p != ']') {
		const char *obj, *obj_end, *tags, *tobj;
		int active, n = 0;
		uint32_t m_occ = 0, m_sel = 0, m_urg = 0;

		if (*p != '{') {
			p++;
			continue;
		}
		obj = p;
		obj_end = json_match_brace(p, json + strlen(json));

		active = json_bool(obj, obj_end, "active");

		if (!parsed || active) {
			tags = json_value(obj, obj_end, "tags");
			if (tags && tags < obj_end && *tags == '[') {
				tags++;
				tobj = tags;
				while (tobj < obj_end && *tobj != ']') {
					const char *t_end;
					int idx, iact, iurg, clients;

					if (*tobj != '{') {
						tobj++;
						continue;
					}
					t_end = json_match_brace(tobj,
					                         obj_end);
					idx = json_int(tobj, t_end,
					               "index");
					iact = json_bool(tobj, t_end,
					                 "is_active");
					iurg = json_bool(tobj, t_end,
					                 "is_urgent");
					clients = json_int(tobj, t_end,
					                   "client_count");

					if (idx >= 1 && idx <= MAX_TAGS) {
						uint32_t bit =
						    1u << (idx - 1);

						if (iact)
							m_sel |= bit;
						if (iurg)
							m_urg |= bit;
						if (clients > 0)
							m_occ |= bit;
						if (idx > n)
							n = idx;
					}
					tobj = t_end;
				}
			}
			occ = m_occ;
			sel = m_sel;
			urg = m_urg;
			tag_count = n;
			parsed = active ? 2 : 1;
		}

		p = obj_end;
	}

	if (!parsed)
		return;

	changed = occ != s->tags_occ || sel != s->tags_sel ||
	          urg != s->tags_urg;

	if (tag_count > 0)
		s->ipc_tag_count = tag_count;
	s->tags_occ = occ;
	s->tags_sel = sel;
	s->tags_urg = urg;
	s->tags_foc = sel;

	if (changed)
		s->dirty = 1;
}

static void
mango_read(struct state *s)
{
	char tmp[4096];
	ssize_t n;

	while ((n = read(s->mango_fd, tmp, sizeof tmp)) > 0) {
		ssize_t i;

		for (i = 0; i < n; i++) {
			if (tmp[i] == '\n') {
				s->mango_buf[s->mango_len] = '\0';
				if (s->mango_len > 0)
					mango_event(s, s->mango_buf);
				s->mango_len = 0;
			} else if (s->mango_len + 1 < sizeof s->mango_buf) {
				s->mango_buf[s->mango_len++] = tmp[i];
			} else {
				s->mango_len = 0;
			}
		}
	}

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return;

	mango_disconnect(s);
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct state *s = data;

	(void)buffer;
	s->buffer_busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void
destroy_buffer(struct state *s)
{
	if (s->buffer) {
		wl_buffer_destroy(s->buffer);
		s->buffer = NULL;
	}
	if (s->pixels) {
		munmap(s->pixels, s->pixels_size);
		s->pixels = NULL;
		s->pixels_size = 0;
	}
	s->buffer_busy = 0;
}

static void
create_buffer(struct state *s)
{
	int stride, fd;
	size_t size;
	void *data;
	struct wl_shm_pool *pool;

	destroy_buffer(s);

	stride = (int)s->width * 4;
	size = (size_t)stride * s->height;

	fd = memfd_create("sbar", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create");

	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		die("ftruncate");
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		die("mmap");
	}

	pool = wl_shm_create_pool(s->shm, fd, (int32_t)size);
	if (!pool) {
		munmap(data, size);
		close(fd);
		die("wl_shm_create_pool");
	}

	s->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)s->width,
	                                      (int32_t)s->height, stride,
	                                      WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	if (!s->buffer) {
		munmap(data, size);
		die("wl_shm_pool_create_buffer");
	}

	s->pixels = data;
	s->pixels_size = size;
	s->buffer_busy = 0;
	wl_buffer_add_listener(s->buffer, &buffer_listener, s);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t width, uint32_t height)
{
	struct state *s = data;

	zwlr_layer_surface_v1_ack_configure(surface, serial);

	if (width == 0)
		width = s->width;
	if (height == 0)
		height = BAR_HEIGHT;
	if (width == 0)
		return;

	if (width != s->width || height != s->height || !s->buffer) {
		s->width = width;
		s->height = height;
		create_buffer(s);
	}

	s->configured = 1;
	s->dirty = 1;
}

static void
destroy_layer_surface(struct state *s)
{
	destroy_buffer(s);
	if (s->layer_surface) {
		zwlr_layer_surface_v1_destroy(s->layer_surface);
		s->layer_surface = NULL;
	}
	if (s->surface) {
		wl_surface_destroy(s->surface);
		s->surface = NULL;
	}
	s->configured = 0;
	s->dirty = 0;
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
	struct state *s = data;

	(void)surface;
	destroy_layer_surface(s);
	s->recreate = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static int
create_layer_surface(struct state *s)
{
	uint32_t anchor;

	if (s->surface || !s->compositor || !s->shm || !s->layer_shell)
		return 0;

	destroy_buffer(s);

	s->surface = wl_compositor_create_surface(s->compositor);
	if (!s->surface)
		return 0;

	s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
	    s->layer_shell, s->surface, NULL,
	    ZWLR_LAYER_SHELL_V1_LAYER_TOP, "sbar");
	if (!s->layer_surface) {
		wl_surface_destroy(s->surface);
		s->surface = NULL;
		return 0;
	}

	zwlr_layer_surface_v1_add_listener(s->layer_surface,
	                                   &layer_surface_listener, s);

	anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
#if BAR_POSITION
	anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
#else
	anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
#endif

	zwlr_layer_surface_v1_set_anchor(s->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(s->layer_surface, 0, BAR_HEIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface,
	                                         BAR_HEIGHT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
	    s->layer_surface,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	s->configured = 0;
	s->dirty = 0;
	s->recreate = 0;
	wl_surface_commit(s->surface);
	wl_display_flush(s->display);

	return 1;
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	struct state *s = data;

	(void)version;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		s->compositor = wl_registry_bind(registry, name,
		                                 &wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		s->shm = wl_registry_bind(registry, name,
		                          &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		s->layer_shell = wl_registry_bind(registry, name,
		                                  &zwlr_layer_shell_v1_interface,
		                                  1);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int
main(void)
{
	struct state s;
	struct sigaction sa;

	memset(&s, 0, sizeof s);
	s.running = 1;
	s.mango_fd = -1;
	umask(077);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	s.display = wl_display_connect(NULL);
	if (!s.display)
		die("wl_display_connect");
	fcntl(wl_display_get_fd(s.display), F_SETFD, FD_CLOEXEC);

	s.registry = wl_display_get_registry(s.display);
	wl_registry_add_listener(s.registry, &registry_listener, &s);
	wl_display_roundtrip(s.display);

	if (!s.compositor || !s.shm || !s.layer_shell)
		die("missing wayland globals");

	mango_connect(&s);

	if (!create_layer_surface(&s))
		die("create_layer_surface");

	update_metrics(&s);
	s.last_metrics = now_ms();
	wl_display_roundtrip(s.display);

	while (s.running && sig_running) {
		struct pollfd fds[2];
		nfds_t nfds = 1;
		int timeout = UPDATE_MS;
		int ret;
		int mango_idx = -1;

		fds[0].fd = wl_display_get_fd(s.display);
		fds[0].events = POLLIN;
		fds[0].revents = 0;

		if (s.mango_fd >= 0) {
			mango_idx = (int)nfds;
			fds[nfds].fd = s.mango_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		while (wl_display_prepare_read(s.display) != 0)
			wl_display_dispatch_pending(s.display);

		wl_display_flush(s.display);

		ret = poll(fds, nfds, timeout);
		if (ret < 0) {
			wl_display_cancel_read(s.display);
			if (errno == EINTR)
				continue;
			break;
		}

		if (ret > 0 && (fds[0].revents & POLLIN)) {
			if (wl_display_read_events(s.display) < 0) {
				s.running = 0;
				break;
			}
			if (wl_display_dispatch_pending(s.display) < 0) {
				s.running = 0;
				break;
			}
		} else {
			wl_display_cancel_read(s.display);
		}

		if (ret > 0 && (fds[0].revents & (POLLERR | POLLHUP))) {
			s.running = 0;
			break;
		}

		if (mango_idx >= 0 && ret > 0) {
			if (fds[mango_idx].revents & POLLIN)
				mango_read(&s);
			if (fds[mango_idx].revents &
			    (POLLERR | POLLHUP | POLLNVAL))
				mango_disconnect(&s);
		}

		if (s.mango_fd < 0)
			mango_connect(&s);

		uint64_t now = now_ms();

		if (now - s.last_metrics >= UPDATE_MS) {
			if (s.recreate && !s.surface)
				create_layer_surface(&s);
			update_metrics(&s);
			s.last_metrics = now;
			s.dirty = 1;
		}

		if (s.dirty && !s.buffer_busy)
			draw(&s);
	}

	destroy_layer_surface(&s);
	mango_disconnect(&s);
	wl_registry_destroy(s.registry);
	wl_display_disconnect(s.display);
	return 0;
}
