/*
 * File transfer over USB (usb_mode.c): the shade's toggle while a computer is plugged in,
 * the board's script switching the USB gadget, back to the default mode when unplugged.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>

#include "test.h"
#include "ui_test.h"
#include "usb_mode.h"

#define USB_POWER "/sys/class/power_supply/axp20x-usb/online"
#define UDC_STATE "/sys/class/udc/musb-hdrc.2.auto/state"

TEST_SETUP
{
	ui_test_reset();
}

/* Plugged into a computer (the gadget configured), a charger, or nothing */
static void plug_computer(void)
{
	ui_test_write(USB_POWER, "1\n");
	ui_test_write(UDC_STATE, "configured\n");
}

static void plug_charger(void)
{
	ui_test_write(USB_POWER, "1\n");
	ui_test_write(UDC_STATE, "not attached\n");
}

static void unplug(void)
{
	ui_test_write(USB_POWER, "0\n");
	ui_test_write(UDC_STATE, "not attached\n");
}

/* Tapped on the status bar: opens, tapped again: closes */
static void bar_tap(void)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
}

/* The toggle, NULL when not shown */
static lv_obj_t *tile(void)
{
	lv_obj_t *label = ui_test_shows("File transfer") ? ui_test_find("File transfer") : NULL;
	return label ? lv_obj_get_parent(label) : NULL;
}

static bool tile_on(void)
{
	return tile() && lv_obj_has_state(tile(), LV_STATE_CHECKED);
}

static bool idle(void)
{
	return !usb_mode_busy();
}

/* Started in the default mode: nothing to switch */
TEST(nothing_run_at_startup)
{
	plug_computer();
	ui_test_start();
	ui_test_wait(500);
	CHECK(!usb_file_transfer());
	CHECK(!ui_test_usb_mode_ran("default", 0));
}

/* Restarted (`just ui`, a crash) while a computer has the files open: kept, the tile on */
TEST(kept_across_a_restart)
{
	ui_test_usb_mode_left("mtp");
	plug_computer();
	ui_test_start();
	CHECK(usb_file_transfer());
	bar_tap();
	CHECK(tile_on());
	ui_test_wait(1100);
	CHECK(!ui_test_usb_mode_ran("default", 0));
	/* And off with the cable as ever */
	unplug();
	CHECK(ui_test_usb_mode_ran("default", 3000));
}

/* Left on with the cable out meanwhile: back to the default mode */
TEST(left_on_unplugged)
{
	ui_test_usb_mode_left("mtp");
	ui_test_start();
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(!usb_file_transfer());
}

TEST(on_and_off)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(tile() && !tile_on());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(tile_on());
	CHECK(usb_file_transfer());
	/* Still on at the next opening */
	bar_tap();
	bar_tap();
	CHECK(tile_on());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("default", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(tile() && !tile_on());
}

/* A charger powers the USB input, but there is no computer to give files to */
TEST(not_on_a_charger)
{
	plug_charger();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(!tile());
}

TEST(not_unplugged)
{
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(!tile());
}

/* Plugged in with the shade open: it shows up */
TEST(shown_once_plugged_in)
{
	ui_test_start();
	bar_tap();
	CHECK(!tile());
	plug_computer();
	ui_test_wait(1100);
	CHECK(tile() && !tile_on());
}

/* Pulled out: the default mode again, and no toggle */
TEST(off_when_unplugged)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	unplug();
	CHECK(ui_test_usb_mode_ran("default", 3000));
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(!usb_file_transfer());
	ui_test_wait(1100);
	CHECK(!tile());
}

/* The gadget leaves the computer while switching: not taken for unplugged */
TEST(kept_while_the_computer_enumerates)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	ui_test_write(UDC_STATE, "not attached\n");
	ui_test_wait(2100);
	CHECK(tile_on());
	CHECK(usb_file_transfer());
	ui_test_write(UDC_STATE, "configured\n");
	ui_test_wait(1100);
	CHECK(tile_on());
	CHECK(!ui_test_usb_mode_ran("default", 0));
}

/* Refused (a trial boot): off again */
TEST(refused)
{
	plug_computer();
	ui_test_start();
	ui_test_usb_mode_fails(true);
	bar_tap();
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	ui_test_wait(1100);
	CHECK(tile() && !tile_on());
	CHECK(!usb_file_transfer());
}

/* Not turned on from the lock screen: the files are the owner's to give */
TEST(not_from_the_lock_screen)
{
	ui_test_write("/sys/power/state", "");
	plug_computer();
	ui_test_start();
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_locked());
	bar_tap();
	CHECK(tile() && lv_obj_has_state(tile(), LV_STATE_DISABLED));
	ui_test_tap(tile());
	ui_test_wait(500);
	CHECK(!usb_file_transfer());
	CHECK(!ui_test_usb_mode_ran("mtp", 0));
}

/* A board without the script: no file transfer, nothing run */
TEST(without_the_script)
{
	remove("/tmp/usb-mode.sh");
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(!tile());
}

/* The longest of its words, in the narrowest of the tiles (three on the second row) */
TEST(romanian_fits)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	plug_computer();
	ui_test_start();
	bar_tap();
	lv_obj_t *label = ui_test_find("Transfer de fișiere");
	CHECK(label);
	if (!label)
		return;
	lv_area_t l, t;
	lv_obj_get_coords(label, &l);
	lv_obj_get_coords(lv_obj_get_parent(label), &t);
	CHECK(l.x1 >= t.x1 && l.x2 <= t.x2);
}
