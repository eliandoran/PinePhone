/*
 * Between the plugin (plugin.c: phone-ui's screen, the game's texture, the
 * controls drawn) and the engine (lib/doomengine.so: doomgeneric, engine.c's
 * platform layer, sound.c), which the plugin loads with dlopen and runs in a
 * thread: doom_run(). Both are built together.
 *
 * The layouts are here for both: the plugin draws them, the engine finds what
 * the fingers touch in them. Upright, in the screen's coordinates (below phone-ui's
 * status bar and header); sideways (the display turned, full screen), in LVGL's
 * 1440 x 720.
 */

#ifndef DOOM_H
#define DOOM_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define DOOM_W 320
#define DOOM_H 200

enum { DOOM_PORTRAIT, DOOM_LEFT, DOOM_RIGHT }; /* sideways: the phone's top to the left, right */

/* Doom's keys (doomkeys.h's names collide with linux/input.h's) */
#define DOOM_KEY_ENTER 13
#define DOOM_KEY_ESCAPE 27
#define DOOM_KEY_TAB 9
#define DOOM_KEY_UPARROW 0xad
#define DOOM_KEY_DOWNARROW 0xaf
#define DOOM_KEY_STRAFE_L 0xa0
#define DOOM_KEY_STRAFE_R 0xa1
#define DOOM_KEY_USE 0xa2
#define DOOM_KEY_FIRE 0xa3
#define DOOM_KEY_RSHIFT (0x80 + 0x36)
#define DOOM_KEY_PREV_WEAPON '[' /* bound by engine.c: unbound by default */
#define DOOM_KEY_NEXT_WEAPON ']'
#define DOOM_KEY_BACK 0 /* not Doom's: back to phone-ui (sideways, without a header) */

struct doom_button {
	int x, y, w, h;
	const char *label;
	int key;
};

/* FIRE is the last button: a finger landing on it also turns */
struct doom_layout {
	int game_x, game_y, scale;  /* the game, DOOM_W x DOOM_H times scale */
	int middle;                 /* moving on its left, turning on its right */
	int hint_y;                 /* MOVE and TURN written at this height */
	int hint_move_x, hint_turn_x;
	unsigned count;
	struct doom_button buttons[10];
};

#define DOOM_BODY_Y 152 /* upright: phone-ui's status bar and header (UI_STATUS_H + UI_HEADER_H) */

static const struct doom_layout doom_layouts[2] = {
	/* Upright: the game below the header, the buttons Doom needs in its menus
	 * under it, the weapons, USE and FIRE on the right */
	{ 40, DOOM_BODY_Y + 16, 2, 360, 1100, 180, 440, 9, {
		{ 0, 584, 144, 80, "MENU", DOOM_KEY_ESCAPE },
		{ 144, 584, 144, 80, "ENTER", DOOM_KEY_ENTER },
		{ 288, 584, 144, 80, "Y", 'y' },
		{ 432, 584, 144, 80, "N", 'n' },
		{ 576, 584, 144, 80, "MAP", DOOM_KEY_TAB },
		{ 520, 680, 100, 110, "<", DOOM_KEY_PREV_WEAPON },
		{ 620, 680, 100, 110, ">", DOOM_KEY_NEXT_WEAPON },
		{ 520, 800, 200, 190, "USE", DOOM_KEY_USE },
		{ 520, 1000, 200, 430, "FIRE", DOOM_KEY_FIRE },
	} },
	/* Sideways, as Doom ports have it: the game 3 times its size in the middle */
	{ 240, 60, 3, 720, 690, 480, 960, 10, {
		{ 0, 0, 90, 120, "<", DOOM_KEY_BACK },
		{ 90, 0, 150, 120, "MENU", DOOM_KEY_ESCAPE },
		{ 240, 0, 320, 60, "ENTER", DOOM_KEY_ENTER },
		{ 560, 0, 320, 60, "Y", 'y' },
		{ 880, 0, 320, 60, "N", 'n' },
		{ 1200, 0, 240, 120, "MAP", DOOM_KEY_TAB },
		{ 1200, 120, 120, 120, "<", DOOM_KEY_PREV_WEAPON },
		{ 1320, 120, 120, 120, ">", DOOM_KEY_NEXT_WEAPON },
		{ 1200, 240, 240, 180, "USE", DOOM_KEY_USE },
		{ 1200, 420, 240, 300, "FIRE", DOOM_KEY_FIRE },
	} },
};

static inline const struct doom_layout *doom_layout_of(int orient)
{
	return &doom_layouts[orient != DOOM_PORTRAIT];
}

struct doom_host {
	/* Set by the plugin before doom_run() */
	const char *iwad;      /* path */
	const char *configdir; /* where settings and saves go */

	/* Set by the plugin (atomic) */
	int orient;            /* of the layout shown: touches are found in it */
	bool paused;           /* the app is not shown: Doom stops, the sound device is let go */

	/* Set by the engine */
	pthread_mutex_t lock;  /* frame, frame_seq */
	uint32_t frame[DOOM_W * DOOM_H]; /* BGRA, as LVGL's textures */
	unsigned frame_seq;
	unsigned pressed;      /* buttons held, a bit per index in the layout (atomic) */
	bool exited;           /* Doom quit (its menu, or an error): doom_run() returned */
};

/* The engine's entry point: runs Doom until it quits */
typedef void doom_run_fn(struct doom_host *host);

#endif
