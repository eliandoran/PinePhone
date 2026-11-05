/*
 * The status bar (ui.c) and the quick settings pulled down from it (shade.c): the
 * battery and the USB network in both, the sliders and toggles of the panel.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_symbols.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define BATTERY UI_BATTERY "/"
#define BACKLIGHT "/sys/class/backlight/backlight/brightness"
#define TORCH "/sys/class/leds/white:flash/brightness"
#define CARRIER "/sys/class/net/usb0/carrier"

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
}

static bool shade_open(void)
{
	return ui_test_shows("Vibration");
}

/* Tapped on the status bar: opens, tapped again: closes */
static void bar_tap(void)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
}

/* The battery icons shown (ui_battery_icon_create()), at most max */
static unsigned batteries(lv_obj_t **found, unsigned max)
{
	static lv_obj_t *all[1024];
	unsigned n = ui_test_find_widgets(&lv_obj_class, all, 1024), k = 0;
	for (unsigned i = 0; i < n && i < 1024 && k < max; i++)
		if (ui_battery_icon_get(all[i], NULL, NULL))
			found[k++] = all[i];
	return k;
}

/* The status bar's battery icon, NULL if none */
static lv_obj_t *bar_battery(void)
{
	lv_obj_t *found[4];
	unsigned n = batteries(found, 4);
	for (unsigned i = 0; i < n; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H)
			return found[i];
	}
	return NULL;
}

/* The icon's level (UI_BATTERY_UNKNOWN, _ABSENT) and charging, -100 without an icon */
static int bar_level(bool *charging)
{
	int level;
	lv_obj_t *icon = bar_battery();
	return icon && ui_battery_icon_get(icon, &level, charging) ? level : -100;
}

/* The fill's color at its left end (the bolt is in the middle) */
static lv_color_t fill_color(lv_obj_t *icon)
{
	lv_area_t a;
	lv_obj_get_coords(icon, &a);
	return ui_test_pixel(a.x1 + 4, (a.y1 + a.y2) / 2);
}

/* The percentage beside the battery in the bar (Settings, Battery), off by default */
static void percentage_on(void)
{
	ui_test_write(SETTINGS, "battery_percent=1\n");
}

/* Status bar */

TEST(status_bar)
{
	ui_test_start();
	CHECK(ui_test_shows(LV_SYMBOL_WARNING)); /* no modem: a badge, not a text */
	CHECK(!ui_test_shows("No modem"));
	CHECK(!ui_test_shows("76%")); /* the battery alone: its fill shows the level */
	bool charging = true;
	CHECK_INT(bar_level(&charging), 76);
	CHECK(!charging);
	CHECK(!ui_test_shows(LV_SYMBOL_USB)); /* not plugged */
	char now[8];
	time_t t = time(NULL);
	strftime(now, sizeof(now), "%H:%M", localtime(&t));
	lv_obj_t *found[4];
	CHECK(ui_test_find_all(now, found, 4) >= 1); /* and the home screen's clock */
}

