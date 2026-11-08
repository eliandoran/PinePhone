/*
 * File transfer over USB (usb_mode.c): the shade's USB row, unfolded by its arrow while a
 * computer is plugged in, lists what the USB is used for (Network only, File transfer); the
 * board's script switches the USB gadget, back to the default mode when unplugged.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"
#include "net/usb_mode.h"

#define USB_POWER "/sys/class/power_supply/axp20x-usb/online"
#define UDC_STATE "/sys/class/udc/musb-hdrc.2.auto/state"
#define CARRIER "/sys/class/net/usb0/carrier"

TEST_SETUP
{
	ui_test_reset();
}

/* Plugged into a computer (the gadget configured, its network up), a charger, or nothing */
static void plug_computer(void)
{
	ui_test_write(USB_POWER, "1\n");
	ui_test_write(UDC_STATE, "configured\n");
	ui_test_write(CARRIER, "1\n");
}

static void plug_charger(void)
{
	ui_test_write(USB_POWER, "1\n");
	ui_test_write(UDC_STATE, "not attached\n");
	ui_test_write(CARRIER, "0\n");
}

static void unplug(void)
{
	ui_test_write(USB_POWER, "0\n");
	ui_test_write(UDC_STATE, "not attached\n");
	ui_test_write(CARRIER, "0\n");
}

/* Tapped on the status bar: opens, tapped again: closes */
static void bar_tap(void)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
}

/* The USB row's arrow: down while folded, up while unfolded; NULL without */
static lv_obj_t *arrow(void)
{
	if (ui_test_shows(LV_SYMBOL_DOWN))
		return ui_test_find(LV_SYMBOL_DOWN);
	if (ui_test_shows(LV_SYMBOL_UP))
		return ui_test_find(LV_SYMBOL_UP);
	return NULL;
}

static bool unfold(void)
{
	return ui_test_shows(LV_SYMBOL_DOWN) && ui_test_tap_text(LV_SYMBOL_DOWN) && ui_test_shows("File transfer");
}

/* A choice's row, NULL when not shown */
static lv_obj_t *choice(const char *text)
{
	lv_obj_t *label = ui_test_shows(text) ? ui_test_find(text) : NULL;
	return label ? lv_obj_get_parent(label) : NULL;
}

