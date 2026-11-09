/*
 * Doom (../../../../apps/doom), a plugin app: doom.so built for the host (DOOM_APP), its
 * engine (lib/doomengine.so, loaded when a game starts) fake_doom/doomengine.c, installed
 * as `just app doom` installs it. Its list of games (WADs made here: games or not, their
 * names and title screens), and playing: the engine's frames and controls shown, paused
 * when left, ended for another game or from its menu, turned sideways.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui.h"
#include "ui_test.h"

#include "../../../../apps/doom/doom.h"
#include "fake_doom/fake_doom.h"

#define APP "/var/lib/apps/doom"
#define WADS "/var/lib/doom/wads"
#define BODY_Y (UI_STATUS_H + UI_HEADER_H)

struct fake_doom fake_doom;

static bool copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
	char buf[65536];
	size_t n;
	bool ok = in && out;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if (in)
		fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	return ok;
}

TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/lib/doomengine.so", "") || !copy(DOOM_APP "/app.json", APP "/app.json") ||
	    !copy(DOOM_APP "/doom.so", APP "/doom.so") ||
	    !copy(DOOM_APP "/lib/doomengine.so", APP "/lib/doomengine.so"))
		TEST_FAIL("cannot install %s", DOOM_APP);
	fake_doom.color = 0x8040c0;
}

/* WADs */

struct lump {
	const char *name;
	const uint8_t *data;
	size_t size;
};

static void le32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = v >> (8 * i);
}

/* A WAD ("IWAD" or "PWAD") of these lumps */
static bool wad_write(const char *path, const char *magic, const struct lump *lumps, unsigned count)
{
	size_t size = 12;
	for (unsigned i = 0; i < count; i++)
		size += lumps[i].size;
	uint8_t *wad = calloc(1, size + 16 * count), *p = wad + 12;
	memcpy(wad, magic, 4);
	le32(wad + 4, count);
	le32(wad + 8, size);
	for (unsigned i = 0; i < count; i++) {
		uint8_t *e = wad + size + 16 * i;
		le32(e, p - wad);
		le32(e + 4, lumps[i].size);
		strncpy((char *)e + 8, lumps[i].name, 8);
		memcpy(p, lumps[i].data, lumps[i].size);
		p += lumps[i].size;
	}
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(wad, 1, size + 16 * count, f) == size + 16 * count;
	if (f && fclose(f) != 0)
		ok = false;
	free(wad);
	return ok;
}

/* A file of these bytes (NULs included) */
static bool bytes_write(const char *path, const char *data, size_t size)
{
	if (!ui_test_write(path, ""))
		return false;
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(data, 1, size, f) == size;
	if (f && fclose(f) != 0)
		ok = false;
	return ok;
}

/* A game without a title screen */
static bool game_write(const char *path)
{
	static const uint8_t map[] = { 0 };
	const struct lump lumps[] = { { "MAP01", map, sizeof(map) } };
	return ui_test_write(path, "") && wad_write(path, "IWAD", lumps, 1);
}

/* PLAYPAL with color 1 `rgb`, and TITLEPIC all of color 1: as Doom's pictures
 * (patches: a column offset each, a post of 200 pixels) or a raw 320 x 200 block */
static bool titled_write(const char *path, uint32_t rgb, bool raw)
{
	static uint8_t pal[768], pic[8 + 4 * 320 + 320 * 205];
	pal[3] = rgb >> 16;
	pal[4] = rgb >> 8;
	pal[5] = rgb;
	size_t size;
	if (raw) {
		memset(pic, 1, 320 * 200);
		size = 320 * 200;
	} else {
		memset(pic, 0, sizeof(pic));
		pic[0] = 320 & 0xff, pic[1] = 320 >> 8, pic[2] = 200;
		uint8_t *post = pic + 8 + 4 * 320;
		for (int x = 0; x < 320; x++, post += 205) {
			le32(pic + 8 + 4 * x, post - pic);
			post[0] = 0;   /* top */
			post[1] = 200; /* length */
			memset(post + 3, 1, 200);
			post[204] = 0xff; /* the column's end */
		}
		size = post - pic;
	}
	const struct lump lumps[] = { { "PLAYPAL", pal, sizeof(pal) }, { "TITLEPIC", pic, size } };
	return ui_test_write(path, "") && wad_write(path, "IWAD", lumps, 2);
}