TEST(battery_charging_low)
{
	percentage_on();
	ui_test_write(BATTERY "capacity", "9\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_start();
	/* The bolt is in the battery, not a glyph of its own */
	CHECK(!ui_test_shows_part(LV_SYMBOL_CHARGE));
	lv_obj_t *label = ui_test_find("9%");
	CHECK(label != NULL);
	if (label) /* the text stays in the text color */
		CHECK_INT(lv_color_to_u32(lv_obj_get_style_text_color(label, 0)) & 0xffffff,
		          lv_color_to_u32(lv_color_hex(UI_COLOR_TEXT)) & 0xffffff);
	bool charging = false;
	CHECK_INT(bar_level(&charging), 9);
	CHECK(charging);
	lv_obj_t *icon = bar_battery();
	if (icon) { /* green while charging, even low */
		lv_color_t c = fill_color(icon);
		CHECK(c.green > 0xa0 && c.red < 0x80);
	}
	bar_tap();
	CHECK(ui_test_shows("9%, Charging"));
}

TEST(battery_low_red)
{
	percentage_on();
	ui_test_write(BATTERY "capacity", "9\n");
	ui_test_start();
	lv_obj_t *icon = bar_battery();
	CHECK(icon != NULL);
	if (icon) {
		lv_color_t c = fill_color(icon);
		CHECK(c.red > 0xe0 && c.green < 0x80);
	}
	lv_obj_t *label = ui_test_find("9%");
	CHECK(label != NULL);
	if (label)
		CHECK_INT(lv_color_to_u32(lv_obj_get_style_text_color(label, 0)) & 0xffffff,
		          lv_color_to_u32(lv_color_hex(UI_COLOR_TEXT)) & 0xffffff);
}

/* Filled in the text color as far as the level reaches, not past it */
TEST(battery_fill_follows_the_level)
{
	ui_test_write(BATTERY "capacity", "50\n");
	ui_test_start();
	lv_obj_t *icon = bar_battery();
	CHECK(icon != NULL);
	if (!icon)
		return;
	lv_area_t a;
	lv_obj_get_coords(icon, &a);
	int32_t y = (a.y1 + a.y2) / 2, w = lv_area_get_width(&a);
	lv_color_t text = lv_color_hex(UI_COLOR_TEXT);
	lv_color_t in = ui_test_pixel(a.x1 + w / 3, y), out = ui_test_pixel(a.x1 + w * 3 / 4, y);
	CHECK(abs(in.red - text.red) < 0x20 && abs(in.green - text.green) < 0x20);
	CHECK(abs(out.red - text.red) > 0x40 || abs(out.green - text.green) > 0x40);
}

/* The time to full of Settings, Battery, on the battery's line, which shows whole */
TEST(battery_time_to_full)
{
	ui_test_write(BATTERY "capacity", "50\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "1375000\n");
	ui_test_start();
	ui_test_wait(22000); /* the charger settles after plugging in */
	bar_tap();
	lv_obj_t *label = ui_test_find("50%, Charging \xc2\xb7 full in about 1 h");
	CHECK(label != NULL);
	if (!label)
		return;
	/* Not clipped by the panel or its row */
	lv_area_t a, clip;
	lv_obj_get_coords(label, &a);
	for (lv_obj_t *p = lv_obj_get_parent(label); p; p = lv_obj_get_parent(p)) {
		lv_obj_get_coords(p, &clip);
		CHECK(a.y1 >= clip.y1 && a.y2 <= clip.y2);
	}
}

TEST(battery_levels_in_the_bar)
{
	/* Every percent, not steps of the glyphs */
	static const struct {
		const char *capacity, *shown;
		int level;
	} levels[] = {
		{ "100\n", "100%", 100 },
		{ "64\n", "64%", 64 },
		{ "39\n", "39%", 39 },
		{ "3\n", "3%", 3 },
	};
	percentage_on();
	ui_test_start();
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		ui_test_write(BATTERY "capacity", levels[i].capacity);
		ui_test_wait(1100); /* the home screen's update */
		CHECK(ui_test_shows(levels[i].shown));
		CHECK_INT(bar_level(NULL), levels[i].level);
	}
}

TEST(no_battery_driver)
{
	percentage_on();
	remove(BATTERY "capacity");
	ui_test_start();
	CHECK_INT(bar_level(NULL), UI_BATTERY_UNKNOWN);
	CHECK(!ui_test_shows_part("%"));
	bar_tap();
	CHECK(ui_test_shows("No battery driver"));
}

/* The AXP803 reports a dead cell as present: its voltage tells */
TEST(dead_battery_crossed_out)
{
	percentage_on();
	ui_test_write(BATTERY "capacity", "0\n");
	ui_test_write(BATTERY "voltage_now", "12000\n");
	ui_test_start();
	CHECK_INT(bar_level(NULL), UI_BATTERY_ABSENT);
	CHECK(!ui_test_shows("0%"));
	bar_tap();
	CHECK(ui_test_shows("No usable battery (0.0 V)"));
}

TEST(battery_not_present)
{
	ui_test_write(BATTERY "present", "0\n");
	remove(BATTERY "voltage_now");
	ui_test_start();
	CHECK_INT(bar_level(NULL), UI_BATTERY_ABSENT);
	bar_tap();
	CHECK(ui_test_shows("No usable battery"));
}

/* The battery comes back (a new one, the driver's state): the cross goes */
TEST(battery_back_after_absent)
{
	percentage_on();
	ui_test_write(BATTERY "present", "0\n");
	ui_test_start();
	CHECK_INT(bar_level(NULL), UI_BATTERY_ABSENT);
	ui_test_write(BATTERY "present", "1\n");
	ui_test_wait(1100);
	CHECK_INT(bar_level(NULL), 76);
	CHECK(ui_test_shows("76%"));
}

/* The shade's battery row shows the same icon as the bar, at the same level */
TEST(shade_battery_icon)
{
	ui_test_write(BATTERY "capacity", "39\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_start();
	bar_tap();
	CHECK(shade_open());
	lv_obj_t *found[4];
	unsigned n = batteries(found, 4);
	CHECK_INT(n, 2);
	for (unsigned i = 0; i < n; i++) {
		int level;
		bool charging;
		CHECK(ui_battery_icon_get(found[i], &level, &charging));
		CHECK_INT(level, 39);
		CHECK(charging);
	}
}

/* Plugged into the host, which does not share its internet (no network here) */
TEST(usb_network)
{
	ui_test_write(CARRIER, "1\n");
	ui_test_start();
	CHECK(ui_test_shows(LV_SYMBOL_USB));
	bar_tap();
	CHECK(ui_test_shows("Connected, no internet (just share-internet)"));
	ui_test_write(CARRIER, "0\n");
	ui_test_wait(1100);
	CHECK(ui_test_shows("Not connected"));
}

/* Opening and closing */

TEST(tapped_open_and_closed)
{
	ui_test_start();
	bar_tap();
	CHECK(shade_open());
	CHECK(ui_test_shows("Not connected"));
	CHECK(ui_test_shows("76%, Discharging"));
	bar_tap();
	CHECK(!shade_open());
}

/* Below the panel, the screen is dimmed while it is open: in the light theme the
 * white panel over Settings' white cards had no edge */
static int brightness(int32_t x, int32_t y)
{
	lv_color_t c = ui_test_pixel(x, y);
	return c.red + c.green + c.blue;
}

TEST(screen_dimmed_below)
{
	ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	int before = brightness(360, 1300);
	bar_tap();
	CHECK(shade_open());
	int open = brightness(360, 1300);
	if (open >= before * 3 / 4)
		TEST_FAIL("below the panel: brightness %d, before %d", open, before);
	bar_tap();
	CHECK(!shade_open());
	CHECK_INT(brightness(360, 1300), before);
}

TEST(a_tap_below_closes_it)
{
	ui_test_start();
	bar_tap();
	CHECK(shade_open());
	ui_test_tap_at(106, 1348); /* the dock's Phone, under it */
	CHECK(!shade_open());
	CHECK(ui_at_home()); /* the tap did not reach it */
}

TEST(pulled_down_and_pushed_up)
{
	ui_test_start();
	ui_test_swipe(360, 20, 360, 900, 600);
	CHECK(shade_open());
	ui_test_swipe(360, 700, 360, 100, 600); /* on the panel */
	CHECK(!shade_open());
}

TEST(pulled_a_little_it_goes_back)
{
	ui_test_start();
	ui_test_swipe(360, 20, 360, 200, 600);
	CHECK(!shade_open());
}

TEST(flicked_down_it_opens)
{
	ui_test_start();
	ui_test_swipe(360, 20, 360, 200, 48);
	CHECK(shade_open());
}

/* A finger from (x, y) by (dx, dy) each reading, n readings, then lifted */
static void flick(int32_t x, int32_t y, int32_t dx, int32_t dy, int n)
{
	lv_test_mouse_move_to(x, y);
	lv_test_mouse_press();
	ui_test_wait(16);
	for (int i = 1; i <= n; i++) {
		lv_test_mouse_move_to(x + i * dx, y + i * dy);
		ui_test_wait(16);
	}
	lv_test_mouse_release();
	ui_test_settle();
}

/* A quick flick moves up to ~300 px between two readings (seen on the phone):
 * not a jump */
TEST(fast_flick_down_opens)
{
	ui_test_start();
	flick(360, 20, 20, 250, 2);
	CHECK(shade_open());
}

TEST(fast_flick_up_closes)
{
	ui_test_start();
	bar_tap();
	CHECK(shade_open());
	/* From the battery's line: the panel's background, not a control */
	lv_obj_t *battery = ui_test_find("76%, Discharging");
	CHECK(battery != NULL);
	lv_area_t a;
	lv_obj_get_coords(battery, &a);
	flick(300, (a.y1 + a.y2) / 2, -30, -200, 2);
	CHECK(!shade_open());
}

/* Far sideways in one reading: another finger */
TEST(a_sideways_jump_is_not_a_pull)
{
	ui_test_start();
	lv_test_mouse_move_to(100, 20);
	lv_test_mouse_press();
	ui_test_wait(16);
	lv_test_mouse_move_to(100, 300);
	ui_test_wait(16);
	lv_test_mouse_move_to(100, 400);
	ui_test_wait(16);
	lv_test_mouse_move_to(600, 1000);
	ui_test_wait(16);
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(!shade_open());
}

/* The pointer reading jumped (another finger): the pull is called off */
TEST(a_jump_is_not_a_pull)
{
	ui_test_start();
	lv_test_mouse_move_to(360, 20);
	lv_test_mouse_press();
	ui_test_wait(16);
	for (int y = 40; y <= 400; y += 40) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	lv_test_mouse_move_to(360, 1200);
	ui_test_wait(16);
	lv_test_mouse_move_to(360, 1250); /* ignored until the finger lifts */
	ui_test_wait(16);
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(!shade_open());
}

/* Controls */

static lv_obj_t *slider(unsigned i)
{
	lv_obj_t *found[2] = { NULL, NULL };
	ui_test_find_widgets(&lv_slider_class, found, 2);
	return found[i];
}

/* Its knob dragged to the left end */
static void slide_to_start(lv_obj_t *s)
{
	lv_area_t a;
	lv_obj_get_coords(s, &a);
	int32_t x = a.x1 + (int32_t)((a.x2 - a.x1) * lv_slider_get_value(s) / lv_slider_get_max_value(s));
	ui_test_swipe(x, (a.y1 + a.y2) / 2, a.x1 - 40, (a.y1 + a.y2) / 2, 600);
}

TEST(brightness_slider)
{
	ui_test_start();
	bar_tap();
	lv_obj_t *s = slider(0);
	CHECK(s != NULL);
	if (!s)
		return;
	CHECK_INT(lv_slider_get_value(s), 50);
	slide_to_start(s);
	CHECK(shade_open()); /* a drag on the slider does not close it */
	CHECK_INT(lv_slider_get_value(s), UI_BRIGHTNESS_MIN);
	CHECK_STR(file(BACKLIGHT), "50\n");
	CHECK_STR(file(SETTINGS), "brightness=5\n");
}

/* The slider's sun: Font Awesome's, solid from half the brightness, regular (an outline)
 * below; both glyphs of the label's own font (symbols.txt) */
TEST(brightness_sun)
{
	ui_test_start();
	bar_tap();
	lv_obj_t *s = slider(0);
	CHECK(s != NULL);
	if (!s)
		return;
	lv_obj_t *sun = lv_obj_get_child(lv_obj_get_parent(s), 0);
	CHECK(lv_obj_check_type(sun, &lv_label_class));
	if (!lv_obj_check_type(sun, &lv_label_class))
		return;
	CHECK_STR(lv_label_get_text(sun), UI_SYMBOL_SUN);
	slide_to_start(s);
	CHECK_STR(lv_label_get_text(sun), UI_SYMBOL_SUN_REGULAR);

	const lv_font_t *font = lv_obj_get_style_text_font(sun, LV_PART_MAIN);
	lv_font_glyph_dsc_t glyph;
	uint32_t codepoints[] = { 0xF185, 0xFF185 };
	for (unsigned i = 0; i < 2; i++) {
		CHECK(lv_font_get_glyph_dsc(font, &glyph, codepoints[i], 0));
		CHECK(glyph.resolved_font == font);
		CHECK(glyph.box_w > 0 && glyph.box_h > 0);
	}
}

TEST(volume_slider)
{
	ui_test_start();
	bar_tap();
	lv_obj_t *s = slider(1);
	CHECK(s != NULL);
	if (!s)
		return;
	CHECK_INT(lv_slider_get_value(s), 12);
	CHECK(ui_test_shows(LV_SYMBOL_VOLUME_MAX));
	slide_to_start(s);
	CHECK_INT(ui_volume_get(), 0);
	CHECK(ui_test_shows(LV_SYMBOL_MUTE));
	CHECK_STR(file(SETTINGS), "volume=0\n");
}

/* The volume keys move its slider */
TEST(volume_keys_move_the_slider)
{
	ui_test_start();
	bar_tap();
	ui_test_button_press(KEY_VOLUMEDOWN, 50);
	ui_test_wait(1100);
	lv_obj_t *s = slider(1);
	CHECK(s && lv_slider_get_value(s) == 11);
}

TEST(vibration_toggle)
{
	ui_test_start();
	CHECK(ui_haptics);
	bar_tap();
	CHECK(ui_test_tap_text("Vibration"));
	CHECK(!ui_haptics);
	CHECK_STR(file(SETTINGS), "haptics=0\n");
	CHECK(shade_open()); /* toggles leave it open */
	CHECK(ui_test_tap_text("Vibration"));
	CHECK(ui_haptics);
}

TEST(flashlight)
{
	ui_test_write(TORCH, "0\n");
	ui_test_start();
	bar_tap();
	CHECK(ui_test_tap_text("Flashlight"));
	CHECK_STR(file(TORCH), "1\n");
	/* Still on once closed, and shown on at the next opening */
	bar_tap();
	bar_tap();
	lv_obj_t *label = ui_test_find("Flashlight");
	CHECK(label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_CHECKED));
	CHECK(ui_test_tap_text("Flashlight"));
	CHECK_STR(file(TORCH), "0\n");
}

TEST(no_flash_led)
{
	ui_test_start();
	bar_tap();
	lv_obj_t *label = ui_test_find("Flashlight");
	CHECK(label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_DISABLED));
}

