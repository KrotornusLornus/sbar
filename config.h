#ifndef CONFIG_H
#define CONFIG_H

/*
 * Colores ARGB de 32 bits: 0xAARRGGBB.
 * 0xffRRGGBB = opaco. Ejemplo: 0xff00ffff.
 */

/* Barra */
#define BAR_HEIGHT       5
#define BAR_POSITION     0      /* 0 = arriba, 1 = abajo */
#define BAR_PADDING      4
#define BLOCK_GAP        2
#define RIGHT_MODULE_GAP 20
#define UPDATE_MS        1000
#define COL_BG           0xcc1a1b00

/* Tags: solo color */
#define TAG_COUNT        9
#define TAG_BLOCK_W      20
#define COL_TAG_EMPTY    0xff434c5e
#define COL_TAG_OCCUPIED 0xff5e81ac
#define COL_TAG_FOCUSED  0xff88c0d0
#define COL_TAG_URGENT   0xffe0566b

/* Reloj: 12 bloques, relleno continuo del bloque actual */
#define CLOCK_BLOCKS     6
#define CLOCK_SPAN_HOURS 6
#define CLOCK_BLOCK_W    20
#define COL_CLOCK_EMPTY  0xff3b4252
#define COL_CLOCK_FILLED 0xff5e81ac

/* Sonido: barra continua */
#define SOUND_BAR_W      40
#define COL_SOUND_BG     0xff2e3440
#define COL_SOUND_FG     0xffb48ead
#define COL_SOUND_MUTED  0xff9299a8

/* CPU y memoria: segmentos discretos */
#define METRIC_BLOCKS    5
#define METRIC_BLOCK_W   20
#define METRIC_SEG_GAP   3
/* Umbral (%) a partir del cual se usa COL_METRIC_HIGH */
#define METRIC_HIGH_THRESHOLD 95
#define COL_CPU          0xffa3be8c
#define COL_MEM          0xff8fbcbb
#define COL_METRIC_EMPTY 0xff2e3440
#define COL_METRIC_HIGH  0xffe0566b

#endif
