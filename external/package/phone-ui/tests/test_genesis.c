/*
 * Genesis (../../../../apps/genesis), a plugin app: genesis.so built for the host
 * (GENESIS_APP), its core (lib/genesis_plus_gx_libretro.so, loaded when a game starts)
 * fake_genesis/libretro.c, installed as `just app genesis` installs it. Its list of games
 * (discs' tracks and playlists' discs left out), and playing: the core's frames at the
 * mode's size, saves when left, resumed, restarted, turned sideways.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test.h"
#include "ui.h"
#include "ui_test.h"

#include "fake_genesis/fake_genesis.h"
#include "libretro.h"

#define APP "/var/lib/apps/genesis"
#define ROMS "/var/lib/genesis/roms"
#define SAVES "/var/lib/genesis/saves"
#define BODY_Y (UI_STATUS_H + UI_HEADER_H)

struct fake_genesis fake_genesis;

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
	if (!ui_test_write(APP "/lib/" "genesis_plus_gx_libretro.so", "") ||
	    !copy(GENESIS_APP "/app.json", APP "/app.json") || !copy(GENESIS_APP "/genesis.so", APP "/genesis.so") ||
	    !copy(GENESIS_APP "/lib/genesis_plus_gx_libretro.so", APP "/lib/genesis_plus_gx_libretro.so"))
		TEST_FAIL("cannot install %s", GENESIS_APP);
	fake_genesis.color = 0x8040c0;
}

/* A file's contents (malloc'd, NULL if none) */
static char *file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *data = calloc(1, 4097);
	*size = fread(data, 1, 4096, f);
	fclose(f);
	return data;
}

static bool same(lv_color_t c, uint32_t rgb)
{
	return abs(c.red - (int)(rgb >> 16 & 0xff)) <= 8 && abs(c.green - (int)(rgb >> 8 & 0xff)) <= 8 &&
	       abs(c.blue - (int)(rgb & 0xff)) <= 8;
}

/* The list */

/* Without ROMs: where to copy them, its text kept (a static label) */
TEST(no_games)
{
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_shows("No games"));
	CHECK(ui_test_shows("Copy ROMs to /var/lib/genesis/roms"));
}

/* Pulled down, the games are read again: one copied meanwhile shows */
TEST(pull_refresh)
{
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_shows("No games"));
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_pull_down();
	CHECK(ui_test_shows("Sonic"));
	CHECK(!ui_test_shows("No games"));
	CHECK(ui_test_write(ROMS "/Columns.md", "rom"));
	ui_test_pull_down();
	CHECK(ui_test_shows("Columns"));
	CHECK(ui_test_shows("Sonic"));
}

/* Cartridges and discs by their names, sorted whatever the case; a disc's tracks
 * (its .cue's FILEs, quoted or not) and a playlist's discs are not games of their own;
 * other files, hidden ones left out */
TEST(games_listed)
{
	CHECK(ui_test_write(ROMS "/Streets.gen", "rom"));
	CHECK(ui_test_write(ROMS "/alex.sms", "rom"));
	CHECK(ui_test_write(ROMS "/Sonic CD.cue", "REM a disc\nFILE \"Sonic CD (Track 1).bin\" BINARY\n"
	                                          "  TRACK 01 MODE1/2352\n\tFILE track2.bin BINARY\n"));
	CHECK(ui_test_write(ROMS "/Sonic CD (Track 1).bin", "track"));
	CHECK(ui_test_write(ROMS "/track2.bin", "track"));
	CHECK(ui_test_write(ROMS "/Snatcher.m3u", "# two discs\nSnatcher (Disc 1).chd\ndiscs/Snatcher (Disc 2).chd\n"));
	CHECK(ui_test_write(ROMS "/Snatcher (Disc 1).chd", "disc"));
	CHECK(ui_test_write(ROMS "/Snatcher (Disc 2).chd", "disc"));
	CHECK(ui_test_write(ROMS "/readme.txt", "text"));
	CHECK(ui_test_write(ROMS "/.Hidden.md", "rom"));
	CHECK(ui_test_open("Genesis"));
	const char *shown[] = { "alex", "Snatcher", "Sonic CD", "Streets" };
	int32_t y = -1;
	for (unsigned i = 0; i < sizeof(shown) / sizeof(*shown); i++) {
		lv_obj_t *label = ui_test_find(shown[i]);
		CHECK(label);
		if (!label)
			continue;
		lv_area_t a;
		lv_obj_get_coords(label, &a);
		CHECK(a.y1 > y);
		y = a.y1;
	}
	CHECK(!ui_test_shows_part("Track"));
	CHECK(!ui_test_shows_part("track"));
	CHECK(!ui_test_shows_part("Disc"));
	CHECK(!ui_test_shows_part("readme"));
	CHECK(!ui_test_shows_part("Hidden"));
}