/* The middle of the title screen of `name`'s tile */
static lv_color_t title_pixel(const char *name)
{
	lv_obj_t *label = ui_test_find(name);
	if (!label)
		return lv_color_black();
	lv_area_t a;
	lv_obj_t *tile = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(label)));
	lv_obj_get_coords(tile, &a);
	return ui_test_pixel((a.x1 + a.x2) / 2, a.y1 + 60);
}

static bool same(lv_color_t c, uint32_t rgb)
{
	return abs(c.red - (int)(rgb >> 16 & 0xff)) <= 8 && abs(c.green - (int)(rgb >> 8 & 0xff)) <= 8 &&
	       abs(c.blue - (int)(rgb & 0xff)) <= 8;
}

#define CHECK_COLOR(c, rgb)                                                                \
	do {                                                                               \
		lv_color_t c_ = (c);                                                       \
		if (!same(c_, rgb))                                                        \
			TEST_FAIL("%s is #%02x%02x%02x, not #%06x", #c, c_.red, c_.green, c_.blue, \
			          (unsigned)(rgb));                                        \
	} while (0)

/* The list */

/* Without IWADs: where to copy them, its text kept (a static label) */
TEST(no_games)
{
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_shows("No games"));
	CHECK(ui_test_shows("Copy IWADs (doom2.wad, freedoom1.wad...) to /var/lib/doom/wads"));
}

/* The games by their names, sorted; those of other games on Doom's engine, mods and
 * other files left out; the shareware Doom next to the plugin */
TEST(games_listed)
{
	static const uint8_t data[] = { 0 };
	const struct lump lump = { "MAP01", data, 1 };
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(game_write(WADS "/MyGame.WAD"));
	CHECK(game_write(WADS "/heretic.wad"));
	CHECK(game_write(WADS "/.hidden.wad"));
	CHECK(game_write(WADS "/game.txt"));
	CHECK(ui_test_write(WADS "/chex.wad", "") && wad_write(WADS "/chex.wad", "PWAD", &lump, 1));
	CHECK(ui_test_write(WADS "/mod.wad", "") && wad_write(WADS "/mod.wad", "PWAD", &lump, 1));
	CHECK(ui_test_write(WADS "/short.wad", "IW"));
	CHECK(game_write(APP "/doom1.wad"));
	CHECK(ui_test_open("Doom"));
	const char *shown[] = { "Chex Quest", "Doom (shareware)", "Doom II", "MyGame" };
	int y = -1;
	for (unsigned i = 0; i < sizeof(shown) / sizeof(*shown); i++) {
		lv_obj_t *label = ui_test_find(shown[i]);
		CHECK(label);
		if (!label)
			continue;
		lv_area_t a;
		lv_obj_get_coords(label, &a);
		CHECK(a.y1 >= y); /* two to a row, in order */
		y = a.y1;
	}
	CHECK(!ui_test_shows_part("heretic"));
	CHECK(!ui_test_shows_part("mod"));
	CHECK(!ui_test_shows_part("hidden"));
	CHECK(!ui_test_shows_part("short"));
	CHECK(!ui_test_shows("No games"));
	CHECK(!ui_test_shows("Paused"));
}

