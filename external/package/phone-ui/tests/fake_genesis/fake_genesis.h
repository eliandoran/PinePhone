/*
 * Genesis Plus GX in the tests: fake_genesis/libretro.c, built as the app's
 * lib/genesis_plus_gx_libretro.so, which genesis.so loads with dlopen() when a game
 * first starts. Its state is the test program's (fake_genesis, defined in
 * test_genesis.c, exported: the program is linked -rdynamic). The core runs in the
 * plugin's thread, in real time: wait for what it does with ui_test_wait_until().
 */

#ifndef FAKE_GENESIS_H
#define FAKE_GENESIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fake_genesis {
	/* What the test sets */
	uint32_t color;          /* of every pixel of its frames, 0xRRGGBB (RGB565) */
	unsigned w, h;           /* its frames' size, 320 x 224 unless set */
	bool load_fails;         /* retro_load_game() refuses the game */
	uint8_t sram[256];       /* its battery-backed RAM */
	size_t sram_size;        /* 0: the game has none */

	/* What the core did */
	char system_dir[256], save_dir[256]; /* the front end's folders */
	bool rgb565;             /* the front end took its pixel format */
	int loads, unloads, resets;
	char path[256];          /* the game loaded last */
	unsigned frames;         /* run, all games together */
	char state[64];          /* the save state it was given last ("" none) */
};

extern struct fake_genesis fake_genesis;

#endif
