/*
 * Power: a short press puts the phone to sleep behind the lock screen, a hold opens
 * the power menu. The lock screen goes when dragged up past a third or flicked.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "ui_test.h"

#define STATE "/sys/power/state"
#define BACKLIGHT "/sys/class/backlight/backlight/"
#define HINT "Swipe up to unlock"

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
	ui_test_write(STATE, "");
	ui_test_start();
}

/* Power pressed briefly: asleep, then awake behind the lock screen */
static void sleep_wake(void)
{
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300); /* the fade out, the lock screen shown, the sleep */
	ui_test_settle();
}

TEST(power_press_sleeps_behind_the_lock_screen)
{
	CHECK(!ui_locked());
	sleep_wake();
	CHECK_STR(file(STATE), "freeze");
	CHECK(ui_locked());
	CHECK(ui_test_shows(HINT));
	/* Awake: the backlight on again, back to its brightness after the fade */
	CHECK_STR(file(BACKLIGHT "bl_power"), "0\n");
	CHECK_STR(file(BACKLIGHT "brightness"), "500\n");
}

TEST(no_sleep_without_the_kernel)
{
	remove(STATE);
	mkdir(STATE, 0755); /* cannot be written */
	sleep_wake();
	CHECK(ui_locked()); /* locked all the same */
	CHECK_STR(file(BACKLIGHT "bl_power"), "0\n");
	CHECK_STR(file(BACKLIGHT "brightness"), "500\n");
}

TEST(the_press_that_woke_it_is_ignored)
{
	sleep_wake();
	ui_test_write(STATE, "");
	sleep_wake(); /* within a second of waking: the press that woke it */
	CHECK_STR(file(STATE), "");
	ui_test_wait(1000);
	sleep_wake();
	CHECK_STR(file(STATE), "freeze");
	lv_obj_t *found[2];
	CHECK_INT(ui_test_find_all(HINT, found, 2), 1); /* the same lock screen */
}

/* A short press and release, read in the frames that follow */
static void power_tap(void)
{
	ui_test_button(KEY_POWER, true);
	ui_test_wait(48);
	ui_test_button(KEY_POWER, false);
	ui_test_wait(48);
}

TEST(presses_made_while_it_went_to_sleep_are_ignored)
{
	/* Pressed twice while the kernel suspended: neither woke it, both arrive with the
	 * press that did, right after waking (seen on the phone: asleep again 0.2 s later) */
	sleep_wake();
	ui_test_write(STATE, "");
	power_tap();
	power_tap();
	ui_test_settle();
	CHECK_STR(file(STATE), "");
	CHECK_STR(file(BACKLIGHT "bl_power"), "0\n");
	ui_test_wait(1000);
	sleep_wake();
	CHECK_STR(file(STATE), "freeze");
}

TEST(the_press_that_stopped_the_sleep_is_ignored)
{
	/* A press while the kernel suspends stops it: the write fails, then that press
	 * arrives */
	remove(STATE);
	mkdir(STATE, 0755);
	sleep_wake();
	rmdir(STATE);
	ui_test_write(STATE, "");
	power_tap();
	ui_test_settle();
	CHECK_STR(file(STATE), "");
	CHECK_STR(file(BACKLIGHT "bl_power"), "0\n");
}

TEST(dragged_up_it_unlocks)
{
	sleep_wake();
	ui_test_swipe(360, 1200, 360, 500, 600);
	CHECK(!ui_locked());
	CHECK(!ui_test_shows(HINT));
	CHECK(ui_at_home()); /* the drag did not open the menu */
}

TEST(dragged_a_little_it_falls_back)
{
	sleep_wake();
	ui_test_swipe(360, 1200, 360, 1000, 600);
	CHECK(ui_locked());
	lv_obj_t *hint = ui_test_find(HINT);
	CHECK(hint != NULL);
	if (!hint)
		return;
	lv_area_t a;
	lv_obj_get_coords(hint, &a);
	CHECK(a.y2 > 1300); /* at the bottom again */
}

