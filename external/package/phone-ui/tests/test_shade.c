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
#define USB_POWER "/sys/class/power_supply/axp20x-usb/"

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
	return ui_test_shows("Flashlight");
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

/* A label with that text in the status bar (the shade's Flashlight tile has the bolt
 * too), NULL if none */
static lv_obj_t *in_bar(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y1 >= 0 && a.y2 <= UI_STATUS_H && !lv_obj_is_hidden(found[i]))
			return found[i];
	}
	return NULL;
}

/* Pixels of the icon's inside, from `from` to `to` of its width (in hundredths), its
 * middle rows, within 0x30 of `color` */
static unsigned pixels_like(lv_obj_t *icon, int from, int to, lv_color_t color)
{
	lv_area_t a;
	lv_obj_get_coords(icon, &a);
	int32_t w = lv_area_get_width(&a);
	unsigned n = 0;
	for (int32_t y = a.y1 + 5; y <= a.y2 - 5; y++)
		for (int32_t x = a.x1 + w * from / 100; x < a.x1 + w * to / 100; x++) {
			lv_color_t c = ui_test_pixel(x, y);
			n += abs(c.red - color.red) < 0x30 && abs(c.green - color.green) < 0x30 &&
			     abs(c.blue - color.blue) < 0x30;
		}
	return n;
}

/* The percentage in the bar's battery (Settings, Battery), off by default */
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
	CHECK(!ui_test_shows(UI_SYMBOL_USB_270) && !ui_test_shows(LV_SYMBOL_USB)); /* not plugged */
	char now[8];
	time_t t = time(NULL);
	strftime(now, sizeof(now), "%H:%M", localtime(&t));
	lv_obj_t *found[4];
	CHECK(ui_test_find_all(now, found, 4) >= 1); /* and the home screen's clock */
}

/* Without the percentage the bolt is in the battery, not a glyph of its own */
TEST(battery_charging_bolt_inside)
{
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_start();
	CHECK(!in_bar(LV_SYMBOL_CHARGE));
	bool charging = false;
	CHECK_INT(bar_level(&charging), 76);
	CHECK(charging);
}

/* With the percentage, the number is in the battery (no label of its own) and the
 * bolt after it, in the bar's padding: nothing moves when plugged in */