TEST(light_theme)
{
	ui_test_start();
	CHECK(!ui_light);
	bar_tap();
	CHECK(ui_test_tap_text("Light theme"));
	ui_test_settle();
	CHECK(ui_light);
	CHECK(!shade_open()); /* gone with the screens rebuilt */
	CHECK_STR(file(SETTINGS), "light=1\n");
	bar_tap();
	CHECK(ui_test_tap_text("Light theme"));
	ui_test_settle();
	CHECK(!ui_light);
}

/* It would rebuild the screens: only at home */
TEST(no_theme_switch_in_an_app)
{
	CHECK(ui_test_open("Notes"));
	bar_tap();
	lv_obj_t *label = ui_test_find("Light theme");
	CHECK(label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_DISABLED));
	CHECK(ui_test_tap_text("Light theme"));
	CHECK(!ui_light);
	CHECK(ui_test_shows("Notes"));
}

/* Over the lock screen, without the theme switch */
TEST(over_the_lock_screen)
{
	ui_test_write("/sys/power/state", "");
	ui_test_start();
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_locked());
	bar_tap();
	CHECK(shade_open());
	lv_obj_t *label = ui_test_find("Light theme");
	CHECK(label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_DISABLED));
	bar_tap();
	CHECK(ui_locked());
}