TEST(flicked_up_it_unlocks)
{
	sleep_wake();
	ui_test_swipe(360, 1200, 360, 1000, 48);
	CHECK(!ui_locked());
}

TEST(flicked_down_it_stays)
{
	sleep_wake();
	/* Up past a third slowly, then quickly down a little before letting go */
	lv_test_mouse_move_to(360, 1300);
	lv_test_mouse_press();
	for (int y = 1300; y >= 600; y -= 50) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	for (int y = 600; y <= 800; y += 100) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(ui_locked());
}

TEST(taps_do_not_reach_the_screen_under_it)
{
	sleep_wake();
	ui_test_tap_at(106, 1348); /* the dock's first app, Phone, under the lock screen */
	CHECK(ui_at_home());
	CHECK(ui_locked());
	/* Unlocked, the same tap opens it */
	ui_test_swipe(360, 1200, 360, 400, 400);
	ui_test_tap_at(106, 1348);
	CHECK(!ui_at_home());
}

TEST(locked_again_half_dragged_it_goes_back_down)
{
	sleep_wake();
	ui_test_wait(1000);
	lv_test_mouse_move_to(360, 1200);
	lv_test_mouse_press();
	for (int y = 1200; y >= 900; y -= 50) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	sleep_wake(); /* the finger still down */
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(ui_locked());
	lv_area_t a;
	lv_obj_t *hint = ui_test_find(HINT);
	CHECK(hint != NULL);
	if (hint) {
		lv_obj_get_coords(hint, &a);
		CHECK(a.y2 > 1300);
	}
}

TEST(unlocked_the_app_is_still_open)
{
	CHECK(ui_test_open("Notes"));
	sleep_wake();
	ui_test_swipe(360, 1200, 360, 400, 400);
	CHECK(!ui_locked());
	CHECK(ui_test_shows("Notes"));
}

/* The lock screen has a big clock: none in the status bar over it, back once unlocked
 * over an app */
TEST(no_status_bar_clock_while_locked)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_status_bar_clock());
	sleep_wake();
	CHECK(!ui_test_status_bar_clock());
	ui_test_swipe(360, 1200, 360, 400, 400);
	ui_test_settle();
	CHECK(!ui_locked());
	CHECK(ui_test_status_bar_clock());
}

/* Power menu */

TEST(power_held_opens_the_menu)
{
	ui_test_button_press(KEY_POWER, 1000);
	CHECK(ui_test_shows("Power off"));
	CHECK(ui_test_shows("Restart"));
	CHECK(!ui_locked()); /* a hold does not sleep */
	CHECK_STR(file(STATE), "");

	ui_test_tap_at(360, 100); /* beside it */
	CHECK(!ui_test_shows("Power off"));
}

TEST(power_pressed_again_closes_the_menu)
{
	ui_test_button_press(KEY_POWER, 1000);
	CHECK(ui_test_shows("Power off"));
	ui_test_button_press(KEY_POWER, 100);
	CHECK(!ui_test_shows("Power off"));
	CHECK(!ui_locked()); /* nor sleeps */
	CHECK_STR(file(STATE), "");
}

TEST(restart)
{
	ui_test_button_press(KEY_POWER, 1000);
	CHECK(ui_test_tap_text("Restart"));
	CHECK(ui_test_shows("Restarting..."));
	ui_test_wait(200);
	CHECK_STR(ui_test_ran(), "reboot\n");
	/* Until init stops phone-ui: taps and Power do not close it */
	ui_test_tap_at(360, 100);
	ui_test_button_press(KEY_POWER, 100);
	CHECK(ui_test_shows("Restarting..."));
}

TEST(power_off_failing_closes_the_menu)
{
	ui_test_commands_fail(true);
	ui_test_button_press(KEY_POWER, 1000);
	CHECK(ui_test_tap_text("Power off"));
	CHECK(ui_test_shows("Powering off..."));
	ui_test_wait(200);
	CHECK_STR(ui_test_ran(), "poweroff\n");
	CHECK(!ui_test_shows("Powering off..."));
	CHECK(!ui_test_shows("Restart"));
}
