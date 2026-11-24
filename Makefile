.POSIX:
.SUFFIXES:

include config.mk

PROTO_LAYER = protocols/wlr-layer-shell-unstable-v1.xml
PROTO_XDG = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
GEN = wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-protocol.h \
      xdg-shell-protocol.c xdg-shell-protocol.h
OBJ = sbar.o wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o

all: sbar

sbar: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

sbar.o: sbar.c config.h wlr-layer-shell-unstable-v1-protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sbar.c

wlr-layer-shell-unstable-v1-protocol.h: $(PROTO_LAYER)
	$(WAYLAND_SCANNER) client-header $(PROTO_LAYER) $@

wlr-layer-shell-unstable-v1-protocol.c: $(PROTO_LAYER)
	$(WAYLAND_SCANNER) private-code $(PROTO_LAYER) $@

wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ wlr-layer-shell-unstable-v1-protocol.c

xdg-shell-protocol.h: $(PROTO_XDG)
	$(WAYLAND_SCANNER) client-header $(PROTO_XDG) $@

xdg-shell-protocol.c: $(PROTO_XDG)
	$(WAYLAND_SCANNER) private-code $(PROTO_XDG) $@

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ xdg-shell-protocol.c

install: sbar
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 sbar $(DESTDIR)$(PREFIX)/bin/sbar

clean:
	rm -f sbar $(OBJ) $(GEN)

.PHONY: all clean install