/* Playing. The core runs in real time, the tests' time faster: what it does is
 * waited for, its frames counted. */

static unsigned frames_mark;

static bool run_since_mark(void)
{
	return __atomic_load_n(&fake_genesis.frames, __ATOMIC_ACQUIRE) >= frames_mark + 3;
}

/* A few of its frames more, then what they hold drawn */
static bool core_frames(void)
{
	frames_mark = __atomic_load_n(&fake_genesis.frames, __ATOMIC_ACQUIRE);
	if (!ui_test_wait_until(run_since_mark, 5000))
		return false;
	ui_test_wait(50);
	return true;
}

/* The game's view: its place and size on the display */
static lv_area_t view(void)
{
	lv_area_t a = { 0 };
	lv_obj_t *obj = ui_test_find_widget(&lv_3dtexture_class);
	if (obj)
		lv_obj_get_coords(obj, &a);
	return a;
}

static bool playing(const char *name)
{
	return ui_test_open("Genesis") && ui_test_tap_text(name) && core_frames();
}

/* The ROM loaded with the front end's folders, its frames twice their size below
 * the header */
TEST(play)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	CHECK(ui_test_shows("Sonic")); /* the header */
	CHECK_STR(fake_genesis.path, ROMS "/Sonic.md");
	CHECK_STR(fake_genesis.system_dir, "/var/lib/genesis");
	CHECK_STR(fake_genesis.save_dir, SAVES);
	CHECK(fake_genesis.rgb565);
	CHECK(same(ui_test_pixel(360, BODY_Y + 16 + 200), 0x8040c0));
	lv_area_t a = view();
	CHECK_INT(a.x1, 40);
	CHECK_INT(a.y1, BODY_Y + 16);
	CHECK_INT(lv_area_get_width(&a), 640);
	CHECK_INT(lv_area_get_height(&a), 448);
	CHECK(ui_test_shows("START"));
	CHECK(ui_test_shows("C"));
}

/* The game's size follows the mode: 240 lines (PAL) doubled, 448 interlaced as they are */
TEST(modes)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	fake_genesis.h = 240;
	CHECK(core_frames());
	lv_area_t a = view();
	CHECK_INT(lv_area_get_height(&a), 480);
	fake_genesis.h = 448;
	CHECK(core_frames());
	a = view();
	CHECK_INT(lv_area_get_height(&a), 448);
	fake_genesis.color = 0x20c040;
	CHECK(core_frames());
	CHECK(same(ui_test_pixel(360, BODY_Y + 16 + 200), 0x20c040));
}

/* Left: where it was and its battery RAM saved, "Saved" in the list; opened again,
 * both given back to the core */
TEST(left_saved_then_resumed)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	fake_genesis.sram_size = 16;
	CHECK(playing("Sonic"));
	memcpy(fake_genesis.sram, "battery-backed!", 16);
	CHECK(ui_test_back());
	CHECK_INT(fake_genesis.unloads, 1);
	CHECK(ui_test_shows("Saved"));
	size_t size;
	char *state = file(SAVES "/Sonic.state", &size), *srm = file(SAVES "/Sonic.srm", &size);
	CHECK(state && strncmp(state, "frames ", 7) == 0);
	CHECK(srm && size == 16 && memcmp(srm, "battery-backed!", 16) == 0);
	memset(fake_genesis.sram, 0, sizeof(fake_genesis.sram));
	CHECK(ui_test_tap_text("Sonic"));
	CHECK(core_frames());
	CHECK_STR(fake_genesis.state, state);
	CHECK(memcmp(fake_genesis.sram, "battery-backed!", 16) == 0);
	CHECK_INT(fake_genesis.loads, 2);
	free(state);
	free(srm);
}