/* Each game's title screen in its palette: Doom's picture format, a raw one, none */
TEST(title_screens)
{
	CHECK(titled_write(WADS "/doom2.wad", 0xc02020, false));
	CHECK(titled_write(WADS "/tnt.wad", 0x20a040, true));
	CHECK(game_write(WADS "/plutonia.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK_COLOR(title_pixel("Doom II"), 0xc02020);
	CHECK_COLOR(title_pixel("Final Doom: TNT"), 0x20a040);
	CHECK_COLOR(title_pixel("Final Doom: Plutonia"), 0x000000);
}

/* Two to a row upright, four sideways: the third game on the first row then */
static int32_t label_y(const char *text)
{
	lv_area_t a = { 0 };
	lv_obj_t *label = ui_test_find(text);
	if (label)
		lv_obj_get_coords(label, &a);
	return a.y1;
}

TEST(list_sideways)
{
	CHECK(game_write(WADS "/tnt.wad"));
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(game_write(WADS "/plutonia.wad"));
	ui_test_sensors(true, false);
	CHECK(ui_test_open("Doom"));
	CHECK(label_y("Final Doom: TNT") > label_y("Doom II"));
	const float right_up[3] = { 1, 0, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(right_up, field, 1);
	ui_test_wait(1000);
	CHECK_INT(lv_display_get_rotation(lv_display_get_default()), LV_DISPLAY_ROTATION_90);
	CHECK_INT(label_y("Final Doom: TNT"), label_y("Doom II"));
}

/* Playing. The engine draws in real time, the tests' time runs faster: what it
 * does is waited for, its frames counted. */

static unsigned frames_mark;

static bool drawn_since_mark(void)
{
	return __atomic_load_n(&fake_doom.frames, __ATOMIC_ACQUIRE) >= frames_mark + 3;
}

/* A few of its frames more, then what they hold drawn */
static bool engine_frames(void)
{
	frames_mark = __atomic_load_n(&fake_doom.frames, __ATOMIC_ACQUIRE);
	if (!ui_test_wait_until(drawn_since_mark, 5000))
		return false;
	ui_test_wait(50);
	return true;
}

static int orient_wanted;

static bool orient_told(void)
{
	return __atomic_load_n(&fake_doom.orient, __ATOMIC_ACQUIRE) == orient_wanted;
}

/* Held so, the display turns; the engine told */
static bool held(float x, float y, int orient)
{
	const float up[3] = { x, y, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000);
	orient_wanted = orient;
	return ui_test_wait_until(orient_told, 5000);
}

static bool paused(void)
{
	return __atomic_load_n(&fake_doom.paused, __ATOMIC_ACQUIRE);
}

static bool playing(void)
{
	return !paused();
}

/* Doom's frames shown, twice their size below the header */
static bool game_shows(uint32_t rgb)
{
	return engine_frames() && same(ui_test_pixel(360, BODY_Y + 16 + 200), rgb);
}

/* Its IWAD, its own folder for settings and saves, its frames */
TEST(play)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(ui_test_shows("Doom II")); /* the header */
	CHECK(game_shows(0x8040c0));
	CHECK_INT(fake_doom.runs, 1);
	CHECK_STR(fake_doom.iwad, WADS "/doom2.wad");
	CHECK_STR(fake_doom.configdir, "/var/lib/app-data/doom/doom2/.");
	CHECK_INT(fake_doom.orient, DOOM_PORTRAIT);
	CHECK(ui_test_shows("FIRE"));
	CHECK(ui_test_shows("MOVE"));
	/* The CPU at its lowest while the engine keeps up (cpu.c: boosted once it does not) */
	ui_test_wait(3000);
	long khz = 0;
	CHECK(ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", &khz));
	CHECK_INT(khz, 648000);
}

/* The fingers (the touchscreen read beside LVGL, ui_touch_*()), several at once: FIRE
 * held fires and turns as it is dragged, the moving pad steers from its center (its knob
 * following); Doom told the keys, the controls drawn so */

static int key_wanted;

static bool key_held(void)
{
	return __atomic_load_n(&fake_doom.keys[key_wanted], __ATOMIC_ACQUIRE);
}

static bool key_let_go(void)
{
	return !key_held();
}

TEST(controls_follow_the_fingers)
{
	ui_test_touchscreen();
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	const struct doom_button *fire = &doom_layouts[0].buttons[8];
	const struct doom_pad *move = &doom_layouts[0].move;
	int fire_x = fire->x + 20, fire_y = fire->y + fire->h / 2, knob_x = move->x + DOOM_KNOB_R + 20;
	CHECK_COLOR(ui_test_pixel(fire_x, fire_y), 0x2a2a2a);
	CHECK_COLOR(ui_test_pixel(knob_x, move->y), 0x1a1a1a);
	ui_test_touch(0, fire->x + fire->w / 2, fire_y);
	key_wanted = DOOM_KEY_FIRE;
	CHECK(ui_test_wait_until(key_held, 3000));
	ui_test_touch(0, fire->x + fire->w / 2 + 50, fire_y); /* dragged: turns */
	ui_test_touch(1, move->x, move->y);
	ui_test_touch(1, move->x + 2 * DOOM_KNOB_R, move->y); /* to the right: strafes */
	key_wanted = DOOM_KEY_STRAFE_R;
	CHECK(ui_test_wait_until(key_held, 3000));
	CHECK_INT(fake_doom.turn, 50);
	CHECK(fake_doom.keys[DOOM_KEY_FIRE]);
	CHECK(!fake_doom.keys[DOOM_KEY_UPARROW]);
	CHECK(!fake_doom.keys[DOOM_KEY_RSHIFT]);
	CHECK(engine_frames());
	CHECK_COLOR(ui_test_pixel(fire_x, fire_y), 0x6a1a12);
	CHECK_COLOR(ui_test_pixel(knob_x, move->y), 0x2a2a2a);
	ui_test_touch_up(0);
	ui_test_touch_up(1);
	CHECK(ui_test_wait_until(key_let_go, 3000));
	key_wanted = DOOM_KEY_FIRE;
	CHECK(ui_test_wait_until(key_let_go, 3000));
	CHECK(engine_frames());
	CHECK_COLOR(ui_test_pixel(fire_x, fire_y), 0x2a2a2a);
	CHECK_COLOR(ui_test_pixel(knob_x, move->y), 0x1a1a1a);
}

/* The keys Doom was told are held: exactly these (-1 ends the list) */
static const int *keys_wanted;

static bool keys_are(void)
{
	for (int k = 1; k < 256; k++) {
		bool want = false;
		for (const int *w = keys_wanted; *w >= 0; w++)
			want |= *w == k;
		if (__atomic_load_n(&fake_doom.keys[k], __ATOMIC_ACQUIRE) != want)
			return false;
	}
	return true;
}

#define KEYS_BECOME(...) (keys_wanted = (const int[]){ __VA_ARGS__, -1 }, ui_test_wait_until(keys_are, 3000))

/* A button of the controller pressed: Doom told `keys` held, then none once let go */
#define BUTTON_IS(code, ...)                                                       \
	do {                                                                           \
		ui_test_gamepad_button(code, true);                                        \
		CHECK(KEYS_BECOME(__VA_ARGS__));                                           \
		ui_test_gamepad_button(code, false);                                       \
		CHECK(KEYS_BECOME(-1));                                                    \
	} while (0)

/* A game controller (an 8BitDo M30's buttons in brackets): the D-pad walks and turns (and
 * moves in the menus), bottom (A) fires and picks in the menus (Enter), right (B) uses and
 * answers yes, left (X) runs, L1 and R1 (Z, C) strafe, L2, R2 and top (Y) change weapons,
 * Start is the menu, Select the map; with the fingers, the screen's buttons lit by it */
TEST(game_controller)
{
	ui_test_touchscreen();
	ui_test_gamepad("Wireless Controller", false);
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));

	BUTTON_IS(BTN_SOUTH, DOOM_KEY_FIRE, DOOM_KEY_ENTER);
	BUTTON_IS(BTN_EAST, DOOM_KEY_USE, 'y');
	BUTTON_IS(BTN_WEST, DOOM_KEY_RSHIFT);
	BUTTON_IS(BTN_TL, DOOM_KEY_STRAFE_L);
	BUTTON_IS(BTN_TR, DOOM_KEY_STRAFE_R);
	BUTTON_IS(BTN_TL2, DOOM_KEY_PREV_WEAPON);
	BUTTON_IS(BTN_TR2, DOOM_KEY_NEXT_WEAPON);
	BUTTON_IS(BTN_NORTH, DOOM_KEY_NEXT_WEAPON);
	BUTTON_IS(BTN_START, DOOM_KEY_ESCAPE);
	BUTTON_IS(BTN_SELECT, DOOM_KEY_TAB);

	ui_test_gamepad_axis(ABS_Y, 0);
	CHECK(KEYS_BECOME(DOOM_KEY_UPARROW));
	ui_test_gamepad_axis(ABS_X, 0);
	CHECK(KEYS_BECOME(DOOM_KEY_UPARROW, DOOM_KEY_LEFTARROW));
	ui_test_gamepad_axis(ABS_Y, 128);
	ui_test_gamepad_axis(ABS_X, 255);
	CHECK(KEYS_BECOME(DOOM_KEY_RIGHTARROW));
	ui_test_gamepad_axis(ABS_X, 128);
	CHECK(KEYS_BECOME(-1));

	/* FIRE drawn pressed; a finger strafing on the moving pad meanwhile */
	const struct doom_button *fire = &doom_layouts[0].buttons[8];
	const struct doom_pad *move = &doom_layouts[0].move;
	int fire_x = fire->x + 20, fire_y = fire->y + fire->h / 2;
	ui_test_gamepad_button(BTN_SOUTH, true);
	ui_test_touch(1, move->x, move->y);
	ui_test_touch(1, move->x + 2 * DOOM_KNOB_R, move->y);
	CHECK(KEYS_BECOME(DOOM_KEY_FIRE, DOOM_KEY_ENTER, DOOM_KEY_STRAFE_R));
	CHECK(engine_frames());
	CHECK_COLOR(ui_test_pixel(fire_x, fire_y), 0x6a1a12);
	ui_test_touch_up(1);
	CHECK(KEYS_BECOME(DOOM_KEY_FIRE, DOOM_KEY_ENTER));
	ui_test_gamepad_button(BTN_SOUTH, false);
	CHECK(KEYS_BECOME(-1));
	CHECK(engine_frames());
	CHECK_COLOR(ui_test_pixel(fire_x, fire_y), 0x2a2a2a);
}

/* The controller's Home leaves the game as the back arrow does: paused, back in the list;
 * it is no key of Doom's */
TEST(game_controller_home_leaves)
{
	ui_test_gamepad("Wireless Controller", false);
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	ui_test_gamepad_button(BTN_MODE, true);
	CHECK(ui_test_wait_for("Paused", 3000));
	CHECK(ui_test_wait_until(paused, 2000));
	CHECK(KEYS_BECOME(-1));
	ui_test_gamepad_button(BTN_MODE, false);
	CHECK(ui_test_tap_text("Doom II")); /* again: Home let go meanwhile, it stays */
	CHECK(ui_test_wait_until(playing, 2000));
	CHECK(engine_frames());
	CHECK(ui_test_shows("FIRE"));
}

/* Connected once Doom plays, it is found; disconnected while a button is held, Doom is told
 * it is let go */
TEST(game_controller_connected_while_playing)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	ui_test_gamepad("Wireless Controller", false);
	ui_test_gamepad_button(BTN_SOUTH, true);
	CHECK(KEYS_BECOME(DOOM_KEY_FIRE, DOOM_KEY_ENTER));
	ui_test_gamepad_gone();
	CHECK(KEYS_BECOME(-1));
}

