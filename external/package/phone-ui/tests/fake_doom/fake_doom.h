/*
 * Doom's engine in the tests: fake_doom/doomengine.c, built as the app's
 * lib/doomengine.so, which doom.so loads with dlopen() when a game starts and
 * unloads when it ends. Its state is the test program's (fake_doom, defined in
 * test_doom.c, exported: the program is linked -rdynamic), so it outlives the
 * library. The engine runs in the plugin's thread, in real time: wait for what it
 * does with ui_test_wait_until().
 */

#ifndef FAKE_DOOM_H
#define FAKE_DOOM_H

#include <stdbool.h>
#include <stdint.h>

struct fake_doom {
	/* What the test sets (read by the engine each frame) */
	uint32_t color;    /* of every pixel of its frames, 0xRRGGBB */
	bool quit;         /* it quits, as from Doom's menu */

	/* What the engine did */
	int runs;          /* doom_run() calls */
	char iwad[256], configdir[256]; /* of the last one */
	unsigned frames;   /* drawn, all runs together */
	int orient;        /* the layout the plugin said it shows */
	bool paused;       /* the plugin paused it (now) */
	bool told_to_quit; /* the plugin ended it (host->quit) */
	bool keys[256];    /* held, as the fingers read each frame (host->touch_read) last said */
	int turn;          /* px dragged sideways, all reports together */
};

extern struct fake_doom fake_doom;

#endif
