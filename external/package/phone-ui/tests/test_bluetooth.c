/* Settings, Bluetooth (settings/bluetooth.c on bluetooth.c), through the UI (ui_test.h),
 * on the fake BlueZ of fake_bluez.c */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"
#include "ui_symbols.h"
#include "src/misc/lv_text_private.h" /* lv_text_encoded_next() */

#define SETTINGS "/var/lib/phone-ui/settings"
#define HEADSET "00:1B:66:AA:BB:01"
#define LAPTOP "3C:22:FB:00:11:22"

/* A file's content (static), "" if it cannot be read */
static const char *file(const char *path)
{
	static char text[4096];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f)
		fclose(f);
	text[n] = '\0';
	return text;
}

static bool open_bluetooth(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Bluetooth");
}

/* The vertical middle of the label showing `text`, -1 if none */
static int32_t y_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	lv_area_t a;
	if (!label)
		return -1;
	lv_obj_get_coords(label, &a);
	return (a.y1 + a.y2) / 2;
}

static lv_obj_t *the_switch(void)
{
	return ui_test_find_widget(&lv_switch_class);
}

/* Reset first: the settings written before starting stay */
TEST_SETUP
{
	ui_test_reset();
}

TEST(not_running)
{
	CHECK(open_bluetooth());
	CHECK(ui_test_shows("Not available"));
	CHECK(!ui_test_shows("Devices nearby"));
}

TEST(off_by_default)
{
	ui_test_bt_running(false);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("Off"));
	CHECK(ui_test_tap_text("Bluetooth"));
	lv_obj_t *sw = the_switch();
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
	CHECK(!ui_test_shows("Devices nearby"));
	CHECK(!ui_test_bt_discovering());
}

TEST(switched_on_discovers_while_open)
{
	ui_test_bt_running(false);
	CHECK(open_bluetooth());
	lv_obj_t *sw = the_switch();
	CHECK(sw != NULL);
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(ui_test_bt_powered());
	CHECK(strstr(file(SETTINGS), "bluetooth=1") != NULL);
	CHECK(ui_test_wait_for("Devices nearby", 2000));
	CHECK(ui_test_shows("Visible as PinePhone"));
	CHECK(ui_test_bt_discovering());
	CHECK(ui_test_bt_discoverable());
	CHECK_STR(ui_test_bt_agent(), "DisplayYesNo");
	CHECK(ui_test_back());
	CHECK(!ui_test_bt_discovering()); /* only while the page is open */
	CHECK(!ui_test_bt_discoverable());
	CHECK_STR(ui_test_bt_agent(), "");
	CHECK(ui_test_bt_powered());
	CHECK(ui_test_shows("On")); /* the list's value */
}

TEST(switched_off)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Devices nearby", 2000));
	lv_obj_t *sw = the_switch();
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(!ui_test_bt_powered());
	CHECK(ui_test_wait_for("Off", 2000));
	CHECK(!ui_test_shows("Devices nearby"));
	CHECK(strstr(file(SETTINGS), "bluetooth=0") != NULL);
}

TEST(restored_at_startup)
{
	ui_test_bt_running(false);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(3000);
	CHECK(ui_test_bt_powered());
}

TEST(off_left_alone_at_startup)
{
	ui_test_bt_running(true); /* turned on elsewhere (bluetoothctl) */
	ui_test_start();
	ui_test_wait(3000);
	CHECK(ui_test_bt_powered());
}

TEST(devices_strongest_first)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -80, 0);
	ui_test_bt_device(HEADSET, "WH-1000XM4", -50, 0);
	ui_test_bt_device("11:22:33:44:55:66", NULL, -40, 0); /* no name: left out */
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("WH-1000XM4", 2000));
	CHECK(ui_test_shows("ThinkPad"));
	CHECK(y_of("WH-1000XM4") < y_of("ThinkPad"));
	CHECK(!ui_test_shows("11:22:33:44:55:66"));
	CHECK(!ui_test_shows("Paired devices"));
}

TEST(nothing_found_yet)
{
	ui_test_bt_running(true);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Searching...", 2000));
}