/* Battery RAM as it was loaded is not written again; a game without any writes none */
TEST(battery_ram_unchanged)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_write(ROMS "/Columns.md", "rom"));
	CHECK(ui_test_write(SAVES "/Sonic.srm", "0123456789abcdef"));
	fake_genesis.sram_size = 16;
	CHECK(playing("Sonic"));
	CHECK(memcmp(fake_genesis.sram, "0123456789abcdef", 16) == 0);
	CHECK(ui_test_back());
	size_t size;
	char *srm = file(SAVES "/Sonic.srm", &size);
	CHECK(srm && size == 16 && memcmp(srm, "0123456789abcdef", 16) == 0);
	free(srm);
	fake_genesis.sram_size = 0;
	CHECK(ui_test_tap_text("Columns"));
	CHECK(core_frames());
	CHECK(ui_test_back());
	CHECK(!file(SAVES "/Columns.srm", &size));
	CHECK(ui_test_find_all("Saved", NULL, 0) == 2);
}

static bool reset(void)
{
	return __atomic_load_n(&fake_genesis.resets, __ATOMIC_ACQUIRE) > 0;
}

/* The header's button restarts the game */
TEST(restart)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	CHECK(ui_test_wait_until(reset, 5000));
	CHECK_INT(fake_genesis.resets, 1);
}

/* A game the core refuses, or no core: said so, nothing saved */
TEST(cannot_start)
{
	CHECK(ui_test_write(ROMS "/Broken.md", "rom"));
	fake_genesis.load_fails = true;
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_tap_text("Broken"));
	CHECK(ui_test_shows("Cannot start this game"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("Saved"));
	CHECK_INT(fake_genesis.unloads, 0);
}

TEST(no_core)
{
	CHECK(remove(APP "/lib/genesis_plus_gx_libretro.so") == 0);
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_tap_text("Sonic"));
	CHECK(ui_test_shows("Cannot start this game"));
}

/* Held so, the display turns */
static bool held(float x, float y, lv_display_rotation_t rotation)
{
	const float up[3] = { x, y, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000);
	return lv_display_get_rotation(lv_display_get_default()) == rotation;
}

/* Sideways: full screen, the game three times its size in the middle, back and
 * restart at the top left */
TEST(sideways)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_sensors(true, false);
	CHECK(playing("Sonic"));
	CHECK(held(1, 0, LV_DISPLAY_ROTATION_90));
	CHECK(!ui_test_shows("Sonic")); /* no header */
	lv_area_t a = view();
	CHECK_INT(lv_area_get_width(&a), 960);
	CHECK_INT(lv_area_get_height(&a), 672);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	CHECK(ui_test_wait_until(reset, 5000));
	CHECK(held(-1, 0, LV_DISPLAY_ROTATION_270));
	CHECK(held(0, 1, LV_DISPLAY_ROTATION_0));
	CHECK(ui_test_shows("Sonic"));
	CHECK(held(1, 0, LV_DISPLAY_ROTATION_90));
	CHECK(ui_test_tap_text(LV_SYMBOL_LEFT));
	CHECK(ui_test_shows("Saved"));
}

/* No frames run (nor sound) */
static bool still(void)
{
	ui_test_wait(100); /* a frame under way finished */
	frames_mark = __atomic_load_n(&fake_genesis.frames, __ATOMIC_ACQUIRE);
	return !ui_test_wait_until(run_since_mark, 500);
}

/* Covered by the lock screen (the phone put to sleep, or woken) or the quick settings, the
 * game waits; uncovered, it goes on */
TEST(paused_while_covered)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	ui_lock();
	ui_test_settle();
	CHECK(still());
	ui_test_swipe(360, 1200, 360, 400, 400);
	CHECK(!ui_locked());
	CHECK(core_frames());
	ui_test_tap_at(360, UI_STATUS_H / 2); /* the quick settings */
	ui_test_settle();
	CHECK(ui_shade_shown());
	CHECK(still());
	ui_shade_close();
	CHECK(core_frames());
}

/* Several fingers at once (the touchscreen read beside LVGL, ui_touch_*()): the D-pad by
 * the angle from its center, from wherever the finger that landed on it goes; the buttons
 * near each other finger, between two both; each finger keeps what it landed on */

#define BIT(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)

static unsigned pad_wanted;

static bool pad_is(void)
{
	return __atomic_load_n(&fake_genesis.pad, __ATOMIC_ACQUIRE) == pad_wanted;
}

static bool pad_becomes(unsigned pad)
{
	pad_wanted = pad;
	return ui_test_wait_until(pad_is, 3000);
}

