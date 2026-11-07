/*
 * The volume keys (volume.c): a step per press, repeated while held, a slider at
 * the right edge until it hides; with Power, a screenshot instead (no display to
 * read here: only that the keys do nothing else).
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define HIDE_MS 1500

static char *file(const char *path)
{
	static char buf[256];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

TEST_SETUP
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
}

static lv_obj_t *slider(void)
{
	return ui_test_find_widget(&lv_slider_class);
}

TEST(a_press_steps_and_shows_the_slider)
{
	ui_test_start();
	CHECK(!slider());
	ui_test_button_press(KEY_VOLUMEUP, 50);
	CHECK_INT(ui_volume_get(), 13);
	CHECK_STR(file(SETTINGS), "volume=13\n");
	lv_obj_t *s = slider();
	CHECK(s && lv_slider_get_value(s) == 13);
	CHECK(ui_test_shows(LV_SYMBOL_VOLUME_MAX));
	ui_test_wait(HIDE_MS + 100);
	CHECK(!slider());
}

TEST(saved_level)
{
	ui_test_write(SETTINGS, "volume=3\n");
	ui_test_start();
	CHECK_INT(ui_volume_get(), 3);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_volume_get(), 2);
	CHECK(ui_test_shows(LV_SYMBOL_VOLUME_MID));
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_volume_get(), 0);
	CHECK(ui_test_shows(LV_SYMBOL_MUTE));
	ui_test_button_press(KEY_VOLUMEDOWN, 50); /* no lower */
	CHECK_INT(ui_volume_get(), 0);
}

TEST(held_it_repeats)
{
	ui_test_start();
	ui_test_button_press(KEY_VOLUMEDOWN, 1000);
	/* At once, after 400 ms, then every 120 ms */
	CHECK(ui_volume_get() >= 4 && ui_volume_get() <= 6);
	int level = ui_volume_get();
	ui_test_wait(500);
	CHECK_INT(ui_volume_get(), level); /* released: stopped */

	ui_test_button_press(KEY_VOLUMEUP, 3000);
	CHECK_INT(ui_volume_get(), UI_VOLUME_LEVELS);
}

TEST(each_press_keeps_it_shown)
{
	ui_test_start();
	for (int i = 0; i < 3; i++) {
		ui_test_wait(HIDE_MS - 500);
		ui_test_button_press(KEY_VOLUMEUP, 50);
	}
	CHECK(slider() != NULL);
	CHECK_INT(ui_volume_get(), 15);
}

TEST(slider_dragged)
{
	ui_test_start();
	ui_test_button_press(KEY_VOLUMEUP, 50);
	lv_obj_t *s = slider();
	CHECK(s != NULL);
	if (!s)
		return;
	lv_area_t a;
	lv_obj_get_coords(s, &a);
	int32_t x = (a.x1 + a.x2) / 2;
	/* Held on it past the hiding time, then down to the bottom */
	lv_test_mouse_move_to(x, a.y1 + 40);
	lv_test_mouse_press();
	ui_test_wait(HIDE_MS + 200);
	CHECK(slider() != NULL); /* not under the finger */
	for (int32_t y = a.y1 + 40; y <= a.y2 + 40; y += 20) {
		lv_test_mouse_move_to(x, y);
		ui_test_wait(16);
	}
	lv_test_mouse_release();
	ui_test_settle();
	CHECK_INT(ui_volume_get(), 0);
	CHECK(ui_test_shows(LV_SYMBOL_MUTE));
	CHECK_STR(file(SETTINGS), "volume=0\n");
	ui_test_wait(HIDE_MS + 100);
	CHECK(!slider());
}

/* Power held, then Volume Down: a screenshot, neither a step nor the power menu */
TEST(screenshot_power_first)
{
	ui_test_start();
	ui_test_button(KEY_POWER, true);
	ui_test_wait(200);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	ui_test_wait(1000); /* past the power menu's hold */
	ui_test_button(KEY_POWER, false);
	ui_test_settle();
	CHECK_INT(ui_volume_get(), 12);
	CHECK(!slider());
	CHECK(!ui_test_shows("Power off"));
	CHECK(!ui_locked()); /* nor a sleep */
	CHECK_STR(file(SETTINGS), "");
}

/* Volume Down held, then Power: its step taken back */
TEST(screenshot_volume_first)
{
	ui_test_start();
	ui_test_button(KEY_VOLUMEDOWN, true);
	ui_test_wait(100);
	CHECK_INT(ui_volume_get(), 11);
	ui_test_button(KEY_POWER, true);
	ui_test_wait(1000); /* no repeat, no power menu */
	ui_test_button(KEY_POWER, false);
	ui_test_button(KEY_VOLUMEDOWN, false);
	ui_test_settle();
	CHECK_INT(ui_volume_get(), 12);
	CHECK(!slider());
	CHECK(!ui_test_shows("Power off"));
	CHECK(!ui_locked());
	/* Then the keys work again */
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_volume_get(), 11);
}

/* A Bluetooth speaker playing (bluetooth.c): the keys and the slider step its volume
 * (bluez-alsa's, 0-127), read back from it; the phone's level stays as it was */

#define SPEAKER "F0:AE:66:E8:A4:BB"

static void speaker(void)
{
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_bt_running(true);
	ui_test_bt_paired(SPEAKER, "soundcore R50i", true);
	ui_test_bt_audio(SPEAKER, 48000);
	ui_test_start();
	ui_test_wait(1100); /* the speaker seen (each second) */
}

TEST(speaker_volume_stepped)
{
	speaker();
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_test_bt_volume(SPEAKER), 119); /* 15 of 16 */
	lv_obj_t *s = slider();
	CHECK(s && lv_slider_get_value(s) == 15);
	CHECK_INT(ui_volume_get(), 15);
	CHECK(strstr(file(SETTINGS), "volume=") == NULL); /* the phone's untouched */
	ui_test_button_press(KEY_VOLUMEUP, 50);
	CHECK_INT(ui_test_bt_volume(SPEAKER), 127);
}

TEST(speaker_muted)
{
	speaker();
	ui_test_bt_set_volume(SPEAKER, 8); /* 1 of 16 */
	ui_test_wait(1100);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_test_bt_volume(SPEAKER), -1);
	CHECK(ui_test_shows(LV_SYMBOL_MUTE));
	ui_test_button_press(KEY_VOLUMEUP, 50);
	CHECK_INT(ui_test_bt_volume(SPEAKER), 8);
}

TEST(speaker_buttons_followed)
{
	speaker();
	ui_test_bt_set_volume(SPEAKER, 64); /* about half, on the speaker */
	ui_test_wait(1100);
	ui_test_button_press(KEY_VOLUMEUP, 50);
	lv_obj_t *s = slider();
	CHECK(s && lv_slider_get_value(s) == 9);
	CHECK_INT(ui_test_bt_volume(SPEAKER), 71);
}

TEST(phone_again_once_speaker_gone)
{
	speaker();
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	ui_test_wait(HIDE_MS + 100);
	ui_test_bt_connected(SPEAKER, false);
	ui_test_wait(1100);
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	CHECK_INT(ui_volume_get(), 11); /* the phone's 12, stepped */
	CHECK(strstr(file(SETTINGS), "volume=11") != NULL);
	CHECK_INT(ui_test_bt_volume(SPEAKER), 119);
}