/* Left, Doom waits, the list says which game; opened again, it goes on */
TEST(left_paused_then_resumed)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(game_write(WADS "/tnt.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	CHECK(ui_test_back());
	CHECK(ui_test_wait_until(paused, 2000));
	CHECK(ui_test_shows("Paused"));
	lv_area_t paused_at, game;
	lv_obj_get_coords(ui_test_find("Paused"), &paused_at);
	lv_obj_get_coords(ui_test_find("Doom II"), &game);
	CHECK(paused_at.y1 < game.y2 && paused_at.y2 > game.y1); /* on its line */
	CHECK(ui_test_tap_text("Doom II"));
	fake_doom.color = 0x30c0c0;
	CHECK(game_shows(0x30c0c0));
	CHECK(!paused());
	CHECK_INT(fake_doom.runs, 1);
}

/* Another game picked: the paused one ends first */
TEST(another_game_ends_the_first)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(game_write(WADS "/tnt.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Final Doom: TNT"));
	CHECK(fake_doom.told_to_quit || fake_doom.runs == 2);
	CHECK(game_shows(0x8040c0));
	CHECK_INT(fake_doom.runs, 2);
	CHECK_STR(fake_doom.iwad, WADS "/tnt.wad");
	CHECK_STR(fake_doom.configdir, "/var/lib/app-data/doom/tnt/.");
}