/* Upright: the D-pad's center at 190, 1040; A at 410, 1110, B at 530, 1040; START at 360, 1330 */
TEST(touch_dpad_and_buttons)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	CHECK(playing("Sonic"));
	ui_test_touch(0, 90, 1040);
	CHECK(pad_becomes(BIT(LEFT)));
	ui_test_touch(1, 410, 1110);
	CHECK(pad_becomes(BIT(LEFT) | BIT(Y))); /* A is the core's Y */
	ui_test_touch(0, 260, 970);             /* slid up and right: a diagonal */
	CHECK(pad_becomes(BIT(UP) | BIT(RIGHT) | BIT(Y)));
	ui_test_touch(0, 600, 1040); /* far to the right of the D-pad, still its */
	CHECK(pad_becomes(BIT(RIGHT) | BIT(Y)));
	ui_test_touch(1, 470, 1075); /* between A and B */
	CHECK(pad_becomes(BIT(RIGHT) | BIT(Y) | BIT(B)));
	ui_test_touch_up(0);
	CHECK(pad_becomes(BIT(Y) | BIT(B)));
	ui_test_touch_up(1);
	CHECK(pad_becomes(0));
	ui_test_touch(2, 360, 1330);
	CHECK(pad_becomes(BIT(START)));
	ui_test_touch(2, 90, 1040); /* onto the D-pad: not its */
	CHECK(pad_becomes(0));
	ui_test_touch_up(2);
}

/* A controller's touchpad has multi-touch axes too: the fingers are still the screen's */
TEST(touch_not_a_controllers_touchpad)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	ui_test_touchpad();
	CHECK(playing("Sonic"));
	ui_test_touch(0, 90, 1040);
	CHECK(pad_becomes(BIT(LEFT)));
	ui_test_touch_up(0);
	CHECK(pad_becomes(0));
}

/* A game controller is the Mega Drive's six-button pad, as the core takes one: its bottom,
 * right and R1 buttons A, B and C (an 8BitDo M30's A, B, C), left, top and L1 X, Y and Z
 * (the core's L, X and R), Select Mode; its D-pad (the M30's: the left stick), with a
 * finger on the screen's controls too */
TEST(game_controller)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	ui_test_gamepad("Wireless Controller", false);
	CHECK(playing("Sonic"));
	ui_test_gamepad_button(BTN_SOUTH, true);
	CHECK(pad_becomes(BIT(Y)));
	ui_test_gamepad_button(BTN_EAST, true);
	CHECK(pad_becomes(BIT(Y) | BIT(B)));
	ui_test_gamepad_button(BTN_TR, true);
	CHECK(pad_becomes(BIT(Y) | BIT(B) | BIT(A)));
	ui_test_gamepad_button(BTN_SOUTH, false);
	ui_test_gamepad_button(BTN_EAST, false);
	ui_test_gamepad_button(BTN_TR, false);
	CHECK(pad_becomes(0));
	ui_test_gamepad_button(BTN_WEST, true);
	CHECK(pad_becomes(BIT(L)));
	ui_test_gamepad_button(BTN_NORTH, true);
	CHECK(pad_becomes(BIT(L) | BIT(X)));
	ui_test_gamepad_button(BTN_TL, true);
	CHECK(pad_becomes(BIT(L) | BIT(X) | BIT(R)));
	ui_test_gamepad_button(BTN_WEST, false);
	ui_test_gamepad_button(BTN_NORTH, false);
	ui_test_gamepad_button(BTN_TL, false);
	ui_test_gamepad_button(BTN_START, true);
	CHECK(pad_becomes(BIT(START)));
	ui_test_gamepad_button(BTN_START, false);
	ui_test_gamepad_button(BTN_SELECT, true);
	CHECK(pad_becomes(BIT(SELECT)));
	ui_test_gamepad_button(BTN_SELECT, false);
	CHECK(pad_becomes(0));

	ui_test_gamepad_axis(ABS_X, 0);
	CHECK(pad_becomes(BIT(LEFT)));
	ui_test_gamepad_axis(ABS_Y, 255);
	CHECK(pad_becomes(BIT(LEFT) | BIT(DOWN)));
	ui_test_gamepad_axis(ABS_X, 128);
	ui_test_gamepad_axis(ABS_Y, 128);
	CHECK(pad_becomes(0));

	/* Both at once: the controller's A, a finger on the screen's D-pad */
	ui_test_gamepad_button(BTN_SOUTH, true);
	ui_test_touch(0, 90, 1040);
	CHECK(pad_becomes(BIT(Y) | BIT(LEFT)));
	ui_test_touch_up(0);
	CHECK(pad_becomes(BIT(Y)));
	ui_test_gamepad_button(BTN_SOUTH, false);
	CHECK(pad_becomes(0));
}