/* Turning with the phone (turn.c): home turns, the panel fits the display */

/* The panel: the top layer's child holding its tiles */
static lv_obj_t *panel(void)
{
	lv_obj_t *obj = ui_test_find("Vibration");
	while (obj && lv_obj_get_parent(obj) != lv_layer_top())
		obj = lv_obj_get_parent(obj);
	return obj;
}

/* Gravity along the phone's x axis */
static void hold_sideways(void)
{
	const float up[3] = { 1, 0, 0 };
	const float field[3] = { 0.2f, 0, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000); /* read a few times */
}

/* bar_tap() sideways: the pointer is the touchscreen's, which LVGL turns back
 * counterclockwise (turn.c) */
static void bar_tap_turned(void)
{
	int32_t x = 360, y = UI_STATUS_H / 2;
	switch (lv_display_get_rotation(lv_display_get_default())) {
	case LV_DISPLAY_ROTATION_90:
		ui_test_tap_at(719 - y, x);
		break;
	case LV_DISPLAY_ROTATION_270:
		ui_test_tap_at(y, 1439 - x);
		break;
	default:
		ui_test_tap_at(x, y);
	}
}

static void panel_fits(int32_t w)
{
	lv_obj_t *p = panel();
	CHECK(p != NULL);
	if (!p)
		return;
	lv_area_t a;
	lv_obj_get_coords(p, &a);
	CHECK_INT(lv_area_get_width(&a), w);
	CHECK_INT(a.x1, 0);
	CHECK(a.y2 < lv_display_get_vertical_resolution(lv_display_get_default()));
}

