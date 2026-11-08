/*
 * Between the plugin (plugin.c: phone-ui's screen, the game's texture, the
 * controls drawn) and the engine (lib/doomengine.so: doomgeneric, engine.c's
 * platform layer, sound.c), which the plugin loads with dlopen and runs in a
 * thread: doom_run(). Both are built together.
 *
 * The layouts are here: the plugin draws them and finds what the fingers touch in
 * them (touch.c), for the engine, which gets the keys held. Upright, in the screen's
 * coordinates (below phone-ui's status bar and header); sideways (the display turned,
 * full screen), in LVGL's 1440 x 720.
 */

#ifndef DOOM_HOST_H
#define DOOM_HOST_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define DOOM_W 320
#define DOOM_H 200

struct ui_sound; /* phone-ui's (ui.h) */

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
	bool round;  /* drawn as a rounded square (squircle-like), else a pill if low */
};

struct doom_pad {
	int x, y, r; /* center, radius; r 0: none */
};

/* FIRE is the last button: a finger landing on it also turns. Upright, the moving
 * and turning sides have a pad each, drawn: a finger landing in the moving one
 * steers from its center (else from where it landed); sideways, their halves of
 * the screen are only written on. */
struct doom_layout {
	int game_x, game_y, scale;  /* the game, DOOM_W x DOOM_H times scale */
	int middle;                 /* moving on its left, turning on its right */
	struct doom_pad move, turn;
	int hint_y;                 /* MOVE and TURN written at this height where x is not 0 */
	int hint_move_x, hint_turn_x;
	unsigned count;
	struct doom_button buttons[10];
};

/* A pad's knob, which follows the thumb within the pad */
#define DOOM_KNOB_R 44

#define DOOM_BODY_Y 152 /* upright: phone-ui's status bar and header (UI_STATUS_H + UI_HEADER_H) */

static const struct doom_layout doom_layouts[2] = {
	/* Upright, as a handheld: the game below the header, the buttons Doom needs in
	 * its menus under it, then USE, the weapons and FIRE, then the two pads for the
	 * thumbs. USE and FIRE are as large and over the pads, the weapons stacked
	 * between them; 20 px apart (the rectangles have 4 px of gap drawn inside) */
	{ 40, DOOM_BODY_Y + 16, 2, 360, { 180, 1195, 165 }, { 540, 1195, 165 }, 0, 0, 0, 9, {
		{ 4, 584, 140, 72, "MENU", DOOM_KEY_ESCAPE, false },
		{ 148, 584, 140, 72, "ENTER", DOOM_KEY_ENTER, false },
		{ 292, 584, 136, 72, "Y", 'y', false },
		{ 432, 584, 140, 72, "N", 'n', false },
		{ 576, 584, 140, 72, "MAP", DOOM_KEY_TAB, false },
		{ 66, 736, 228, 228, "USE", DOOM_KEY_USE, true },
		{ 306, 736, 108, 108, "<", DOOM_KEY_PREV_WEAPON, true },
		{ 306, 856, 108, 108, ">", DOOM_KEY_NEXT_WEAPON, true },
		{ 426, 736, 228, 228, "FIRE", DOOM_KEY_FIRE, true },
	} },
	/* Sideways, as Doom ports have it: the game 3 times its size in the middle, the
	 * MOVE pad on the left, the buttons in the right column as upright (20 px apart),
	 * FIRE at the bottom; turning on the right of the middle (no room for a pad) */
	{ 240, 60, 3, 720, { 120, 470, 112 }, { 0 }, 690, 0, 960, 10, {
		{ 8, 8, 84, 72, "<", DOOM_KEY_BACK, false },
		{ 96, 8, 136, 72, "MENU", DOOM_KEY_ESCAPE, false },
		{ 240, 0, 320, 60, "ENTER", DOOM_KEY_ENTER, false },
		{ 560, 0, 320, 60, "Y", 'y', false },
		{ 880, 0, 320, 60, "N", 'n', false },
		{ 1208, 8, 224, 72, "MAP", DOOM_KEY_TAB, false },
		{ 1208, 96, 108, 100, "<", DOOM_KEY_PREV_WEAPON, true },
		{ 1324, 96, 108, 100, ">", DOOM_KEY_NEXT_WEAPON, true },
		{ 1208, 208, 224, 236, "USE", DOOM_KEY_USE, true },
		{ 1208, 456, 224, 256, "FIRE", DOOM_KEY_FIRE, true },
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
	/* phone-ui's sound output (ui_sound_*()), for the engine's sound thread: a write blocks
	 * while its buffer is full, false without a device; released while paused */
	struct ui_sound *sound;
	bool (*sound_write)(struct ui_sound *s, const int16_t *frames, unsigned count);
	void (*sound_release)(struct ui_sound *s);
	/* The fingers, read by the plugin (touch.c) in the engine's thread: after each report,
	 * held() with the keys they hold (DOOM_KEY_*) and the px dragged sideways since (turning).
	 * Dropped while paused: what was reported meanwhile, nothing held. */
	void (*touch_read)(void (*held)(const bool keys[256], int turn));
	void (*touch_drop)(void);

	/* Set by the plugin (atomic) */
	int orient;            /* of the layout shown: touches are found in it */
	bool paused;           /* the app is not shown: Doom stops, the sound device is let go */
	bool quit;             /* Doom to end (another WAD picked): doom_run() returns */

	/* Set by the engine */
	pthread_mutex_t lock;  /* frame, frame_seq */
	uint32_t frame[DOOM_W * DOOM_H]; /* BGRA, as LVGL's textures */
	unsigned frame_seq;
	bool exited;           /* Doom quit (its menu, or an error): doom_run() returned */

	/* Set by touch_read() and touch_drop() (atomic) */
	unsigned pressed;      /* buttons held, a bit per index in the layout */
	int knob[2][2];        /* the pads' (move, turn) knobs: x, y from their center */
};

/* The engine's entry point: runs Doom until it quits */
typedef void doom_run_fn(struct doom_host *host);
doom_run_fn doom_run;

/* The engine's sound (sound.c), for engine.c: the output, before Doom starts; the app
 * not shown, or shown again; Doom quit, the library about to be unloaded */
void doom_sound_host(struct doom_host *host);
void doom_sound_pause(bool paused);
void doom_sound_stop(void);

#endif