/* Quit from Doom's menu: back to the list, nothing paused; the game starts again */
TEST(quit_from_the_menu)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	__atomic_store_n(&fake_doom.quit, true, __ATOMIC_RELEASE);
	CHECK(ui_test_wait_for("Copy IWADs (doom2.wad, freedoom1.wad...) to /var/lib/doom/wads", 5000));
	CHECK(!ui_test_shows("Paused"));
	__atomic_store_n(&fake_doom.quit, false, __ATOMIC_RELEASE);
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	CHECK_INT(fake_doom.runs, 2);
}

/* Turned sideways: the layout's, the engine told; its back button leaves */
TEST(sideways)
{
	CHECK(game_write(WADS "/doom2.wad"));
	ui_test_sensors(true, false);
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	CHECK(held(1, 0, DOOM_LEFT)); /* right edge up: the top to the left */
	CHECK(!ui_test_shows("Doom II")); /* no header */
	CHECK(ui_test_shows("MOVE"));
	CHECK(ui_test_shows("TURN")); /* written on its half, no pad */
	CHECK(held(-1, 0, DOOM_RIGHT));
	CHECK(held(0, 1, DOOM_PORTRAIT));
	CHECK(ui_test_shows("Doom II"));
	CHECK(held(1, 0, DOOM_LEFT));
	CHECK(ui_test_tap_text(LV_SYMBOL_LEFT));
	CHECK(ui_test_wait_until(paused, 2000));
	CHECK(ui_test_shows("Paused"));
}