TEST(battery_charging_low)
{
	percentage_on();
	ui_test_write(BATTERY "capacity", "9\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_start();
	CHECK(!ui_test_shows("9%"));
	lv_obj_t *bolt = in_bar(LV_SYMBOL_CHARGE), *icon = bar_battery();
	CHECK(bolt && icon);
	if (bolt && icon) { /* in the text color, right of the battery, on the screen */
		CHECK_INT(lv_color_to_u32(lv_obj_get_style_text_color(bolt, 0)) & 0xffffff,
		          lv_color_to_u32(lv_color_hex(UI_COLOR_TEXT)) & 0xffffff);
		lv_area_t b, i;
		lv_obj_get_coords(bolt, &b);
		lv_obj_get_coords(icon, &i);
		CHECK(b.x1 > i.x2 && b.x1 < i.x2 + 8 && b.x2 < 720);
	}
	bool charging = false;
	CHECK_INT(bar_level(&charging), 9);
	CHECK(charging);
	if (icon) { /* green while charging, even low; the number in the text color past the fill */
		lv_color_t c = fill_color(icon);
		CHECK(c.green > 0xa0 && c.red < 0x80);
		CHECK(pixels_like(icon, 30, 80, lv_color_hex(UI_COLOR_TEXT)) > 10);
	}
	bar_tap();
	CHECK(ui_test_shows("9%, Charging"));
	/* Unplugged: the bolt goes */
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_wait(1100);
	CHECK(!in_bar(LV_SYMBOL_CHARGE));
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
	CHECK(!ui_test_shows("9%"));
	CHECK(!in_bar(LV_SYMBOL_CHARGE));
}

/* The number over the fill is cut out of it (the shade's color), not in the fill's */
TEST(battery_percentage_in_the_fill)
{
	ui_test_start();
	lv_obj_t *icon = bar_battery();
	CHECK(icon != NULL);
	if (!icon)
		return;
	CHECK_INT(pixels_like(icon, 15, 60, lv_color_hex(UI_COLOR_SHADE)), 0); /* off: filled whole */
	int32_t w = lv_obj_get_width(icon);
	ui_setting_set_int("battery_percent", 1);
	ui_test_wait(1100);
	CHECK(lv_obj_get_width(icon) > w); /* wider, for "100" */
	CHECK(pixels_like(icon, 15, 60, lv_color_hex(UI_COLOR_SHADE)) > 10);
	CHECK(!ui_test_shows("76%"));
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
		CHECK(!ui_test_shows(levels[i].shown)); /* drawn in the icon */
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

/* Both icons (the bar's and the shade's) plugged in, or not */
static bool icons_plugged(bool plugged)
{
	lv_obj_t *found[4];
	unsigned n = batteries(found, 4);
	bool ok = n == 2;
	for (unsigned i = 0; i < n; i++) {
		bool charging;
		ok = ok && ui_battery_icon_get(found[i], NULL, &charging) && charging == plugged;
	}
	return ok;
}

/* Full on the charger, the AXP803 stops charging and the battery tops up the input with a
 * few mA: its driver then says "Discharging". Plugged in, that is "Full" at 100% and "Not
 * charging" below (it waits for the voltage to drop before charging again), with the bolt. */
TEST(battery_full_on_the_charger)
{
	ui_test_write(USB_POWER "online", "1\n");
	ui_test_write(BATTERY "capacity", "100\n");
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "current_now", "-33000\n");
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("100%, Full"));
	CHECK(icons_plugged(true));

	ui_test_write(BATTERY "capacity", "99\n");
	ui_test_wait(1100);
	CHECK(ui_test_shows("99%, Not charging"));
	CHECK(icons_plugged(true));

	/* Unplugged */
	ui_test_write(USB_POWER "online", "0\n");
	ui_test_wait(1100);
	CHECK(ui_test_shows("99%, Discharging"));
	CHECK(icons_plugged(false));
}

/* Plugged into the host, which does not share its internet (no network here) */
TEST(usb_network)
{
	ui_test_write(CARRIER, "1\n");
	ui_test_start();
	CHECK(ui_test_shows(UI_SYMBOL_USB_270));
	bar_tap();
	CHECK(ui_test_shows("Connected to a computer"));
	CHECK(!ui_test_shows_part("internet"));
	ui_test_write(CARRIER, "0\n");
	ui_test_wait(1100);
	CHECK(ui_test_shows("Not connected"));
}

static int invalidated;

static void invalidate_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	invalidated++;
}

/* Nothing redrawn while nothing changes: the GPU sleeps between frames (the status
 * bar's update set the USB symbol's color again every second, redrawing it) */
TEST(status_bar_still_while_plugged)
{
	ui_test_wall_clock(1800000000 + 10); /* 10 s into a minute: the clock stays */
	ui_test_write(CARRIER, "1\n");
	ui_test_start();
	ui_test_wait(2000);
	CHECK(ui_test_shows(UI_SYMBOL_USB_270));
	lv_display_add_event_cb(lv_display_get_default(), invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);
	ui_test_wait(3000);
	CHECK_INT(invalidated, 0);
}