/* Connected once the game plays, it is found; disconnected while a button is held, the
 * button is let go */
TEST(game_controller_connected_while_playing)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	ui_test_gamepad("Wireless Controller", false);
	ui_test_gamepad_button(BTN_SOUTH, true);
	CHECK(pad_becomes(BIT(Y)));
	ui_test_gamepad_gone();
	CHECK(pad_becomes(0));
}

/* Sideways, in the turned layout (the D-pad's center at 124, 430 of 1440 x 720): the
 * panel's coordinates turned either way */
TEST(touch_sideways)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	ui_test_sensors(true, false);
	CHECK(playing("Sonic"));
	CHECK(held(1, 0, LV_DISPLAY_ROTATION_90));
	ui_test_touch(0, 719 - 430, 24); /* the layout's 24, 430: left of the D-pad */
	CHECK(pad_becomes(BIT(LEFT)));
	ui_test_touch_up(0);
	CHECK(pad_becomes(0));
	CHECK(held(-1, 0, LV_DISPLAY_ROTATION_270));
	ui_test_touch(0, 430, 1439 - 24);
	CHECK(pad_becomes(BIT(LEFT)));
	ui_test_touch_up(0);
	CHECK(pad_becomes(0));
}

/* Covered, nothing is held; what the fingers did meanwhile was meant for what covered it */
TEST(touches_dropped_while_covered)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	CHECK(playing("Sonic"));
	ui_test_touch(0, 410, 1110);
	CHECK(pad_becomes(BIT(Y)));
	ui_lock();
	ui_test_settle();
	CHECK(still());
	ui_test_touch_up(0);
	ui_test_touch(1, 530, 1040); /* on the lock screen */
	ui_test_swipe(360, 1200, 360, 400, 400);
	CHECK(!ui_locked());
	CHECK(core_frames());
	CHECK_INT(fake_genesis.pad, 0);
	ui_test_touch(2, 530, 1040);
	CHECK(pad_becomes(BIT(B)));
}

/* Its sound where phone-ui's goes (ui_sound_device()): the phone's, or a Bluetooth speaker
 * connected, onto which it moves while playing */

#define SPEAKER "F0:AE:66:E8:A4:BB"
#define TO_SPEAKER "phone_bt:DEV=" SPEAKER ",RATE=48000"

static bool on_speaker(void)
{
	return strcmp(ui_test_played_device(), TO_SPEAKER) == 0;
}

static bool on_phone(void)
{
	return strcmp(ui_test_played_device(), "default") == 0;
}

static void speaker(bool connected)
{
	ui_test_write("/var/lib/phone-ui/settings", "bluetooth=1\n");
	ui_test_bt_running(true);
	ui_test_bt_paired(SPEAKER, "soundcore R50i", connected);
	ui_test_bt_audio(SPEAKER, 48000);
	ui_test_start();
	ui_test_wait(1100); /* the speaker seen (each second) */
}

TEST(sound_on_the_phone)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	CHECK(ui_test_wait_until(on_phone, 3000));
}

TEST(sound_on_bluetooth_speaker)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	speaker(true);
	CHECK(playing("Sonic"));
	CHECK(ui_test_wait_until(on_speaker, 3000));
}

TEST(sound_moves_to_speaker_and_back)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	speaker(false);
	CHECK(playing("Sonic"));
	CHECK(ui_test_wait_until(on_phone, 3000));
	ui_test_bt_connected(SPEAKER, true);
	CHECK(ui_test_wait_until(on_speaker, 5000));
	ui_test_bt_connected(SPEAKER, false);
	CHECK(ui_test_wait_until(on_phone, 5000));
}

/* The frames run in a second of real time */
static unsigned frames_in_a_second(void)
{
	unsigned before = __atomic_load_n(&fake_genesis.frames, __ATOMIC_ACQUIRE);
	usleep(1000000);
	return __atomic_load_n(&fake_genesis.frames, __ATOMIC_ACQUIRE) - before;
}

/* Without sound the game runs by the clock, at the rate the core tells */
TEST(no_sound_by_the_clock)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_no_sound_card(true);
	fake_genesis.fps = 10;
	CHECK(playing("Sonic"));
	unsigned n = frames_in_a_second();
	CHECK(n >= 3 && n <= 13);
}