/* The choice with the check mark */
static bool chosen(const char *text)
{
	lv_obj_t *row = choice(text);
	lv_obj_t *check = row ? lv_obj_get_child(row, -1) : NULL;
	return check && lv_obj_check_type(check, &lv_label_class) &&
	       strcmp(lv_label_get_text(check), LV_SYMBOL_OK) == 0 && !lv_obj_is_hidden(check);
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

/* Restarted (`just ui`, a crash) while a computer has the files open: kept, chosen */
TEST(kept_across_a_restart)
{
	ui_test_usb_mode_left("mtp");
	plug_computer();
	ui_test_start();
	CHECK(usb_file_transfer());
	bar_tap();
	CHECK(unfold());
	CHECK(chosen("File transfer"));
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
	CHECK(ui_test_usb_mode_ran("default", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(!usb_file_transfer());
}

/* Folded at each opening: the arrow down, no choices; unfolded, the arrow up */
TEST(folded_then_unfolded)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Connected to a computer"));
	CHECK(ui_test_shows(LV_SYMBOL_DOWN));
	CHECK(!ui_test_shows("Network only") && !ui_test_shows("File transfer"));
	CHECK(unfold());
	CHECK(ui_test_shows(LV_SYMBOL_UP) && !ui_test_shows(LV_SYMBOL_DOWN));
	CHECK(chosen("Network only") && !chosen("File transfer"));
	/* The choices under the row, the arrow at its end */
	lv_area_t row, net, arr;
	lv_obj_get_coords(lv_obj_get_parent(ui_test_find("Connected to a computer")), &row);
	lv_obj_get_coords(choice("Network only"), &net);
	lv_obj_get_coords(arrow(), &arr);
	CHECK(net.y1 >= row.y2);
	CHECK(arr.x2 > 600);
	/* Tapped again: folded */
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(!ui_test_shows("Network only"));
	/* And folded at the next opening */
	CHECK(unfold());
	bar_tap();
	bar_tap();
	CHECK(ui_test_shows(LV_SYMBOL_DOWN) && !ui_test_shows("Network only"));
}

TEST(on_and_off)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(unfold());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	ui_test_wait(1100);
	CHECK(chosen("File transfer") && !chosen("Network only"));
	CHECK(usb_file_transfer());
	/* Folded, the row says so; still on at the next opening */
	bar_tap();
	bar_tap();
	CHECK(ui_test_shows("Computer \xc2\xb7 File transfer"));
	CHECK(unfold());
	CHECK(chosen("File transfer"));
	CHECK(ui_test_tap_text("Network only"));
	CHECK(ui_test_usb_mode_ran("default", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	ui_test_wait(1100);
	CHECK(chosen("Network only") && !chosen("File transfer"));
	CHECK(!usb_file_transfer());
}

/* The chosen one again: nothing to switch */
TEST(chosen_again)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(unfold());
	CHECK(ui_test_tap_text("Network only"));
	ui_test_wait(500);
	CHECK(!ui_test_usb_mode_ran("default", 0));
	CHECK(chosen("Network only"));
}

/* A charger powers the USB input, but there is no computer to give files to */
TEST(not_on_a_charger)
{
	plug_charger();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Charger only"));
	CHECK(!arrow());
}

TEST(not_unplugged)
{
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Not connected"));
	CHECK(!arrow());
}

/* Plugged in with the shade open: the arrow shows up */
TEST(shown_once_plugged_in)
{
	ui_test_start();
	bar_tap();
	CHECK(!arrow());
	plug_computer();
	ui_test_wait(1100);
	CHECK(unfold());
	CHECK(chosen("Network only"));
}

/* Pulled out: the default mode again, the choices and the arrow gone */
TEST(off_when_unplugged)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(unfold());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	unplug();
	CHECK(ui_test_usb_mode_ran("default", 3000));
	CHECK(ui_test_wait_until(idle, 2000));
	CHECK(!usb_file_transfer());
	ui_test_wait(1100);
	CHECK(!arrow());
	CHECK(!ui_test_shows("File transfer"));
}

/* The gadget leaves the computer while switching: not taken for unplugged */
TEST(kept_while_the_computer_enumerates)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(unfold());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	ui_test_write(UDC_STATE, "not attached\n");
	ui_test_wait(2100);
	CHECK(chosen("File transfer"));
	CHECK(usb_file_transfer());
	ui_test_write(UDC_STATE, "configured\n");
	ui_test_wait(1100);
	CHECK(chosen("File transfer"));
	CHECK(!ui_test_usb_mode_ran("default", 0));
}

/* Refused (a trial boot): Network only again */
TEST(refused)
{
	plug_computer();
	ui_test_start();
	ui_test_usb_mode_fails(true);
	bar_tap();
	CHECK(unfold());
	CHECK(ui_test_tap_text("File transfer"));
	CHECK(ui_test_usb_mode_ran("mtp", 2000));
	CHECK(ui_test_wait_until(idle, 2000));
	ui_test_wait(1100);
	CHECK(chosen("Network only") && !chosen("File transfer"));
	CHECK(!usb_file_transfer());
}

/* Not chosen from the lock screen: the files are the owner's to give */
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
	CHECK(unfold());
	lv_obj_t *row = choice("File transfer");
	CHECK(row && lv_obj_has_state(row, LV_STATE_DISABLED));
	if (row)
		ui_test_tap(row);
	ui_test_wait(500);
	CHECK(!usb_file_transfer());
	CHECK(!ui_test_usb_mode_ran("mtp", 0));
}

/* A board without the script: no choices, nothing run */
TEST(without_the_script)
{
	remove("/tmp/usb-mode.sh");
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Connected to a computer"));
	CHECK(!arrow());
}

/* No tile any more: the toggles as they were */
TEST(no_tile)
{
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(!ui_test_shows("File transfer"));
}

TEST(romanian)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	plug_computer();
	ui_test_start();
	bar_tap();
	CHECK(ui_test_tap_text(LV_SYMBOL_DOWN));
	CHECK(ui_test_shows("Doar rețea"));
	CHECK(ui_test_shows("Transfer de fișiere"));
}

/* The cable's line on one line beside its arrow, in Romanian too */
TEST(romanian_file_transfer_on_one_line)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	ui_test_usb_mode_left("mtp");
	plug_computer();
	ui_test_start();
	bar_tap();
	lv_obj_t *label = ui_test_find("Computer \xc2\xb7 Transfer de fișiere");
	CHECK(label != NULL);
	if (!label)
		return;
	lv_obj_update_layout(label);
	CHECK_INT(lv_obj_get_height(label), lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
}