/* Without its engine: said so */
TEST(no_engine)
{
	CHECK(remove(APP "/lib/doomengine.so") == 0);
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(ui_test_shows("Cannot start Doom"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("Paused"));
}

/* More games than the list first has room for; leaving the app frees them (ASan) */
TEST(many_games)
{
	char path[64];
	for (int i = 1; i <= 12; i++) {
		snprintf(path, sizeof(path), WADS "/game%02d.wad", i);
		CHECK(game_write(path));
	}
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_shows("game01"));
	CHECK(ui_test_shows("game12"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("game01"));
}

/* Covered by the lock screen (the phone put to sleep, or woken) or the quick settings, Doom
 * waits as when left: no sound, no touches; uncovered, it goes on */
TEST(paused_while_covered)
{
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	ui_lock();
	CHECK(ui_test_wait_until(paused, 2000));
	ui_test_swipe(360, 1200, 360, 400, 400);
	CHECK(!ui_locked());
	CHECK(ui_test_wait_until(playing, 2000));
	ui_test_tap_at(360, UI_STATUS_H / 2); /* the quick settings */
	ui_test_settle();
	CHECK(ui_shade_shown());
	CHECK(ui_test_wait_until(paused, 2000));
	ui_shade_close();
	CHECK(ui_test_wait_until(playing, 2000));
	CHECK_INT(fake_doom.runs, 1);
}

/* USE held by a finger, drawn pressed; turning from the turning pad, its knob kept in the
 * pad however far the thumb goes; the moving pad pushed far: walking and running */
TEST(use_turn_pad_and_running)
{
	ui_test_touchscreen();
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	const struct doom_button *use = &doom_layouts[0].buttons[5];
	const struct doom_pad *move = &doom_layouts[0].move, *turn = &doom_layouts[0].turn;
	int max = turn->r - DOOM_KNOB_R;
	ui_test_touch(0, use->x + use->w / 2, use->y + use->h / 2);
	key_wanted = DOOM_KEY_USE;
	CHECK(ui_test_wait_until(key_held, 3000));
	ui_test_touch(1, turn->x, turn->y);
	ui_test_touch(1, turn->x + 200, turn->y); /* past the pad's edge */
	ui_test_touch(2, move->x, move->y);
	ui_test_touch(2, move->x, move->y - 150); /* up, far */
	key_wanted = DOOM_KEY_RSHIFT;
	CHECK(ui_test_wait_until(key_held, 3000));
	CHECK(fake_doom.keys[DOOM_KEY_UPARROW]);
	CHECK(fake_doom.keys[DOOM_KEY_USE]);
	CHECK(!fake_doom.keys[DOOM_KEY_FIRE]);
	CHECK_INT(fake_doom.turn, 200);
	CHECK(engine_frames());
	CHECK_COLOR(ui_test_pixel(use->x + 20, use->y + use->h / 2), 0x6a1a12);
	/* The knobs at the pads' edges: the turning one right, the moving one up */
	CHECK_COLOR(ui_test_pixel(turn->x + max, turn->y), 0x2a2a2a);
	CHECK_COLOR(ui_test_pixel(turn->x, turn->y), 0x1a1a1a);
	CHECK_COLOR(ui_test_pixel(move->x, move->y - (move->r - DOOM_KNOB_R)), 0x2a2a2a);
	CHECK_COLOR(ui_test_pixel(move->x, move->y), 0x1a1a1a);
	ui_test_touch_up(0);
	ui_test_touch_up(1);
	ui_test_touch_up(2);
	CHECK(ui_test_wait_until(key_let_go, 3000));
}

/* Sideways, the moving pad steers too (the panel's coordinates turned: the layout's x, y
 * is the panel's 719 - y, x); the turning side has no pad */
TEST(sideways_stick)
{
	ui_test_touchscreen();
	CHECK(game_write(WADS "/doom2.wad"));
	ui_test_sensors(true, false);
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(game_shows(0x8040c0));
	CHECK(held(1, 0, DOOM_LEFT));
	const struct doom_pad *move = &doom_layouts[1].move;
	ui_test_touch(0, 719 - move->y, move->x);
	ui_test_touch(0, 719 - (move->y - 60), move->x); /* up */
	key_wanted = DOOM_KEY_UPARROW;
	CHECK(ui_test_wait_until(key_held, 3000));
	CHECK(!fake_doom.keys[DOOM_KEY_RSHIFT]);
	ui_test_touch_up(0);
	CHECK(ui_test_wait_until(key_let_go, 3000));
}

/* WADs whose title screen cannot be read: listed, without one */
TEST(broken_title_screens)
{
	static const uint8_t bad_patch[8] = { 0 }; /* 0 x 0 */
	static uint8_t pal[768];
	const struct lump lumps[] = { { "PLAYPAL", pal, sizeof(pal) }, { "TITLEPIC", bad_patch, sizeof(bad_patch) } };
	CHECK(bytes_write(WADS "/doom2.wad", "IWAD\1\0\0\0", 8)); /* its header cut short */
	CHECK(bytes_write(WADS "/tnt.wad", "IWAD\0\0\0\0\14\0\0\0", 12)); /* no lumps */
	CHECK(bytes_write(WADS "/plutonia.wad", "IWAD\1\0\0\0\0\0\1\0", 12)); /* its directory past the end */
	CHECK(ui_test_write(WADS "/doomu.wad", "") && wad_write(WADS "/doomu.wad", "IWAD", lumps, 2));
	CHECK(ui_test_open("Doom"));
	CHECK_COLOR(title_pixel("Doom II"), 0x000000);
	CHECK_COLOR(title_pixel("Final Doom: TNT"), 0x000000);
	CHECK_COLOR(title_pixel("Final Doom: Plutonia"), 0x000000);
	CHECK_COLOR(title_pixel("The Ultimate Doom"), 0x000000);
}

/* An engine without Doom in it (another library in its place): said so, as without one */
TEST(engine_without_doom)
{
	CHECK(copy(DOOM_APP "/doom.so", APP "/lib/doomengine.so"));
	CHECK(game_write(WADS "/doom2.wad"));
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(ui_test_shows("Cannot start Doom"));
}