/* The speaker gone and the phone's sound with it: the game goes on, by the clock */
TEST(sound_lost_game_goes_on)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	speaker(true);
	CHECK(playing("Sonic"));
	CHECK(ui_test_wait_until(on_speaker, 3000));
	ui_test_no_sound_card(true);
	ui_test_bt_connected(SPEAKER, false);
	ui_test_wait(1500); /* the device looked at again (each second) */
	CHECK(core_frames());
	fake_genesis.color = 0x20c040;
	CHECK(core_frames());
	CHECK(same(ui_test_pixel(360, BODY_Y + 16 + 200), 0x20c040));
}

/* A save state the core refuses (another version's): the game starts afresh */
TEST(save_state_refused)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_write(SAVES "/Sonic.state", "not a state"));
	CHECK(playing("Sonic"));
	CHECK_STR(fake_genesis.state, "");
	CHECK_INT(fake_genesis.loads, 1);
}

/* Saves that cannot be written (the folder read-only): not "Saved", the game played */
TEST(saves_not_written)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_write(SAVES "/other", ""));
	CHECK(chmod(SAVES, 0555) == 0);
	fake_genesis.sram_size = 16;
	CHECK(playing("Sonic"));
	memcpy(fake_genesis.sram, "battery-backed!", 16);
	CHECK(ui_test_back());
	CHECK_INT(fake_genesis.unloads, 1);
	CHECK(!ui_test_shows("Saved"));
	CHECK(access(SAVES "/Sonic.state", F_OK) != 0);
	CHECK(access(SAVES "/Sonic.srm", F_OK) != 0);
	chmod(SAVES, 0755);
}

/* The D-pad's center: no direction until the finger moves from it */
TEST(touch_dpad_center)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_touchscreen();
	CHECK(playing("Sonic"));
	ui_test_touch(0, 190 + 10, 1040 - 10);
	ui_test_touch(1, 410, 1110); /* A: the finger on the D-pad was read */
	CHECK(pad_becomes(BIT(Y)));
	CHECK(core_frames());
	CHECK_INT(fake_genesis.pad, BIT(Y));
	ui_test_touch(0, 190, 1040 + 60);
	CHECK(pad_becomes(BIT(DOWN) | BIT(Y)));
}

/* Interlaced PAL (576 lines) sideways: as high as the display, no higher */
TEST(sideways_interlaced)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	ui_test_sensors(true, false);
	CHECK(playing("Sonic"));
	CHECK(held(1, 0, LV_DISPLAY_ROTATION_90));
	fake_genesis.h = 576;
	CHECK(core_frames());
	lv_area_t a = view();
	CHECK_INT(lv_area_get_width(&a), 960);
	CHECK_INT(lv_area_get_height(&a), 720);
}

/* A core without libretro's functions (another library in its place): said so */
TEST(core_incomplete)
{
	CHECK(copy(GENESIS_APP "/genesis.so", APP "/lib/genesis_plus_gx_libretro.so"));
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_tap_text("Sonic"));
	CHECK(ui_test_shows("Cannot start this game"));
}

/* Left, the list goes (its games freed); opened again, read again */
TEST(list_closed_and_opened_again)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_shows("Sonic"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("Sonic"));
	CHECK(ui_test_write(ROMS "/Columns.md", "rom"));
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_shows("Sonic"));
	CHECK(ui_test_shows("Columns"));
}

static size_t played_mark;

static bool played_more(void)
{
	size_t frames;
	unsigned channels, rate;
	ui_test_played(&frames, &channels, &rate);
	return frames > played_mark;
}

/* The sound card goes while playing, then comes back: the game goes on, silent meanwhile,
 * and is heard again */
TEST(sound_card_lost_and_back)
{
	CHECK(ui_test_write(ROMS "/Sonic.md", "rom"));
	CHECK(playing("Sonic"));
	CHECK(ui_test_wait_until(on_phone, 3000));
	ui_test_no_sound_card(true);
	ui_test_sound_lost();
	ui_test_wait(1500); /* past when it tries again: none to open */
	unsigned channels, rate;
	ui_test_played(&played_mark, &channels, &rate);
	CHECK(core_frames());
	ui_test_wait(500);
	CHECK(!played_more());
	ui_test_no_sound_card(false);
	CHECK(ui_test_wait_until(played_more, 5000));
	CHECK(core_frames());
}