/* A wall charger powers the USB input without a network: not "Not connected" */
TEST(usb_charger)
{
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", "1\n");
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Charger only"));
	CHECK(!ui_test_shows("Not connected"));
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", "0\n");
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

/* Held still, or past the panel's end, nothing moves: nothing is drawn again (each
 * reading set the scrim's opacity again, redrawing the whole screen: 44 fps, CPU
 * doubled, held near the bottom) */
TEST(held_still_while_pulled)
{
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	lv_test_mouse_move_to(360, 20);
	lv_test_mouse_press();
	ui_test_wait(16);
	for (int y = 60; y <= 1400; y += 40) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	lv_test_mouse_move_to(360, 1400);
	ui_test_wait(100);
	lv_display_add_event_cb(lv_display_get_default(), invalidate_cb, LV_EVENT_INVALIDATE_AREA, NULL);
	ui_test_wait(500);
	CHECK_INT(invalidated, 0);
	for (int i = 0; i < 10; i++) { /* a finger trembling, below the panel */
		lv_test_mouse_move_to(360 + i % 2 * 3, 1400 - i % 3 * 4);
		ui_test_wait(16);
	}
	CHECK_INT(invalidated, 0);
	lv_test_mouse_release();
	ui_test_settle();
	CHECK(shade_open());
}

/* What the panel hides is not drawn: the screen under it was, then the dimming, then
 * the panel, on every frame of a pull (5 times the display's pixels over Messages, 40 fps) */

static int screen_draws;
static int32_t screen_drawn_from; /* the highest row of the screen drawn */

static void screen_draw_cb(lv_event_t *e)
{
	const lv_area_t *clip = &lv_event_get_layer(e)->_clip_area;
	if (!screen_draws++ || clip->y1 < screen_drawn_from)
		screen_drawn_from = clip->y1;
}

static void screen_draws_count(void)
{
	screen_draws = 0;
	lv_obj_add_event_cb(lv_screen_active(), screen_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
}

/* The panel: the top layer's child holding its tiles */
static lv_obj_t *panel(void)
{
	lv_obj_t *obj = ui_test_find("Flashlight");
	while (obj && lv_obj_get_parent(obj) != lv_layer_top())
		obj = lv_obj_get_parent(obj);
	return obj;
}

TEST(screen_not_drawn_under_the_open_panel)
{
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	bar_tap();
	CHECK(shade_open());
	screen_draws_count();
	lv_area_t a = { 0, 300, 719, 600 }; /* the toggles */
	lv_obj_invalidate_area(lv_layer_top(), &a);
	ui_test_wait(16);
	CHECK_INT(screen_draws, 0);
	CHECK(ui_test_shows("Flashlight"));
}

/* Over home, with notifications, opened a second time (the phone's case) */
TEST(home_not_drawn_under_the_open_panel)
{
	ui_test_start();
	ui_notify("Messages", "1", "Raluca", "See you", 8, time(NULL), false, NULL);
	ui_notify("Messages", "2", "Bogdan", "Six hours", 3, time(NULL), false, NULL);
	for (int i = 0; i < 2; i++) {
		bar_tap();
		CHECK(shade_open());
		CHECK(ui_test_shows("Raluca"));
		if (!i) {
			bar_tap();
			CHECK(!shade_open());
		}
	}
	screen_draws_count();
	lv_area_t a = { 0, 300, 719, 600 };
	lv_obj_invalidate_area(lv_layer_top(), &a);
	ui_test_wait(16);
	CHECK_INT(screen_draws, 0);
}

static void pulled_and_checked(void)
{
	lv_test_mouse_move_to(360, 20);
	lv_test_mouse_press();
	ui_test_wait(16);
	for (int y = 60; y <= 700; y += 40) {
		lv_test_mouse_move_to(360, y);
		ui_test_wait(16);
	}
	screen_draws_count();
	lv_test_mouse_move_to(360, 720); /* the panel and the dimming change */
	ui_test_wait(16);
	lv_area_t p;
	lv_obj_get_coords(panel(), &p);
	CHECK(screen_draws > 0); /* below it */
	int32_t r = lv_obj_get_style_radius(panel(), 0);
	if (screen_drawn_from < p.y2 - r)
		TEST_FAIL("the screen drawn from row %d, the panel ends at %d (radius %d)",
		          (int)screen_drawn_from, (int)p.y2, (int)r);
	lv_test_mouse_release();
	ui_test_settle();
}

TEST(screen_drawn_only_below_the_pulled_panel)
{
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	pulled_and_checked();
}

TEST(home_drawn_only_below_the_pulled_panel)
{
	ui_test_start();
	ui_notify("Messages", "1", "Raluca", "See you", 8, time(NULL), false, NULL);
	ui_notify("Messages", "2", "Bogdan", "Six hours", 3, time(NULL), false, NULL);
	bar_tap();
	bar_tap();
	CHECK(!shade_open());
	pulled_and_checked();
}

/* Its rounded corners show the dimmed screen beside them, drawn with the rest below */
TEST(panel_corners_over_the_dimmed_screen)
{
	ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	bar_tap();
	CHECK(shade_open());
	lv_area_t p;
	lv_obj_get_coords(panel(), &p);
	lv_area_t a = { 0, 300, 719, 1439 };
	lv_obj_invalidate_area(lv_layer_top(), &a);
	ui_test_wait(16);
	lv_color_t corner = ui_test_pixel(1, p.y2 - 1), below = ui_test_pixel(1, p.y2 + 8);
	CHECK_INT(corner.red, below.red);
	CHECK_INT(corner.green, below.green);
	CHECK_INT(corner.blue, below.blue);
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
	lv_obj_t *sun = lv_obj_get_child(s, 0);
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

/* A Bluetooth speaker playing: the slider is its volume (bluez-alsa's) */
TEST(volume_slider_of_speaker)
{
	ui_test_write(SETTINGS, "bluetooth=1\n");
	ui_test_bt_running(true);
	ui_test_bt_paired("F0:AE:66:E8:A4:BB", "soundcore R50i", true);
	ui_test_bt_audio("F0:AE:66:E8:A4:BB", 48000);
	ui_test_bt_set_volume("F0:AE:66:E8:A4:BB", 64);
	ui_test_start();
	ui_test_wait(1100); /* the speaker seen (each second) */
	bar_tap();
	lv_obj_t *s = slider(1);
	CHECK(s && lv_slider_get_value(s) == 13);
	if (!s)
		return;
	slide_to_start(s);
	CHECK_INT(ui_test_bt_volume("F0:AE:66:E8:A4:BB"), -1);
	CHECK(strstr(file(SETTINGS), "volume=") == NULL);
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

/* The tile with that symbol (its only label), NULL if none */
static lv_obj_t *tile(const char *symbol)
{
	lv_obj_t *label = ui_test_find(symbol);
	return label ? lv_obj_get_parent(label) : NULL;
}

TEST(vibration_toggle)
{
	ui_test_start();
	CHECK(ui_haptics);
	bar_tap();
	CHECK(tile(LV_SYMBOL_BELL) && lv_obj_has_state(tile(LV_SYMBOL_BELL), LV_STATE_CHECKED));
	CHECK(ui_test_tap_text(LV_SYMBOL_BELL));
	CHECK(!ui_haptics);
	CHECK_STR(file(SETTINGS), "haptics=0\n");
	CHECK(shade_open()); /* toggles leave it open */
	CHECK(!lv_obj_has_state(tile(LV_SYMBOL_BELL), LV_STATE_CHECKED));
	CHECK(ui_test_tap_text(LV_SYMBOL_BELL));
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

/* The moon: on in the dark theme */
TEST(dark_mode)
{
	ui_test_start();
	CHECK(!ui_light);
	bar_tap();
	CHECK(tile(UI_SYMBOL_MOON) && lv_obj_has_state(tile(UI_SYMBOL_MOON), LV_STATE_CHECKED));
	CHECK(ui_test_tap_text(UI_SYMBOL_MOON));
	ui_test_settle();
	CHECK(ui_light);
	CHECK(shade_open()); /* made again in the new theme, still open */
	CHECK_STR(file(SETTINGS), "light=1\n");
	CHECK(tile(UI_SYMBOL_MOON) && !lv_obj_has_state(tile(UI_SYMBOL_MOON), LV_STATE_CHECKED));
	CHECK(ui_test_tap_text(UI_SYMBOL_MOON));
	ui_test_settle();
	CHECK(!ui_light);
	CHECK(shade_open());
	ui_test_tap_at(360, 1400); /* beside it: closes as before */
	ui_test_settle();
	CHECK(!shade_open());
}

/* It would rebuild the screens: only at home */
TEST(no_theme_switch_in_an_app)
{
	CHECK(ui_test_open("Notes"));
	bar_tap();
	CHECK(tile(UI_SYMBOL_MOON) && lv_obj_has_state(tile(UI_SYMBOL_MOON), LV_STATE_DISABLED));
	CHECK(ui_test_tap_text(UI_SYMBOL_MOON));
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
	CHECK(tile(UI_SYMBOL_MOON) && lv_obj_has_state(tile(UI_SYMBOL_MOON), LV_STATE_DISABLED));
	bar_tap();
	CHECK(ui_locked());
}

/* The layout: each slider a card with its icon inside, a square tile beside it (the moon
 * beside the brightness, the bell beside the volume), no words on those; the other toggles
 * on two rows at most */

static lv_area_t coords(lv_obj_t *obj)
{
	lv_area_t a = { 0 };
	if (obj)
		lv_obj_get_coords(obj, &a);
	return a;
}

/* The tile beside the slider: right of it, on its row, as tall, square */
static void beside(lv_obj_t *slider, const char *symbol)
{
	lv_obj_t *t = tile(symbol);
	CHECK(slider && t);
	if (!slider || !t)
		return;
	lv_area_t s = coords(slider), a = coords(t);
	CHECK(a.x1 > s.x2 && a.x1 < s.x2 + 40);
	CHECK_INT(a.y1, s.y1);
	CHECK_INT(a.y2, s.y2);
	CHECK_INT(lv_area_get_width(&a), lv_area_get_height(&a));
	CHECK_INT(lv_obj_get_child_count(t), 1); /* the symbol alone */
}

/* The icon within the slider, at its start */
static void icon_inside(lv_obj_t *slider, const char *symbol)
{
	lv_obj_t *icon = slider ? lv_obj_get_child(slider, 0) : NULL;
	CHECK(icon && lv_obj_check_type(icon, &lv_label_class));
	if (!icon || !lv_obj_check_type(icon, &lv_label_class))
		return;
	CHECK_STR(lv_label_get_text(icon), symbol);
	lv_area_t s = coords(slider), a = coords(icon);
	CHECK(a.x1 >= s.x1 && a.x2 < s.x1 + lv_area_get_width(&s) / 4);
	CHECK(a.y1 > s.y1 && a.y2 < s.y2);
}

TEST(sliders_with_tiles_beside)
{
	ui_test_start();
	bar_tap();
	beside(slider(0), UI_SYMBOL_MOON);
	beside(slider(1), LV_SYMBOL_BELL);
	icon_inside(slider(0), UI_SYMBOL_SUN);
	icon_inside(slider(1), LV_SYMBOL_VOLUME_MAX);
	CHECK(!ui_test_shows("Vibration"));
	CHECK(!ui_test_shows("Light theme"));
	CHECK(!ui_test_shows(LV_SYMBOL_EYE_OPEN));
}

static bool similar(lv_color_t a, lv_color_t b)
{
	return abs(a.red - b.red) < 0x18 && abs(a.green - b.green) < 0x18 && abs(a.blue - b.blue) < 0x18;
}

/* Filled in the accent color from its start to the value, the tiles' color past it, empty
 * at the minimum; no knob. The icon white on the fill, in the text color without. */
TEST(slider_filled_as_a_card)
{
	ui_test_write(SETTINGS, "light=1\n"); /* its text color is not white */
	ui_test_start();
	bar_tap();
	lv_obj_t *s = slider(0);
	CHECK(s != NULL);
	if (!s)
		return;
	lv_area_t a = coords(s);
	int32_t y = a.y1 + 12, w = lv_area_get_width(&a); /* above the icon */
	lv_color_t accent = lv_color_hex(UI_COLOR_ACCENT), track = lv_color_hex(UI_COLOR_BUTTON);
	CHECK(similar(ui_test_pixel(a.x1 + 40, y), accent));
	CHECK(similar(ui_test_pixel(a.x1 + w * 3 / 10, y), accent)); /* 50 of 5 to 100 */
	CHECK(similar(ui_test_pixel(a.x1 + w * 7 / 10, y), track));
	CHECK(similar(ui_test_pixel(a.x2 - 40, (a.y1 + a.y2) / 2), track)); /* no knob */
	lv_obj_t *icon = lv_obj_get_child(s, 0);
	CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, 0), lv_color_white()));
	slide_to_start(s);
	CHECK_INT(lv_slider_get_value(s), UI_BRIGHTNESS_MIN);
	CHECK(similar(ui_test_pixel(a.x1 + 40, y), track));
	CHECK(similar(ui_test_pixel(a.x1 + 4, (a.y1 + a.y2) / 2), track));
	CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, 0), lv_color_hex(UI_COLOR_TEXT)));
}