TEST(pair_without_code)
{
	ui_test_bt_running(true);
	ui_test_bt_device(HEADSET, "WH-1000XM4", -50, 0);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("WH-1000XM4", 2000));
	CHECK(ui_test_tap_text("WH-1000XM4"));
	CHECK(ui_test_wait_for("Connected", 3000));
	CHECK(ui_test_bt_is_paired(HEADSET));
	CHECK(ui_test_bt_is_connected(HEADSET));
	CHECK(ui_test_shows("Paired devices"));
	CHECK(y_of("Paired devices") < y_of("WH-1000XM4"));
	CHECK(y_of("WH-1000XM4") < y_of("Devices nearby"));
}

TEST(pair_with_code_confirmed)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 123456);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("123456", 3000));
	CHECK(ui_test_shows("Pair with ThinkPad?"));
	CHECK(!ui_test_bt_is_paired(LAPTOP));
	CHECK(ui_test_tap_text("Pair"));
	CHECK(ui_test_wait_for("Paired devices", 3000));
	CHECK(ui_test_bt_is_paired(LAPTOP));
	CHECK(!ui_test_shows("123456"));
}

TEST(pair_with_code_cancelled)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 654321);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("654321", 3000));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_wait_for("Could not pair with ThinkPad", 3000));
	CHECK(!ui_test_bt_is_paired(LAPTOP));
	CHECK(!ui_test_shows("654321"));
	CHECK(!ui_test_shows("Paired devices"));
}

/* Asked to pair without a code (RequestAuthorization): the question without one */
TEST(pair_authorized_without_code)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 0);
	ui_test_bt_pairing(LAPTOP, "RequestAuthorization", NULL);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("Pair with ThinkPad?", 3000));
	CHECK(ui_test_tap_text("Pair"));
	CHECK(ui_test_wait_for("Paired devices", 3000));
	CHECK(ui_test_bt_is_paired(LAPTOP));
	CHECK(!ui_test_shows("Pair with ThinkPad?"));
}

/* A keyboard: a code to type on it, no question; paired once typed */
static void pair_typing(const char *method, const char *code)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "K380", -60, 0);
	ui_test_bt_pairing(LAPTOP, method, code);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("K380", 2000));
	CHECK(ui_test_tap_text("K380"));
	CHECK(ui_test_wait_for("Type this code on K380", 3000));
	CHECK(ui_test_shows(code));
	CHECK(!ui_test_shows("Pair"));
	ui_test_bt_typed(LAPTOP);
	CHECK(ui_test_wait_for("Paired devices", 3000));
	CHECK(ui_test_bt_is_paired(LAPTOP));
	CHECK(!ui_test_shows("Type this code on K380"));
	CHECK(!ui_test_shows(code));
}

TEST(pair_typing_a_passkey)
{
	pair_typing("DisplayPasskey", "012345");
}

TEST(pair_typing_a_pin_code)
{
	pair_typing("DisplayPinCode", "0000");
}

/* A code to type on the phone: not yet, refused */
TEST(pair_asking_for_a_pin_code_refused)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "Car kit", -60, 0);
	ui_test_bt_pairing(LAPTOP, "RequestPinCode", NULL);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Car kit", 2000));
	CHECK(ui_test_tap_text("Car kit"));
	CHECK(ui_test_wait_for("Could not pair with Car kit", 3000));
	CHECK(!ui_test_bt_is_paired(LAPTOP));
}

/* The device gives up while the code is shown: the question goes */
TEST(pair_cancelled_by_the_device)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 123456);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("123456", 3000));
	ui_test_bt_cancel(LAPTOP);
	CHECK(ui_test_wait_for("Could not pair with ThinkPad", 3000));
	CHECK(!ui_test_shows("123456"));
	CHECK(!ui_test_shows("Pair with ThinkPad?"));
	CHECK(!ui_test_bt_is_paired(LAPTOP));
}

/* Leaving the page with the question open answers no */
TEST(pair_question_refused_on_leaving)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 123456);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("123456", 3000));
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(!ui_test_bt_is_paired(LAPTOP));
	CHECK(ui_test_tap_text("Bluetooth"));
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(!ui_test_shows("123456"));
	CHECK(!ui_test_shows("Paired devices"));
}