/* Opened sideways, then asleep and awake: upright behind the lock screen, the panel
 * was left 1440 px wide */
TEST(upright_after_waking_while_open)
{
	ui_test_sensors(true, false);
	ui_test_start();
	hold_sideways();
	CHECK(ui_sideways());
	bar_tap_turned();
	CHECK(shade_open());
	panel_fits(1440);
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300); /* the fade out, the lock screen shown, the sleep */
	ui_test_settle();
	CHECK(ui_locked());
	CHECK(!ui_sideways());
	CHECK(shade_open());
	panel_fits(720);
	bar_tap();
	CHECK(!shade_open());
}

TEST(turned_sideways_while_open)
{
	ui_test_sensors(true, false);
	ui_test_start();
	bar_tap();
	CHECK(shade_open());
	panel_fits(720);
	hold_sideways();
	CHECK(ui_sideways());
	panel_fits(1440);
	bar_tap_turned();
	CHECK(!shade_open());
}

/* Rotation lock: the display stays as it was when locked, whichever way the phone is held */

static void hold_upright(void)
{
	const float up[3] = { 0, 1, 0 };
	const float field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000);
}

static bool rotation_locked(void)
{
	lv_obj_t *label = ui_test_find("Rotation lock");
	return label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_CHECKED);
}