/* The rows of tiles below the sliders: distinct tops of the toggles' row's tiles */
static unsigned tile_rows(void)
{
	lv_obj_t *label = ui_test_find("Flashlight");
	if (!label)
		return 0;
	lv_obj_t *tiles = lv_obj_get_parent(lv_obj_get_parent(label));
	int32_t tops[8];
	unsigned n = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(tiles); i++) {
		if (lv_obj_is_hidden(lv_obj_get_child(tiles, i)))
			continue;
		int32_t y = coords(lv_obj_get_child(tiles, i)).y1;
		unsigned k = 0;
		while (k < n && tops[k] != y)
			k++;
		if (k == n && n < 8)
			tops[n++] = y;
	}
	return n;
}

TEST(toggles_on_two_rows)
{
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("Mobile data"));
	CHECK_INT(tile_rows(), 2);
	/* Wi-Fi, Bluetooth and mobile data, then the flashlight and the rotation lock in their columns */
	lv_area_t wifi = coords(lv_obj_get_parent(ui_test_find("Wi-Fi")));
	lv_area_t bt = coords(lv_obj_get_parent(ui_test_find("Bluetooth")));
	lv_area_t data = coords(lv_obj_get_parent(ui_test_find("Mobile data")));
	lv_area_t torch = coords(lv_obj_get_parent(ui_test_find("Flashlight")));
	lv_area_t lock = coords(lv_obj_get_parent(ui_test_find("Rotation lock")));
	CHECK(wifi.y1 == bt.y1 && wifi.y1 == data.y1 && torch.y1 == lock.y1 && torch.y1 > wifi.y1);
	CHECK_INT(lv_area_get_width(&torch), lv_area_get_width(&wifi));
	CHECK_INT(lv_area_get_width(&lock), lv_area_get_width(&bt));
	CHECK_INT(torch.x1, wifi.x1);
	CHECK_INT(lock.x1, bt.x1);
	CHECK(lock.x2 < data.x1);
}