/* A phone paired: nothing to connect to, not left "Connecting..." */
TEST(paired_without_profiles_not_connected)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "Pixel 8", -60, 0);
	ui_test_bt_no_profiles(LAPTOP);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Pixel 8", 2000));
	CHECK(ui_test_tap_text("Pixel 8"));
	CHECK(ui_test_wait_for("Not connected", 3000));
	CHECK(ui_test_bt_is_paired(LAPTOP));
	CHECK(!ui_test_bt_is_connected(LAPTOP));
	CHECK(!ui_test_shows("Connecting..."));
}

/* Paired devices first, by name; then those nearby, strongest first */
TEST(paired_devices_by_name)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(LAPTOP, "Speaker", false);
	ui_test_bt_paired(HEADSET, "Headset", false);
	ui_test_bt_device("11:22:33:44:55:66", "Car kit", -30, 0);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Car kit", 2000));
	CHECK(y_of("Paired devices") < y_of("Headset"));
	CHECK(y_of("Headset") < y_of("Speaker"));
	CHECK(y_of("Speaker") < y_of("Devices nearby"));
	CHECK(y_of("Devices nearby") < y_of("Car kit"));
}

/* bluetoothd stopped (its agent released): not available, then back with the agent */
TEST(bluetoothd_stopped_and_back)
{
	ui_test_bt_running(true);
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("Visible as PinePhone", 2000));
	CHECK_STR(ui_test_bt_agent(), "DisplayYesNo");
	ui_test_bt_exit();
	CHECK(ui_test_wait_for("Not available", 3000));
	CHECK(!ui_test_shows("Devices nearby"));
	ui_test_bt_running(true);
	CHECK(ui_test_wait_for("Visible as PinePhone", 5000));
	CHECK(ui_test_wait_for("Devices nearby", 2000));
	CHECK_STR(ui_test_bt_agent(), "DisplayYesNo");
	CHECK(ui_test_wait_until(ui_test_bt_discovering, 6000)); /* set again every few seconds */
}

/* The system bus restarted while a code was shown: that pairing failed, the rest back */
TEST(bus_restarted)
{
	ui_test_bt_running(true);
	ui_test_bt_device(LAPTOP, "ThinkPad", -60, 123456);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	CHECK(open_bluetooth());
	CHECK(ui_test_wait_for("ThinkPad", 2000));
	CHECK(ui_test_tap_text("ThinkPad"));
	CHECK(ui_test_wait_for("123456", 3000));
	ui_test_bt_bus_lost();
	CHECK(ui_test_wait_for("Not available", 3000));
	CHECK(!ui_test_shows("123456"));
	ui_test_bt_running(true);
	CHECK(ui_test_wait_for("ThinkPad", 5000));
	CHECK(ui_test_shows("Could not pair with ThinkPad")); /* named as it was listed */
	CHECK_STR(ui_test_bt_agent(), "DisplayYesNo");
	CHECK(!ui_test_bt_is_paired(LAPTOP));
}

TEST(paired_device_disconnected_and_connected)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "WH-1000XM4", true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("WH-1000XM4")); /* the list's value: connected to it */
	CHECK(ui_test_tap_text("Bluetooth"));
	CHECK(ui_test_wait_for("Connected", 2000));
	CHECK(ui_test_tap_text("WH-1000XM4"));
	CHECK(ui_test_wait_for("Not connected", 3000));
	CHECK(!ui_test_bt_is_connected(HEADSET));
	CHECK(ui_test_tap_text("WH-1000XM4"));
	CHECK(ui_test_wait_for("Connected", 3000));
	CHECK(ui_test_bt_is_connected(HEADSET));
}

TEST(from_the_network_page)
{
	ui_test_bt_running(true);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Network"));
	CHECK(ui_test_shows("On"));
	CHECK(ui_test_tap_text("Bluetooth"));
	CHECK(ui_test_wait_for("Devices nearby", 2000));
}

/* The status bar: the Bluetooth symbol while on, dim until a device is connected */