TEST(rotation_lock_upright)
{
	ui_test_sensors(true, false);
	ui_test_start();
	bar_tap();
	CHECK(!rotation_locked());
	CHECK(ui_test_tap_text("Rotation lock"));
	CHECK(rotation_locked());
	CHECK(strstr(file(SETTINGS), "rotation_lock=0\n"));
	bar_tap();
	hold_sideways();
	CHECK(!ui_sideways());

	bar_tap();
	CHECK(ui_test_tap_text("Rotation lock"));
	CHECK(!rotation_locked());
	CHECK(strstr(file(SETTINGS), "rotation_lock=-1\n"));
	bar_tap();
	hold_sideways();
	CHECK(ui_sideways());
}

TEST(rotation_lock_sideways)
{
	ui_test_sensors(true, false);
	ui_test_start();
	hold_sideways();
	CHECK_INT(lv_display_get_rotation(lv_display_get_default()), LV_DISPLAY_ROTATION_90);
	bar_tap_turned();
	CHECK(ui_test_tap_text("Rotation lock"));
	CHECK(rotation_locked());
	CHECK(strstr(file(SETTINGS), "rotation_lock=1\n"));
	bar_tap_turned();
	CHECK(!shade_open());
	hold_upright();
	CHECK_INT(lv_display_get_rotation(lv_display_get_default()), LV_DISPLAY_ROTATION_90);

	/* An app that does not turn is still upright, and home sideways again after it */
	CHECK(ui_test_open("Notes"));
	CHECK(!ui_sideways());
	CHECK(ui_test_back());
	ui_test_wait(1000);
	CHECK_INT(lv_display_get_rotation(lv_display_get_default()), LV_DISPLAY_ROTATION_90);
}

TEST(rotation_lock_kept)
{
	ui_test_write(SETTINGS, "rotation_lock=3\n");
	ui_test_sensors(true, false);
	ui_test_start();
	hold_upright();
	CHECK_INT(lv_display_get_rotation(lv_display_get_default()), LV_DISPLAY_ROTATION_270);
	bar_tap_turned();
	CHECK(rotation_locked());
}

/* Held, a tile grows (the theme's pressed style): at the edge of its row too, not cut */
TEST(held_tile_not_clipped)
{
	ui_test_start();
	bar_tap();
	lv_obj_t *label = ui_test_find("Vibration");
	CHECK(label);
	if (!label)
		return;
	/* The tile reaching furthest right */
	lv_obj_t *tiles = lv_obj_get_parent(lv_obj_get_parent(label)), *tile = NULL;
	lv_area_t a = { 0 };
	for (uint32_t i = 0; i < lv_obj_get_child_count(tiles); i++) {
		lv_area_t c;
		lv_obj_get_coords(lv_obj_get_child(tiles, i), &c);
		if (!tile || c.x2 > a.x2)
			tile = lv_obj_get_child(tiles, i), a = c;
	}
	/* Just past the row, which the tile reaches as it grows */
	lv_area_t row;
	lv_obj_get_coords(tiles, &row);
	int32_t x = row.x2 + 1, y = (a.y1 + a.y2) / 2;
	lv_color_t before = ui_test_pixel(x, y);
	lv_test_mouse_move_to((a.x1 + a.x2) / 2, y);
	lv_test_mouse_press();
	ui_test_wait(300);
	lv_color_t held = ui_test_pixel(x, y);
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(!lv_color_eq(held, before));
}