/* Without a modem (the board has none), no mobile data: two rows of two */
TEST(toggles_without_a_modem)
{
	remove("/tmp/modem.conf");
	ui_test_start();
	bar_tap();
	CHECK(!ui_test_shows("Mobile data"));
	CHECK_INT(tile_rows(), 2);
	lv_area_t wifi = coords(lv_obj_get_parent(ui_test_find("Wi-Fi")));
	lv_area_t bt = coords(lv_obj_get_parent(ui_test_find("Bluetooth")));
	lv_area_t lock = coords(lv_obj_get_parent(ui_test_find("Rotation lock")));
	CHECK_INT(lock.x1, bt.x1);
	CHECK_INT(lock.x2, bt.x2);
	CHECK(lock.y1 > wifi.y1);
}

/* Turning with the phone (turn.c): home turns, the panel fits the display */

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

/* Opened sideways, then asleep and awake: closed by locking (the panel was once left
 * 1440 px wide, upright behind the lock screen) */
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
	CHECK(!shade_open());
	CHECK(!ui_shade_shown());
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
static void held_tile_grows(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
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

TEST(held_tile_not_clipped)
{
	ui_test_start();
	bar_tap();
	held_tile_grows(LV_SYMBOL_BELL);  /* beside the volume */
	held_tile_grows("Rotation lock"); /* the toggles' last row */
}

/* In Romanian: the tiles, the cable's line, the battery's with the kernel's word */
TEST(romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_write(BATTERY "capacity", "50\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "1375000\n");
	ui_test_start();
	ui_test_wait(22000); /* the charger settles after plugging in */
	bar_tap();
	CHECK(ui_test_shows("Lanternă"));
	CHECK(ui_test_shows("Blocare rotire"));
	CHECK(ui_test_shows("Neconectat"));
	CHECK(ui_test_shows("50%, Se încarcă \xc2\xb7 plină în aproximativ 1 h"));
}

TEST(romanian_battery_unknown)
{
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_write(BATTERY "status", "\n");
	ui_test_start();
	bar_tap();
	CHECK(ui_test_shows("76%, stare necunoscută"));
}