static lv_obj_t *status_symbol(void)
{
	return ui_test_shows(UI_SYMBOL_BLUETOOTH_B) ? ui_test_find(UI_SYMBOL_BLUETOOTH_B) : NULL;
}

static bool lit(lv_obj_t *label)
{
	return lv_color_eq(lv_obj_get_style_text_color(label, 0), lv_color_hex(UI_COLOR_TEXT));
}

TEST(status_bar_none_while_off)
{
	ui_test_bt_running(false);
	ui_test_start();
	ui_test_wait(1100);
	CHECK(!status_symbol());
}

TEST(status_bar_dim_while_on)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(1100);
	lv_obj_t *s = status_symbol();
	CHECK(s && !lit(s));
}

TEST(status_bar_lit_while_connected)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "WH-1000XM4", true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(1100);
	lv_obj_t *s = status_symbol();
	CHECK(s && lit(s));
	ui_test_bt_connected(HEADSET, false);
	ui_test_wait(1100);
	s = status_symbol();
	CHECK(s && !lit(s));
}

TEST(status_bar_gone_once_off)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	CHECK(open_bluetooth());
	ui_test_wait(1100);
	CHECK(status_symbol() != NULL);
	lv_obj_t *sw = the_switch();
	CHECK(sw != NULL);
	if (!sw)
		return;
	ui_test_tap(sw);
	ui_test_wait(1100);
	CHECK(!status_symbol());
}

/* Beside Wi-Fi, USB staying the outermost: left of it */
TEST(status_bar_after_usb)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_write("/sys/class/net/usb0/carrier", "1\n");
	ui_test_start();
	ui_test_wait(1100);
	lv_obj_t *bt = status_symbol(), *usb = ui_test_find(UI_SYMBOL_USB_270);
	CHECK(bt && usb);
	if (!bt || !usb)
		return;
	lv_area_t b, u;
	lv_obj_get_coords(bt, &b);
	lv_obj_get_coords(usb, &u);
	CHECK(u.x2 < b.x1);
}

/* The rows a symbol's glyph covers on screen, as lv_draw_label places it */
static void glyph_rows(lv_obj_t *label, int32_t *top, int32_t *bottom)
{
	const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
	uint32_t i = 0, letter = lv_text_encoded_next(lv_label_get_text(label), &i);
	lv_font_glyph_dsc_t g;
	lv_font_get_glyph_dsc(font, &g, letter, 0);
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	*top = a.y1 + (font->line_height - font->base_line) - g.box_h - g.ofs_y;
	*bottom = *top + g.box_h - 1;
}

/* As tall as Wi-Fi and the mobile signal beside them: the 24 rows in the bar's middle */
TEST(status_bar_symbols_as_tall_as_wifi)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_write("/sys/class/net/usb0/carrier", "1\n");
	ui_test_start();
	ui_test_wait(1100);
	lv_obj_t *bt = status_symbol(), *usb = ui_test_find(UI_SYMBOL_USB_270);
	CHECK(bt && usb);
	if (!bt || !usb)
		return;
	lv_area_t bar;
	lv_obj_get_coords(lv_obj_get_parent(bt), &bar);
	int32_t wifi_top = bar.y1 + (lv_area_get_height(&bar) - 24) / 2, wifi_bottom = wifi_top + 23;
	int32_t top, bottom;
	glyph_rows(bt, &top, &bottom);
	CHECK_INT(top, wifi_top);
	CHECK_INT(bottom, wifi_bottom);
	/* 25 rows in the 20 px font (no size has 24): one more at the top */
	glyph_rows(usb, &top, &bottom);
	CHECK(top >= wifi_top - 1 && top <= wifi_top);
	CHECK_INT(bottom, wifi_bottom);
}

/* Quick settings: a Bluetooth tile beside Wi-Fi's, as it: a tap turns it on or off, a long
 * press opens its settings, labelled with the device connected */

static void shade_open(void)
{
	ui_test_tap_at(360, 20);
	ui_test_settle();
}

/* The tile whose label shows `text` */
static lv_obj_t *tile(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	return label ? lv_obj_get_parent(label) : NULL;
}

TEST(quick_tile_turns_it_on_and_off)
{
	ui_test_bt_running(false);
	ui_test_start();
	shade_open();
	lv_obj_t *t = tile("Bluetooth");
	CHECK(t && !lv_obj_has_state(t, LV_STATE_CHECKED) && !lv_obj_has_state(t, LV_STATE_DISABLED));
	CHECK(ui_test_tap_text("Bluetooth"));
	CHECK(ui_test_bt_powered());
	CHECK(strstr(file(SETTINGS), "bluetooth=1") != NULL);
	t = tile("Bluetooth");
	CHECK(t && lv_obj_has_state(t, LV_STATE_CHECKED));
	CHECK(ui_test_tap_text("Bluetooth"));
	CHECK(!ui_test_bt_powered());
	CHECK(strstr(file(SETTINGS), "bluetooth=0") != NULL);
}

TEST(quick_tile_names_the_device)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "WH-1000XM4", true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(1100);
	shade_open();
	lv_obj_t *t = tile("WH-1000XM4");
	CHECK(t && lv_obj_has_state(t, LV_STATE_CHECKED));
	ui_test_bt_connected(HEADSET, false);
	ui_test_wait(1100);
	CHECK(ui_test_shows("Bluetooth"));
	CHECK(!ui_test_shows("WH-1000XM4"));
}

TEST(quick_tile_without_bluetoothd)
{
	ui_test_start();
	shade_open();
	lv_obj_t *t = tile("Bluetooth");
	CHECK(t && lv_obj_has_state(t, LV_STATE_DISABLED));
}

TEST(quick_tile_long_press_opens_settings)
{
	ui_test_bt_running(true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	shade_open();
	CHECK(ui_test_long_press_text("Bluetooth"));
	CHECK(ui_test_wait_for("Devices nearby", 2000));
}

TEST(quick_tile_beside_wifi)
{
	ui_test_bt_running(false);
	ui_test_start();
	shade_open();
	lv_obj_t *bt = tile("Bluetooth"), *wifi = tile("Wi-Fi");
	CHECK(bt && wifi);
	if (!bt || !wifi)
		return;
	lv_area_t b, w;
	lv_obj_get_coords(bt, &b);
	lv_obj_get_coords(wifi, &w);
	CHECK_INT(b.y1, w.y1);
	CHECK(b.x1 > w.x2 && b.x1 - w.x2 < 40);
}

/* A name too long for one line: wrapped onto two inside the tile, none of it cut off */
TEST(quick_tile_long_name)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "soundcore R50i Pro Max", true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(1100);
	shade_open();
	lv_obj_t *label = ui_test_find("soundcore R50i Pro Max");
	CHECK(label != NULL);
	if (!label)
		return;
	lv_obj_t *t = lv_obj_get_parent(label);
	lv_area_t l, a;
	lv_obj_get_coords(label, &l);
	lv_obj_get_coords(t, &a);
	const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
	lv_point_t one_line;
	lv_text_get_size(&one_line, "soundcore R50i Pro Max", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	CHECK(lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_CLIP || one_line.x <= lv_area_get_width(&l));
	CHECK(lv_area_get_height(&l) >= 2 * lv_font_get_line_height(font)); /* wrapped */
	CHECK(l.y1 >= a.y1 && l.y2 <= a.y2 && l.x1 >= a.x1 && l.x2 <= a.x2);
}

/* Longer still: two lines, the rest cut, inside the tile */
TEST(quick_tile_very_long_name)
{
	ui_test_bt_running(true);
	ui_test_bt_paired(HEADSET, "Very Long Bluetooth Speaker Name For The Living Room", true);
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_start();
	ui_test_wait(1100);
	shade_open();
	lv_obj_t *label = ui_test_find("Very Long Bluetooth Speaker Name For The Living Room");
	CHECK(label != NULL);
	if (!label)
		return;
	lv_area_t l, a;
	lv_obj_get_coords(label, &l);
	lv_obj_get_coords(lv_obj_get_parent(label), &a);
	CHECK_INT(lv_area_get_height(&l), 2 * lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
	CHECK(l.y1 >= a.y1 && l.y2 <= a.y2);
}
