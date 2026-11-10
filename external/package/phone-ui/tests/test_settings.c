/* Settings (settings/, the settings file of prefs.c), through the UI (ui_test.h), on the
 * sysfs of ui_test_reset(): backlight 500 of 1000, battery 76% discharging */

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "test.h"
#include "ui/ui_internal.h"
#include "ui_symbols.h"
#include "ui_test.h"
#include "system/usage.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define BACKLIGHT "/sys/class/backlight/backlight/"
#define BATTERY UI_BATTERY "/"

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

/* Taps a slider `fraction` of the way along */
static void slide(lv_obj_t *slider, double fraction)
{
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	ui_test_tap_at(a.x1 + (int32_t)((a.x2 - a.x1) * fraction), (a.y1 + a.y2) / 2);
}

TEST(list)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("Display"));
	CHECK(ui_test_shows("50%"));    /* brightness */
	CHECK(ui_test_font("Display") == UI_FONT_TITLE); /* the rows' titles, as the other lists' */
	CHECK(ui_test_font("50%") == UI_FONT_BODY);
	CHECK(ui_test_shows("Vibrate"));
	CHECK(ui_test_shows("UTC"));
	CHECK(ui_test_shows("76%"));    /* battery */
	CHECK(ui_test_shows("Offline")); /* no USB network */
	CHECK(ui_test_shows("PinePhone"));
}

TEST(saved_settings_applied_at_startup)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "brightness=30\nhaptics=0\ntz=IST-5:30\n");
	ui_test_start();
	CHECK_STR(file(BACKLIGHT "brightness"), "300\n");
	CHECK(!ui_haptics);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("30%"));
	CHECK(ui_test_shows("Silent"));
	CHECK(ui_test_shows("India"));
}

/* A line without a value (edited by hand) is skipped, the others read */
TEST(saved_settings_with_a_broken_line)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "brightness=30\nhaptics\nhaptics=0\n");
	ui_test_start();
	CHECK_STR(file(BACKLIGHT "brightness"), "300\n");
	CHECK(!ui_haptics);
}

/* Cannot be saved (the data partition is full or read-only): applied all the same */
TEST(settings_not_saved)
{
	ui_test_reset();
	ui_test_write("/var/lib/phone-ui", "not a folder");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Sound & vibration"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw != NULL);
	if (sw)
		ui_test_tap(sw);
	CHECK(!ui_haptics);
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Silent"));
}

TEST(unknown_time_zone_shown_as_set)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "tz=CET-1CEST\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("CET-1CEST"));
}

TEST(brightness)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("Screen brightness"));
	CHECK(ui_test_shows("50%"));
	lv_obj_t *slider = ui_test_find_widget(&lv_slider_class);
	CHECK(slider);
	if (!slider)
		return;

	slide(slider, 1.0);
	CHECK(ui_test_shows("100%"));
	CHECK_STR(file(BACKLIGHT "brightness"), "1000\n");
	CHECK_STR(file(SETTINGS), "brightness=100\n");

	/* Never black: 5% at the least */
	slide(slider, 0.0);
	CHECK(ui_test_shows("5%"));
	CHECK_STR(file(BACKLIGHT "brightness"), "50\n");
	CHECK_STR(file(SETTINGS), "brightness=5\n");

	CHECK(ui_test_back());
	CHECK(ui_test_shows("5%"));
}

/* The slider between Font Awesome's suns, as the shade's: the regular one (an outline)
 * at the dim end, the solid one at the bright end, as big */
TEST(brightness_suns)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	lv_obj_t *slider = ui_test_find_widget(&lv_slider_class);
	CHECK(slider);
	if (!slider)
		return;
	lv_obj_t *row = lv_obj_get_parent(slider);
	lv_obj_t *dim = lv_obj_get_child(row, 0), *bright = lv_obj_get_child(row, -1);
	CHECK(lv_obj_check_type(dim, &lv_label_class) && lv_obj_check_type(bright, &lv_label_class));
	if (!lv_obj_check_type(dim, &lv_label_class) || !lv_obj_check_type(bright, &lv_label_class))
		return;
	CHECK_STR(lv_label_get_text(dim), UI_SYMBOL_SUN_REGULAR);
	CHECK_STR(lv_label_get_text(bright), UI_SYMBOL_SUN);
	/* The same size: the outline makes the difference */
	CHECK(lv_obj_get_style_text_font(dim, LV_PART_MAIN) ==
	      lv_obj_get_style_text_font(bright, LV_PART_MAIN));
}

TEST(no_backlight)
{
	ui_test_reset();
	remove(BACKLIGHT "max_brightness");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("No backlight found"));
	CHECK(!ui_test_find_widget(&lv_slider_class));
}

TEST(screen_info)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("720 x 1440"));
	CHECK(ui_test_shows("269 dpi"));
	CHECK(ui_test_shows("220 ms"));
}

/* Tap to wake: the touchscreen (the input device on the display) as a wakeup source, its
 * device's power/wakeup, where the kernel's driver can be one. The Power key's is not it. */
#define TOUCH_WAKE "/sys/class/input/event1/device/device/power/wakeup"
#define POWER_WAKE "/sys/class/input/event0/device/device/power/wakeup"

static void touch_wakes(const char *state)
{
	ui_test_write("/sys/class/input/event0/device/properties", "0\n");
	ui_test_write(POWER_WAKE, "enabled\n");
	ui_test_write("/sys/class/input/event1/device/properties", "2\n"); /* INPUT_PROP_DIRECT */
	ui_test_write(TOUCH_WAKE, state);
}

TEST(tap_to_wake)
{
	ui_test_reset();
	touch_wakes("disabled\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("Tap to wake"));
	CHECK(ui_test_shows_part("more battery"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK_STR(file(TOUCH_WAKE), "enabled\n");
	CHECK_STR(file(SETTINGS), "tap_wake=1\n");
	ui_test_tap(sw);
	CHECK_STR(file(TOUCH_WAKE), "disabled\n");
	CHECK_STR(file(SETTINGS), "tap_wake=0\n");
	CHECK_STR(file(POWER_WAKE), "enabled\n");
}

/* The kernel forgets it at each boot: set again at startup */
TEST(tap_to_wake_at_startup)
{
	ui_test_reset();
	touch_wakes("disabled\n");
	ui_test_write(SETTINGS, "tap_wake=1\n");
	ui_test_start();
	CHECK_STR(file(TOUCH_WAKE), "enabled\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Off unless turned on, whatever the kernel starts with (a wakeup-source in the device tree) */
TEST(tap_to_wake_off_at_startup)
{
	ui_test_reset();
	touch_wakes("enabled\n");
	ui_test_start();
	CHECK_STR(file(TOUCH_WAKE), "disabled\n");
	CHECK_STR(file(POWER_WAKE), "enabled\n");
}

/* A touchscreen that cannot wake the phone: no switch */
TEST(no_tap_to_wake)
{
	ui_test_reset();
	ui_test_write("/sys/class/input/event1/device/properties", "2\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("Screen brightness"));
	CHECK(!ui_test_shows("Tap to wake"));
	CHECK(!ui_test_find_widget(&lv_switch_class));
}

TEST(light_and_dark)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(!ui_light);
	/* Every screen is rebuilt, Settings and Display reopened on top */
	CHECK(ui_test_tap_text("Light"));
	CHECK(ui_light);
	CHECK_STR(file(SETTINGS), "light=1\n");
	CHECK(ui_test_shows("Display"));
	CHECK(ui_test_shows("Screen brightness"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Sound & vibration"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Settings")); /* the menu's tile */

	/* The rest of the UI works in it */
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("No notes"));

	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_tap_text("Dark"));
	CHECK(!ui_light);
	CHECK_STR(file(SETTINGS), "light=0\n");
	CHECK(ui_test_shows("Screen brightness"));
}

TEST(light_at_startup)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	CHECK(ui_light);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("Light"));
}

/* Settings in Romanian (i18n.c): every screen is rebuilt, as for the theme */
TEST(language)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("English")); /* the Language row's value */
	CHECK(ui_test_tap_text("Language"));
	CHECK(ui_test_tap_text("Română"));
	CHECK_STR(file(SETTINGS), "language=ro\n");
	CHECK(ui_test_shows("Limbă"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Setări"));
	CHECK(ui_test_shows("Afișaj"));
	CHECK(ui_test_shows("Sunet și vibrații"));
	CHECK(ui_test_shows("Română"));
	CHECK(!ui_test_shows("Display"));

	CHECK(ui_test_tap_text("Limbă"));
	CHECK(ui_test_tap_text("English"));
	CHECK_STR(file(SETTINGS), "language=en\n");
	CHECK(ui_test_shows("Language"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Display"));
	CHECK(!ui_test_shows("Afișaj"));
}

TEST(language_at_startup)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_start();
	CHECK(ui_test_open("Setări")); /* the menu in Romanian too */
	CHECK(ui_test_shows("Baterie"));
	CHECK(ui_test_shows("Despre telefon"));
	CHECK(ui_test_tap_text("Afișaj"));
	CHECK(ui_test_shows("Luminozitatea ecranului"));
	CHECK(ui_test_shows("Întunecat"));
	CHECK(ui_test_back());
	/* The kernel's words too */
	CHECK(ui_test_tap_text("Baterie"));
	CHECK(ui_test_shows("Se descarcă"));
	CHECK(ui_test_shows("Bună")); /* health */
	CHECK(ui_test_shows("Ultimele 24 de ore"));
}

/* Day and month names of its own: the phone has no glibc locales */
TEST(language_date)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_wall_clock(1793880000); /* Thursday 5 November 2026, 12:00 UTC */
	ui_test_start();
	CHECK(ui_test_open("Setări"));
	CHECK(ui_test_tap_text("Dată și oră"));
	CHECK(ui_test_shows("joi 5 noiembrie 2026"));
	CHECK(ui_test_shows("Fus orar"));
	CHECK(ui_test_shows("Londra"));
}

TEST(date)
{
	ui_test_wall_clock(1793880000);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Date & time"));
	CHECK(ui_test_shows("Thursday 5 November 2026"));
}

TEST(vibration)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Sound & vibration"));
	CHECK(ui_test_shows("Vibrate on touch"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(!ui_haptics);
	CHECK_STR(file(SETTINGS), "haptics=0\n");
	CHECK(ui_test_tap_text(LV_SYMBOL_BELL "  Test vibration")); /* without a vibrator */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Silent"));

	CHECK(ui_test_tap_text("Sound & vibration"));
	sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (sw)
		ui_test_tap(sw);
	CHECK(ui_haptics);
	CHECK_STR(file(SETTINGS), "haptics=1\n");
}

/* The motor: a tap's buzz turning touch vibration on, none off; the test's longer
 * either way */
TEST(vibration_felt)
{
	ui_test_reset();
	ui_test_vibrator();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Sound & vibration"));
	unsigned ms, before = ui_test_vibrations(NULL);
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw != NULL);
	if (!sw)
		return;
	ui_test_tap(sw); /* off */
	CHECK_INT(ui_test_vibrations(NULL), before);
	CHECK(ui_test_tap_text(LV_SYMBOL_BELL "  Test vibration"));
	CHECK_INT(ui_test_vibrations(&ms), before + 1);
	CHECK_INT(ms, 300);
	ui_test_tap(sw); /* on */
	CHECK_INT(ui_test_vibrations(&ms), before + 2);
	CHECK_INT(ms, 60);
}

TEST(time_zone)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Date & time"));
	CHECK(ui_test_shows("UTC (UTC)"));
	CHECK(ui_test_shows("UTC+9")); /* Tokyo's row */
	CHECK(ui_test_shows("UTC+5:30"));

	CHECK(ui_test_tap_text("Tokyo"));
	CHECK(ui_test_shows("Tokyo (UTC+9)"));
	CHECK_STR(file(SETTINGS), "tz=JST-9\n");
	/* The time shown is Tokyo's, here and in the status bar */
	char now[8];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	CHECK_INT(tm.tm_gmtoff, 9 * 3600);
	strftime(now, sizeof(now), "%H:%M", &tm);
	lv_obj_t *times[4];
	CHECK(ui_test_find_all(now, times, 4) >= 2);
	/* One check mark */
	lv_obj_t *checks[4];
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, checks, 4), 1);

	CHECK(ui_test_back());
	CHECK(ui_test_shows("Tokyo"));
}

TEST(battery)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("76%"));
	CHECK(ui_test_shows("Discharging"));
	CHECK(ui_test_shows("Good"));
	CHECK(ui_test_shows("3.85 V"));
	CHECK(ui_test_shows("-310 mA")); /* rounded to 10 mA */
	CHECK(ui_test_shows("Not connected")); /* the charger */
	CHECK(ui_test_shows("Li-ion"));
	CHECK(ui_test_shows("Design capacity"));
	CHECK(ui_test_shows("2750 mAh"));

	/* Updated every 2 s */
	ui_test_write(BATTERY "capacity", "75\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("75%"));
	CHECK(ui_test_shows("Charging"));
}

/* The battery drawn by the charge, as in the status bar, larger */
TEST(battery_icon)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	lv_obj_t *all[1024], *icon = NULL;
	unsigned n = ui_test_find_widgets(&lv_obj_class, all, 1024);
	for (unsigned i = 0; i < n && i < 1024; i++) {
		lv_area_t a;
		lv_obj_get_coords(all[i], &a);
		if (a.y1 > UI_STATUS_H && ui_battery_icon_get(all[i], NULL, NULL))
			icon = all[i];
	}
	CHECK(icon != NULL);
	if (!icon)
		return;
	CHECK(lv_obj_get_height(icon) >= 2 * 20); /* the status bar's is 20 high */
	int level;
	bool charging;
	CHECK(ui_battery_icon_get(icon, &level, &charging));
	CHECK_INT(level, 76);
	CHECK(!charging);

	ui_test_write(BATTERY "capacity", "75\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_wait(2100);
	CHECK(ui_battery_icon_get(icon, &level, &charging));
	CHECK_INT(level, 75);
	CHECK(charging);
}

/* A label with this text in the status bar (the page shows the charge too) */
static bool bar_shows(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H)
			return true;
	}
	return false;
}

/* The percentage beside the status bar's battery: off by default, on at once */
/* The status bar's battery icon's width, 0 without */
static int32_t bar_battery_width(void)
{
	static lv_obj_t *all[1024];
	unsigned n = ui_test_find_widgets(&lv_obj_class, all, 1024);
	for (unsigned i = 0; i < n && i < 1024; i++) {
		lv_area_t a;
		lv_obj_get_coords(all[i], &a);
		if (a.y2 <= UI_STATUS_H && a.y1 >= 0 && ui_battery_icon_get(all[i], NULL, NULL))
			return lv_area_get_width(&a);
	}
	return 0;
}

/* The percentage in the bar's battery icon, which widens for it (no label) */
TEST(battery_percentage)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Show percentage in status bar"));
	int32_t w = bar_battery_width();
	CHECK(w > 0);
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	ui_test_wait(1100);
	CHECK(bar_battery_width() > w);
	CHECK(!bar_shows("76%"));
	CHECK_STR(file(SETTINGS), "battery_percent=1\n");
	ui_test_tap(sw);
	ui_test_wait(1100);
	CHECK_INT(bar_battery_width(), w);
	CHECK_STR(file(SETTINGS), "battery_percent=0\n");
}

/* The estimate shown, in minutes, -1 for none */
static int minutes_to_full(void)
{
	lv_obj_t *labels[256];
	const char *s = NULL;
	int n = (int)ui_test_find_widgets(&lv_label_class, labels, 256);
	for (int i = 0; i < n && !s; i++)
		s = strstr(lv_label_get_text(labels[i]), "full in about ");
	int h = 0, m = 0;
	if (!s)
		return -1;
	s += strlen("full in about ");
	if (sscanf(s, "%d h %d min", &h, &m) == 2)
		return h * 60 + m;
	if (strstr(s, " h"))
		return atoi(s) * 60;
	return atoi(s);
}

/* While charging: the charge missing to 100% of the design capacity (2750 mAh) at the
 * charging current averaged over the last minutes ("Charging" is also the temperature
 * scale's legend) */
TEST(battery_time_to_full)
{
	ui_test_reset();
	ui_test_write(BATTERY "capacity", "50\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "1375000\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(22000); /* the charger settles after plugging in */
	CHECK(ui_test_shows("Charging \xc2\xb7 full in about 1 h"));

	/* 2200 mAh at 1375 mA */
	ui_test_write(BATTERY "capacity", "20\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Charging \xc2\xb7 full in about 1 h 36 min"));

	/* The current doubles: the average follows over minutes, not at once */
	ui_test_write(BATTERY "current_now", "2750000\n");
	ui_test_wait(10000);
	int minutes = minutes_to_full();
	CHECK(minutes > 48 && minutes < 96);
	ui_test_wait(30000);
	CHECK(minutes_to_full() < minutes);

	/* Not charging: no estimate, and the average starts again */
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "current_now", "-300000\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Discharging"));
	CHECK(!ui_test_shows_part("full in"));
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "440000\n");
	ui_test_wait(22100);
	CHECK(ui_test_shows("Charging \xc2\xb7 full in about 5 h"));

	/* At 100% the charger tops it up: nothing to predict */
	ui_test_write(BATTERY "capacity", "100\n");
	ui_test_wait(2100);
	CHECK(!ui_test_shows_part("full in"));
}

/* Under a minute to go (1% of a 1000 mAh battery at 2.75 A): a minute, not none */
TEST(battery_time_to_full_last_minute)
{
	ui_test_reset();
	ui_test_write(BATTERY "charge_full_design", "1000000\n");
	ui_test_write(BATTERY "capacity", "99\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "2750000\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(22000);
	CHECK(ui_test_shows("Charging \xc2\xb7 full in about 1 min"));
}

/* A driver without `present` (assumed there) or `status` (none to show): the level still
 * shown */
TEST(battery_without_present_and_status)
{
	ui_test_reset();
	remove(BATTERY "present");
	remove(BATTERY "status");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("76%"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows_part("76%"));
	CHECK(!ui_test_shows_part("Charging"));
}

#define HISTORY "/var/lib/phone-ui/battery-history"

/* The level and charging, in a file: when phone-ui starts, when charging starts or
 * stops, then every 5 minutes; two days are kept */
TEST(battery_history_recorded)
{
	ui_test_reset();
	time_t now = time(NULL);
	char old[64];
	snprintf(old, sizeof(old), "%lld 40 D\n", (long long)(now - 3 * 86400));
	ui_test_write(HISTORY, old);
	ui_test_start();
	long long t;
	int level;
	char state;
	CHECK(sscanf(file(HISTORY), "%lld %d %c", &t, &level, &state) == 3);
	CHECK(llabs(t - now) < 5);
	CHECK_INT(level, 76);
	CHECK_INT(state, 'D');
	CHECK(!strstr(file(HISTORY), " 40 D"));

	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_wait(1100);
	CHECK(strstr(file(HISTORY), " 76 C\n") != NULL);

	ui_test_write(BATTERY "capacity", "77\n");
	ui_test_wait(60000);
	CHECK(strstr(file(HISTORY), " 77 C") == NULL);
	ui_test_wait(250000);
	CHECK(strstr(file(HISTORY), " 77 C\n") != NULL);
}

/* The history chart's bars, oldest first, and the y of its 100% and 0% lines (the box of
 * its "100%" label: the bars are narrow, the lines wide and 1 px tall; the screen strip neither) */
static int history_bars(lv_obj_t **bars, int max, int32_t *top, int32_t *bottom)
{
	lv_obj_t *label = ui_test_find("50%"); /* the axis: "100%" may be the level too */
	if (!label)
		return -1;
	lv_obj_t *chart = lv_obj_get_parent(label);
	int n = 0;
	*top = INT32_MAX;
	*bottom = INT32_MIN;
	for (uint32_t i = 0; i < lv_obj_get_child_count(chart); i++) {
		lv_obj_t *c = lv_obj_get_child(chart, (int32_t)i);
		if (!lv_obj_check_type(c, &lv_obj_class))
			continue;
		lv_area_t a;
		lv_obj_get_coords(c, &a);
		if (lv_area_get_width(&a) > 20 && lv_area_get_height(&a) == 1) {
			*top = a.y1 < *top ? a.y1 : *top;
			*bottom = a.y1 > *bottom ? a.y1 : *bottom;
		} else if (lv_area_get_width(&a) <= 20 && n < max) {
			bars[n++] = c;
		}
	}
	return n;
}

/* A bar's level in percent, -1 if hidden */
static int bar_level(lv_obj_t *bar, int32_t top, int32_t bottom)
{
	if (lv_obj_is_hidden(bar))
		return -1;
	lv_area_t a;
	lv_obj_get_coords(bar, &a);
	return (int)lround(100.0 * (bottom - a.y1) / (bottom - top));
}

static bool bar_colored(lv_obj_t *bar, uint32_t hex)
{
	return lv_color_eq(lv_obj_get_style_bg_color(bar, 0), lv_color_hex(hex));
}

/* Lighter: halfway to the card's color, for levels drawn between samples while asleep */
static bool bar_colored_asleep(lv_obj_t *bar, uint32_t hex)
{
	return lv_color_eq(lv_obj_get_style_bg_color(bar, 0),
	                   lv_color_mix(lv_color_hex(hex), lv_color_hex(UI_COLOR_CARD), LV_OPA_50));
}

/* The last 24 hours, a bar per 15 minutes: the level between samples drawn straight
 * (phone-ui does not run while the phone sleeps), green while charging, up to now */
TEST(battery_history_chart)
{
	ui_test_reset();
	time_t now = time(NULL);
	char history[256];
	snprintf(history, sizeof(history), "%lld 100 D\n%lld 50 D\n%lld 50 C\n%lld 100 D\n%lld 70 D\n",
	         (long long)(now - 23 * 3600), (long long)(now - 13 * 3600),
	         (long long)(now - 12 * 3600), (long long)(now - 7 * 3600), (long long)(now - 3600));
	ui_test_write(HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Last 24 hours"));
	ui_test_wait(100);

	lv_obj_t *bars[128];
	int32_t top, bottom;
	CHECK_INT(history_bars(bars, 128, &top, &bottom), 96);
	CHECK(bottom - top > 100);
	if (history_bars(bars, 128, &top, &bottom) != 96)
		return;
	/* Before the first sample: nothing */
	CHECK_INT(bar_level(bars[0], top, bottom), -1);
	/* About 18 h ago: halfway from 100% (23 h) to 50% (13 h), lighter: asleep in between */
	int level = bar_level(bars[95 - 72], top, bottom);
	CHECK(level >= 70 && level <= 80);
	CHECK(bar_colored_asleep(bars[95 - 72], UI_COLOR_ACCENT));
	/* About 10 h ago: charging */
	CHECK(bar_colored_asleep(bars[95 - 40], 0x34c759));
	level = bar_level(bars[95 - 40], top, bottom);
	CHECK(level >= 60 && level <= 80);
	/* Now: the level now (76%) */
	level = bar_level(bars[95], top, bottom);
	CHECK(level >= 74 && level <= 78);
	CHECK(bar_colored(bars[95], UI_COLOR_ACCENT));
}

/* A gap longer than 12 hours (the phone was off): left empty */
TEST(battery_history_gap)
{
	ui_test_reset();
	time_t now = time(NULL);
	char history[128];
	snprintf(history, sizeof(history), "%lld 80 D\n%lld 60 D\n", (long long)(now - 20 * 3600),
	         (long long)(now - 5 * 3600));
	ui_test_write(HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(100);
	lv_obj_t *bars[128];
	int32_t top, bottom;
	if (history_bars(bars, 128, &top, &bottom) != 96) {
		TEST_FAIL("no chart");
		return;
	}
	CHECK_INT(bar_level(bars[95 - 10 * 4], top, bottom), -1);
	CHECK(bar_level(bars[95 - 2 * 4], top, bottom) > 0);
}

/* Awake, phone-ui samples every 5 minutes; samples further apart mean the phone slept
 * between them: the level drawn there is a guess, lighter than the measured one */
TEST(battery_history_asleep)
{
	ui_test_reset();
	time_t now = time(NULL);
	char history[4096];
	size_t len = 0;
	/* Awake from 20 h to 18 h ago, asleep until 10 h ago, awake since */
	for (time_t t = now - 20 * 3600; t <= now - 18 * 3600; t += 300)
		len += (size_t)snprintf(history + len, sizeof(history) - len, "%lld 99 D\n", (long long)t);
	for (time_t t = now - 10 * 3600; t < now; t += 300)
		len += (size_t)snprintf(history + len, sizeof(history) - len, "%lld 52 D\n", (long long)t);
	ui_test_write(HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(100);
	lv_obj_t *bars[128];
	int32_t top, bottom;
	if (history_bars(bars, 128, &top, &bottom) != 96) {
		TEST_FAIL("no chart");
		return;
	}
	/* 19 h ago: measured */
	CHECK_INT(bar_level(bars[95 - 19 * 4], top, bottom), 99);
	CHECK(bar_colored(bars[95 - 19 * 4], UI_COLOR_ACCENT));
	/* 14 h ago: asleep, the level drawn straight from 99% to 52% */
	int level = bar_level(bars[95 - 14 * 4], top, bottom);
	CHECK(level > 52 && level < 99);
	CHECK(bar_colored_asleep(bars[95 - 14 * 4], UI_COLOR_ACCENT));
	/* 5 h ago: measured again */
	CHECK_INT(bar_level(bars[95 - 5 * 4], top, bottom), 52);
	CHECK(bar_colored(bars[95 - 5 * 4], UI_COLOR_ACCENT));
}

/* Just plugged in, the charger ramps its current up from almost nothing: no estimate
 * until it settles, rather than one of days that the average takes minutes to bring down */
TEST(battery_time_to_full_after_plugging_in)
{
	ui_test_reset();
	ui_test_write(BATTERY "capacity", "50\n");
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "current_now", "-300000\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "9000\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Charging"));
	CHECK(!ui_test_shows_part("full in"));
	ui_test_write(BATTERY "current_now", "1375000\n");
	ui_test_wait(30000);
	CHECK(ui_test_shows("Charging \xc2\xb7 full in about 1 h"));
}

/* Full on the charger the driver says "Discharging" (the battery tops up the input): "Full",
 * and plugged in for the history, green as charging */
TEST(battery_full_on_the_charger)
{
	ui_test_reset();
	char history[64];
	snprintf(history, sizeof(history), "%lld 98 C\n", (long long)(time(NULL) - 3600));
	ui_test_write(HISTORY, history);
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", "1\n");
	ui_test_write(BATTERY "capacity", "100\n");
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "current_now", "-33000\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Full"));
	CHECK(!ui_test_shows_part("Discharging"));
	ui_test_wait(100);

	lv_obj_t *bars[128];
	int32_t top, bottom;
	CHECK_INT(history_bars(bars, 128, &top, &bottom), 96);
	if (history_bars(bars, 128, &top, &bottom) != 96)
		return;
	CHECK(bar_colored(bars[95], 0x34c759));
	char log[256] = "";
	FILE *f = fopen(HISTORY, "r");
	if (f) {
		if (fread(log, 1, sizeof(log) - 1, f) == 0)
			log[0] = '\0';
		fclose(f);
	}
	CHECK(strstr(log, " 100 C\n") != NULL);
}

/* Without the battery's capacity (no battery in the device tree), no estimate */
TEST(battery_time_to_full_unknown)
{
	ui_test_reset();
	unlink(BATTERY "charge_full_design");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_write(BATTERY "current_now", "1000000\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(!ui_test_shows_part("full in"));
	CHECK(!ui_test_shows("Design capacity"));
}

#define USB_POWER "/sys/class/power_supply/axp20x-usb/"
#define TYPEC "/sys/class/typec/"
#define UDC_STATE "/sys/class/udc/musb-hdrc.2.auto/state"

/* What powers the phone: a computer (it configured the USB gadget) or a charger, what
 * the USB-C port advertises (the ANX7688 driver's typec port) and the input current
 * limit the driver set from it */
TEST(battery_charger)
{
	ui_test_reset();
	ui_test_write(USB_POWER "online", "1\n");
	ui_test_write(USB_POWER "input_current_limit", "3000000\n");
	ui_test_write(TYPEC "port0/power_operation_mode", "usb_power_delivery\n");
	ui_test_write(TYPEC "port0-partner/supports_usb_power_delivery", "yes\n");
	ui_test_write(UDC_STATE, "configured\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Computer \xc2\xb7 USB-C PD \xc2\xb7 3 A"));

	/* Nothing talks USB: a charger */
	ui_test_write(UDC_STATE, "not attached\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Charger \xc2\xb7 USB-C PD \xc2\xb7 3 A"));

	ui_test_write(TYPEC "port0/power_operation_mode", "1.5A\n");
	ui_test_write(USB_POWER "input_current_limit", "1500000\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Charger \xc2\xb7 USB-C \xc2\xb7 1.5 A"));

	/* USB-C's default current, or no USB-C partner: plain USB */
	ui_test_write(TYPEC "port0/power_operation_mode", "default\n");
	ui_test_write(USB_POWER "input_current_limit", "500000\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Charger \xc2\xb7 USB \xc2\xb7 500 mA"));

	ui_test_write(USB_POWER "online", "0\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("Not connected"));
}

/* The AXP803's registers as the regmap's debugfs file lists them: its defaults, the TS pin
 * fed 80 µA, the temperature check on (0x84 bit 2 clear) */
#define SCREEN_TIME "/var/lib/phone-ui/screen-time"
#define SCREEN_HISTORY "/var/lib/phone-ui/screen-history"

static void plugged(bool in)
{
	ui_test_write(USB_POWER "online", in ? "1\n" : "0\n");
	ui_test_write(BATTERY "status", in ? "Charging\n" : "Discharging\n");
}

/* Power pressed: asleep `ms`, then awake and unlocked */
static void sleep_for(uint32_t ms)
{
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300); /* the fade out, the lock screen shown, the sleep */
	ui_test_suspended(ms);
	ui_test_settle();
	ui_test_swipe(360, 1200, 360, 500, 600);
	ui_test_settle();
}

/* No "Since unplugged" card: Battery use's Screen and Asleep rows say how long */
TEST(battery_no_since_unplugged)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	plugged(false);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(2100);
	CHECK(!ui_test_shows("Since unplugged"));
	CHECK(!ui_test_shows("Screen on"));
}

/* The screen's period open when phone-ui stopped ends at its last save, in the history at
 * the next start */
TEST(battery_screen_period_kept)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	ui_test_wall_clock(1700000000);
	ui_test_write(SCREEN_TIME, "1699990000 1699990600\n");
	ui_test_start();
	char history[256] = "";
	FILE *f = fopen(SCREEN_HISTORY, "r");
	size_t n = f ? fread(history, 1, sizeof(history) - 1, f) : 0;
	history[n] = '\0';
	if (f)
		fclose(f);
	CHECK_STR(history, "1699990000 1699990600\n");
}

/* The chart's strip of the times the screen was on, below the bars, as x ranges */
static int screen_strip(int32_t *x1, int32_t *x2, int max, int32_t *strip_x1, int32_t *strip_w)
{
	lv_obj_t *label = ui_test_find("Screen");
	if (!label)
		return -1;
	lv_obj_t *chart = lv_obj_get_parent(label), *strip = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(chart); i++)
		if (lv_obj_get_child_count(lv_obj_get_child(chart, (int32_t)i)) > 0)
			strip = lv_obj_get_child(chart, (int32_t)i);
	if (!strip)
		return -1;
	lv_area_t a;
	lv_obj_get_coords(strip, &a);
	*strip_x1 = a.x1;
	*strip_w = lv_area_get_width(&a);
	int n = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(strip) && n < max; i++) {
		lv_obj_t *c = lv_obj_get_child(strip, (int32_t)i);
		if (lv_obj_is_hidden(c))
			continue;
		lv_obj_get_coords(c, &a);
		x1[n] = a.x1;
		x2[n++] = a.x2;
	}
	return n;
}

/* Below the bars, when the screen was on: what was logged, then since phone-ui started,
 * apart while it slept */
TEST(battery_chart_screen_on)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	time_t now = 1700000000 / 900 * 900 + 450, end = now + 450, start = end - 24 * 3600;
	ui_test_wall_clock(now);
	char history[64];
	snprintf(history, sizeof(history), "%lld %lld\n", (long long)(now - 7200), (long long)(now - 5400));
	ui_test_write(SCREEN_HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(100);

	int32_t x1[8], x2[8], sx, w;
	CHECK_INT(screen_strip(x1, x2, 8, &sx, &w), 2);
	if (screen_strip(x1, x2, 8, &sx, &w) != 2)
		return;
#define AT(t) (sx + (int32_t)((int64_t)w * ((t) - start) / (24 * 3600)))
	CHECK(abs(x1[0] - AT(now - 7200)) <= 2 && abs(x2[0] - AT(now - 5400)) <= 2);
	CHECK(abs(x1[1] - AT(now)) <= 2);

	/* An hour asleep, then the screen on for a minute */
	ui_test_wait(60000);
	sleep_for(3600 * 1000);
	ui_test_wait(60000);
	start += 3600; /* the chart ends with the slot of now */
	CHECK_INT(screen_strip(x1, x2, 8, &sx, &w), 3);
	if (screen_strip(x1, x2, 8, &sx, &w) != 3)
		return;
	CHECK(abs(x2[1] - AT(now + 60)) <= 2);
	CHECK(abs(x1[2] - AT(now + 3660)) <= 2);
#undef AT
}

/* More periods than the chart reads (512): the newest kept, the screen on now among them;
 * lines that are not periods skipped */
TEST(battery_chart_screen_on_many)
{
	ui_test_reset();
	time_t now = 1700000000 / 900 * 900 + 450, end = now + 450, start = end - 24 * 3600;
	ui_test_wall_clock(now);
	static char history[600 * 32 + 64];
	size_t len = 0;
	for (int i = 0; i < 600; i++) {
		long long t = now - 3600 - (600 - i) * 120;
		len += (size_t)snprintf(history + len, sizeof(history) - len, "%lld %lld\n", t, t + 60);
		if (i == 300)
			len += (size_t)snprintf(history + len, sizeof(history) - len, "garbage\n%lld %lld\n", t + 90, t + 70);
	}
	ui_test_write(SCREEN_HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_wait(100);

	int32_t x1[8], x2[8], sx, w;
	CHECK_INT(screen_strip(x1, x2, 8, &sx, &w), 2); /* a minute on in every two: one run */
	if (screen_strip(x1, x2, 8, &sx, &w) != 2)
		return;
#define AT(t) (sx + (int32_t)((int64_t)w * ((t) - start) / (24 * 3600)))
	/* The first 89 dropped: 511 from the file and the screen on now */
	CHECK(abs(x1[0] - AT(now - 3600 - (600 - 89) * 120)) <= 2);
	CHECK(abs(x2[0] - AT(now - 3600 - 60)) <= 2);
	CHECK(abs(x1[1] - AT(now)) <= 2);
#undef AT
}

/* More samples than the chart reads (2047): the newest kept; lines that are not samples
 * (a level past 100) skipped */
TEST(battery_history_chart_long)
{
	ui_test_reset();
	time_t now = time(NULL);
	static char history[2200 * 24];
	size_t len = 0;
	for (int i = 0; i < 2100; i++)
		len += (size_t)snprintf(history + len, sizeof(history) - len, "%lld %d D\n",
		                        (long long)(now - (2100 - i) * 30), i < 53 ? 90 : 30);
	len += (size_t)snprintf(history + len, sizeof(history) - len, "%lld 150 D\nbroken\n", (long long)now - 10);
	ui_test_write(HISTORY, history);
	ui_test_write(BATTERY "capacity", "30\n"); /* now's, the last sample's */
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	lv_obj_t *bars[200];
	int32_t top, bottom;
	int n = history_bars(bars, 200, &top, &bottom);
	CHECK(n > 10);
	int shown = 0;
	for (int i = 0; i < n; i++) {
		int level = bar_level(bars[i], top, bottom);
		if (level >= 0) {
			shown++;
			CHECK(level <= 31);
		}
	}
	CHECK(shown > 50); /* 17 hours of the 24 */
}

#define PMIC_REGS "/sys/kernel/debug/regmap/sunxi-rsb-3a3/registers"
#define PMIC_DEFAULTS "33: c1\n38: a5\n39: 1f\n3c: fc\n3d: 16\n84: f2\n"

/* The battery's thermistor (3 kΩ at 25 °C) read at the TS pin, in Details with the
 * charging limits of the power chip, which assumes 10 kΩ: charging only below 14 °C */
TEST(battery_temperature)
{
	ui_test_reset();
	ui_test_write(PMIC_REGS, PMIC_DEFAULTS);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	/* 300 mV / 80 µA = 3750 ohm; charging 0xa5 and 0x1f x 12.8 mV */
	CHECK(ui_test_shows("Temperature"));
	CHECK(ui_test_shows("20 °C, charges -18 to 14 °C"));
	CHECK(!ui_test_shows("Alarm"));

	/* Warmer, every 2 s */
	ui_test_write("/sys/bus/iio/devices/iio:device0/in_voltage0_raw", "250\n"); /* 2500 ohm */
	ui_test_wait(2100);
	CHECK(ui_test_shows("29 °C, charges -18 to 14 °C"));
}

/* The check off (0x84 bit 2): the chip ignores the thermistor, it charges at any
 * temperature; the TS current from 0x84 too (bits 5:4, 40 µA here) */
TEST(battery_temperature_check_off)
{
	ui_test_reset();
	ui_test_write(PMIC_REGS, "33: c1\n38: a5\n39: 1f\n3c: fc\n3d: 16\n84: d6\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("5 °C, no charging limits")); /* 300 mV / 40 µA = 7500 ohm */
}

/* Without debugfs: the temperature from the default 80 µA, the limits unknown; without
 * the ADC's channel, no temperature */
TEST(battery_temperature_unknown)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("20 °C"));

	remove("/sys/bus/iio/devices/iio:device0/in_voltage0_raw");
	ui_test_wait(2100);
	CHECK(!ui_test_shows("20 °C"));
	CHECK(ui_test_shows("Unknown"));
}

/* Without the drivers' readings: said unknown, not made up */
TEST(battery_readings_unknown)
{
	ui_test_reset();
	ui_test_write(USB_POWER "online", "1\n");
	ui_test_write(UDC_STATE, "not attached\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("3.85 V"));
	remove(BATTERY "voltage_now");
	remove(BATTERY "current_now");
	remove(USB_POWER "input_current_limit");
	ui_test_wait(2100);
	CHECK(!ui_test_shows("3.85 V"));
	CHECK(!ui_test_shows("-310 mA"));
	CHECK(ui_test_shows("Charger \xc2\xb7 USB"));
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("Unknown", found, 4), 2);
	remove(USB_POWER "online");
	ui_test_wait(2100);
	CHECK_INT(ui_test_find_all("Unknown", found, 4), 3);
}

/* A brightness above full in the settings (edited by hand): full */
TEST(brightness_above_full)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "brightness=150\n");
	ui_test_start();
	CHECK_STR(file(BACKLIGHT "brightness"), "1000\n");
}

/* Without a backlight (another board): nothing to set, the screen still sleeps and wakes */
TEST(no_backlight_device)
{
	ui_test_reset();
	remove(BACKLIGHT "brightness");
	remove(BACKLIGHT "max_brightness");
	remove(BACKLIGHT "bl_power");
	remove(BACKLIGHT "actual_brightness");
	rmdir(BACKLIGHT);
	ui_test_write(SETTINGS, "brightness=30\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_shows("No backlight found"));
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	CHECK(ui_locked());
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	CHECK(ui_locked());
	CHECK(ui_test_shows("Swipe up to unlock"));
}

TEST(no_battery)
{
	ui_test_reset();
	ui_test_write(BATTERY "present", "0\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("None"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("--"));
	CHECK(ui_test_shows("No battery"));
	CHECK(!ui_test_shows("Present"));
}

/* Present, but under 2.5 V: dead */
TEST(dead_battery)
{
	ui_test_reset();
	ui_test_write(BATTERY "voltage_now", "1200000\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Dead battery"));
}

TEST(no_battery_driver)
{
	ui_test_reset();
	remove(BATTERY "capacity");
	remove(BATTERY "health");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("No battery driver"));
	CHECK(ui_test_shows("Unknown"));
}

TEST(network)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Network"));
	CHECK(ui_test_shows("Not connected"));
	CHECK(ui_test_shows("No modem"));
}

/* A wall charger: USB powered, without a network */
TEST(network_usb_charger)
{
	ui_test_reset();
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", "1\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Network"));
	CHECK(ui_test_shows("Charger only"));
	CHECK(!ui_test_shows("Not connected"));
}

TEST(storage)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Storage"));
	CHECK(ui_test_shows("System"));
	CHECK(ui_test_shows_part(" of "));
	CHECK(ui_test_shows_part("free, read-only")); /* the sandbox's root, as the phone's */
	CHECK(ui_test_shows("Not mounted"));          /* no /data */
}

TEST(about)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("About phone"));
	CHECK(ui_test_shows("PinePhone"));
	CHECK(ui_test_shows("9.6.0"));
	CHECK(ui_test_shows("Allwinner A64"));
	CHECK(ui_test_shows("1152 MHz (schedutil)"));
	CHECK(ui_test_shows("41 °C"));  /* CPU */
	CHECK(ui_test_shows("40 °C"));  /* GPU: the hotter of its two sensors */
	CHECK(ui_test_shows("46 °C"));  /* PMIC: (raw + offset) * scale */
	CHECK(ui_test_shows("20 °C"));  /* the battery: its thermistor */
	CHECK(ui_test_shows_part("MB free of"));
}

static bool colored(const char *text, uint32_t rgb)
{
	lv_obj_t *label = ui_test_find(text);
	return label && lv_color_eq(lv_obj_get_style_text_color(label, 0), lv_color_hex(rgb));
}

/* Booted from slot B; the CPU warm (within 10 °C of throttling: orange), the GPU's
 * sensors missing, no governor */
TEST(about_slot_b_warm)
{
	ui_test_reset();
	ui_test_write("/tmp/cmdline", "console=ttyS0,115200 root=PARTUUID=50480001-02 rootwait ro\n");
	ui_test_write("/sys/class/thermal/thermal_zone0/temp", "70000\n");
	remove("/sys/class/thermal/thermal_zone0/trip_point_0_temp"); /* 75 °C then */
	remove("/sys/class/thermal/thermal_zone1/temp");
	remove("/sys/class/thermal/thermal_zone2/temp");
	remove("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("About phone"));
	CHECK(ui_test_shows("B (partition 2)"));
	CHECK(ui_test_shows("70 °C"));
	CHECK(colored("70 °C", UI_COLOR_ORANGE));
	CHECK(ui_test_shows("Unknown")); /* the GPU's */
	CHECK(ui_test_shows("1152 MHz"));
}

/* Slot A; the CPU past its trip point (red); no frequency */
TEST(about_slot_a_hot)
{
	ui_test_reset();
	ui_test_write("/tmp/cmdline", "root=PARTUUID=50480001-01 ro\n");
	ui_test_write("/sys/class/thermal/thermal_zone0/temp", "80000\n");
	remove("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("About phone"));
	CHECK(ui_test_shows("A (partition 1)"));
	CHECK(colored("80 °C", UI_COLOR_RED));
	CHECK(colored("40 °C", UI_COLOR_DIM)); /* the GPU's, cool */
	CHECK(ui_test_shows("Unknown")); /* the frequency */
}

/* Booted from elsewhere (an SD card, another partition): not a slot */
TEST(about_slot_unknown)
{
	ui_test_reset();
	ui_test_write("/tmp/cmdline", "root=PARTUUID=50480001-03 ro\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("About phone"));
	CHECK(ui_test_shows("Unknown"));
	CHECK(!ui_test_shows_part("partition"));
	/* Nor a root given by its partition's ID */
	CHECK(ui_test_back());
	ui_test_write("/tmp/cmdline", "root=/dev/mmcblk2p1 ro\n");
	CHECK(ui_test_tap_text("About phone"));
	CHECK(ui_test_shows("Unknown"));
	CHECK(!ui_test_shows_part("partition"));
}

/* Developer mode, with its icon in the list: its sections, the speed test and the
 * accelerometer each on a screen of their own */
TEST(developer_mode)
{
	CHECK(ui_test_open("Settings"));
	lv_obj_t *row = ui_test_find("Developer mode");
	CHECK(row != NULL);
	if (row) {
		lv_obj_t *icon = lv_obj_get_child(lv_obj_get_parent(row), 1);
		CHECK_STR(lv_label_get_text(lv_obj_get_child(icon, 0)), UI_SYMBOL_CODE);
	}
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_shows("Performance"));
	CHECK(ui_test_shows("Computers on USB"));
	CHECK(ui_test_shows("Network"));
	CHECK(ui_test_shows("Sensors"));
	CHECK(ui_test_shows("Speed test"));
	CHECK(ui_test_shows("Accelerometer"));
	CHECK(!ui_test_shows("Start speed test"));
	CHECK(!ui_test_shows("Mount matrix"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_shows("Start speed test"));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Accelerometer"));
	CHECK(ui_test_shows("Mount matrix"));
}

/* The accelerometer's raw values, as the mount matrix turns them and as the screen
 * rotation takes them */
TEST(developer_mode_accelerometer)
{
	ui_test_reset();
	ui_test_sensors(true, false);
	const float up[3] = { 0, 1, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Accelerometer"));
	CHECK(ui_test_shows("iio:device1 (mpu6050)"));
	CHECK(ui_test_shows("0, -1, 0; 1, 0, 0; 0, 0, 1"));
	CHECK(ui_test_shows("16384"));                 /* raw x: the chip's x to the top edge */
	CHECK(ui_test_shows("+9.80 m/s\xc2\xb2"));     /* the top edge's axis: 16384 x 0.000598 */
	CHECK(ui_test_shows("Top edge"));              /* up, as the matrix gives it */
	CHECK(ui_test_shows("Upright"));

	const float left[3] = { -1, 0, 0 }; /* its left edge up */
	ui_test_sensors_feed(left, field, 1);
	ui_test_wait(600);
	CHECK(ui_test_shows("Left edge"));
	CHECK(ui_test_shows("Left edge up (270\xc2\xb0)"));
}

/* A row's value (settings_value(): the label after its title, not a value showing the
 * same text), "?" without the row */
static const char *row_value(const char *title)
{
	lv_obj_t *found[4];
	unsigned n = ui_test_find_all(title, found, 4);
	for (unsigned i = 0; i < n && i < 4; i++) {
		lv_obj_t *value = lv_obj_get_sibling(found[i], 1);
		if (value && lv_obj_check_type(value, &lv_label_class))
			return lv_label_get_text(value);
	}
	return "?";
}

/* Developer mode › Game controller: the controller connected and what it holds, buttons by
 * where they are (the standard layout's), the D-pad from the left stick past half-way or
 * from the hat; disconnected, and another connecting while the page is open */
TEST(developer_mode_game_controller)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_shows("Input"));
	CHECK(ui_test_tap_text("Game controller"));
	CHECK_STR(row_value("Controller"), "None connected");
	CHECK_STR(row_value("Last pressed"), "");

	ui_test_gamepad("Wireless Controller", false);
	CHECK(ui_test_wait_for("Wireless Controller", 3000));
	ui_test_gamepad_button(BTN_SOUTH, true);
	ui_test_wait(100);
	CHECK_STR(row_value("Bottom button"), "Held");
	CHECK_STR(row_value("Right button"), "");
	CHECK_STR(row_value("Last pressed"), "Bottom button");
	ui_test_gamepad_button(BTN_TR, true); /* the M30's C */
	ui_test_wait(100);
	CHECK_STR(row_value("Bottom button"), "Held");
	CHECK_STR(row_value("R1"), "Held");
	CHECK_STR(row_value("Last pressed"), "R1");
	ui_test_gamepad_button(BTN_SOUTH, false);
	ui_test_gamepad_button(BTN_TR, false);
	ui_test_wait(100);
	CHECK_STR(row_value("Bottom button"), "");
	CHECK_STR(row_value("R1"), "");
	CHECK_STR(row_value("Last pressed"), "R1");

	/* The M30's D-pad is the left stick: 0, 128 or 255 */
	ui_test_gamepad_axis(ABS_X, 0);
	ui_test_wait(100);
	CHECK_STR(row_value("D-pad left"), "Held");
	CHECK_STR(row_value("Last pressed"), "D-pad left");
	ui_test_gamepad_axis(ABS_X, 128);
	ui_test_gamepad_axis(ABS_Y, 255);
	ui_test_wait(100);
	CHECK_STR(row_value("D-pad left"), "");
	CHECK_STR(row_value("D-pad down"), "Held");
	ui_test_gamepad_axis(ABS_Y, 170); /* a stick not past half-way */
	ui_test_wait(100);
	CHECK_STR(row_value("D-pad down"), "");
	ui_test_gamepad_button(BTN_MODE, true);
	ui_test_wait(100);
	CHECK_STR(row_value("Home"), "Held");
	/* A tap shorter than a read: pressed and let go between two */
	ui_test_gamepad_button(BTN_NORTH, true);
	ui_test_gamepad_button(BTN_NORTH, false);
	ui_test_wait(100);
	CHECK_STR(row_value("Last pressed"), "Top button");
	CHECK_STR(row_value("Top button"), "");

	ui_test_gamepad_gone();
	CHECK(ui_test_wait_for("None connected", 3000));
	CHECK_STR(row_value("Home"), "");

	/* Another, its D-pad a hat */
	ui_test_gamepad("Pro Controller", true);
	CHECK(ui_test_wait_for("Pro Controller", 3000));
	ui_test_gamepad_axis(ABS_HAT0Y, -1);
	ui_test_wait(100);
	CHECK_STR(row_value("D-pad up"), "Held");
	CHECK_STR(row_value("Last pressed"), "D-pad up");
	ui_test_gamepad_axis(ABS_HAT0Y, 0);
	ui_test_gamepad_axis(ABS_HAT0X, 1);
	ui_test_wait(100);
	CHECK_STR(row_value("D-pad up"), "");
	CHECK_STR(row_value("D-pad right"), "Held");
}

/* The right edge up turns the screen; flat on its back it stays as it was */
TEST(developer_mode_accelerometer_turned)
{
	ui_test_reset();
	ui_test_sensors(true, false);
	const float right[3] = { 1, 0, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(right, field, 1);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Accelerometer"));
	CHECK(ui_test_shows("Right edge"));
	CHECK(ui_test_shows("Right edge up (90\xc2\xb0)"));

	const float flat[3] = { 0, 0, 1 };
	ui_test_sensors_feed(flat, field, 1);
	ui_test_wait(600);
	CHECK(ui_test_shows("Stays (flat, upside down or between)"));
}

/* A driver without a scale: the values as the matrix turns them, without a unit */
TEST(developer_mode_accelerometer_unscaled)
{
	ui_test_reset();
	ui_test_sensors(true, false);
	remove("/sys/bus/iio/devices/iio:device1/in_accel_scale");
	const float up[3] = { 0, 1, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Accelerometer"));
	CHECK(ui_test_shows("+16384"));
	CHECK(!ui_test_shows_part("m/s\xc2\xb2"));
	CHECK(ui_test_shows("Top edge"));
}

TEST(developer_mode_no_accelerometer)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Accelerometer"));
	CHECK(ui_test_shows("No accelerometer"));
}

/* The frame rate counter (LVGL's performance monitor): hidden by default, shown by
 * Developer mode's switch at once and kept */
TEST(developer_mode_fps_counter)
{
	ui_test_start();
	ui_test_wait(1000); /* the monitor's label updated */
	CHECK(!ui_test_shows_part("FPS"));
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_shows("Show frame rate"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	ui_test_wait(1000);
	CHECK(ui_test_shows_part("FPS"));
	CHECK_STR(file(SETTINGS), "fps_counter=1\n");
	ui_test_tap(sw);
	ui_test_wait(1000);
	CHECK(!ui_test_shows_part("FPS"));
	CHECK_STR(file(SETTINGS), "fps_counter=0\n");
}

/* The counter's text, NULL while hidden */
static const char *fps_text(void)
{
	lv_obj_t *sys = lv_layer_sys();
	for (uint32_t i = 0; i < lv_obj_get_child_count(sys); i++) {
		lv_obj_t *obj = lv_obj_get_child(sys, (int32_t)i);
		if (lv_obj_check_type(obj, &lv_label_class) && !lv_obj_is_hidden(obj) &&
		    strstr(lv_label_get_text(obj), "FPS"))
			return lv_label_get_text(obj);
	}
	return NULL;
}

/* It counts the frames drawn: few while nothing changes (it showed the display's rate, 62
 * here, whatever was drawn), every one while each frame draws */
TEST(developer_mode_fps_counter_counts_frames_drawn)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "fps_counter=1\n");
	ui_test_start();
	ui_test_wait(1000);
	for (int i = 0; i < 40; i++)
		ui_test_frame(16);
	const char *text = fps_text();
	CHECK(text != NULL);
	if (!text)
		return;
	CHECK(atoi(text) < 10);
	CHECK(!strstr(text, "monitor"));
	for (int i = 0; i < 40; i++) {
		lv_obj_invalidate(lv_screen_active());
		ui_test_frame(16);
	}
	CHECK(atoi(fps_text()) >= 55);
	CHECK(atoi(fps_text()) <= 63);
}

/* A monitor's frames: 30 a second */
static uint32_t monitor_frames(void)
{
	return lv_tick_get() * 30 / 1000;
}

/* With a monitor, the frames it showed too: it shows fewer than the display draws */
TEST(developer_mode_fps_counter_monitor)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "fps_counter=1\n");
	ui_test_start();
	ui_fps_counter_monitor(monitor_frames);
	ui_test_wait(1000);
	for (int i = 0; i < 40; i++)
		ui_test_frame(16);
	const char *text = fps_text();
	const char *monitor = text ? strstr(text, "monitor ") : NULL;
	CHECK(monitor != NULL);
	if (!monitor)
		return;
	CHECK(atoi(monitor + 8) >= 27);
	CHECK(atoi(monitor + 8) <= 33);
	ui_fps_counter_monitor(NULL);
	for (int i = 0; i < 40; i++)
		ui_test_frame(16);
	CHECK(!strstr(fps_text(), "monitor"));
}

TEST(developer_mode_fps_counter_saved)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "fps_counter=1\n");
	ui_test_start();
	ui_test_wait(1000);
	CHECK(ui_test_shows_part("FPS"));
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
}

#define SPEED_DOWN "https://speed.cloudflare.com/__down?bytes=*"
#define SPEED_UP "https://speed.cloudflare.com/__up"

/* The number before " Mbit/s" on the screen in the row titled `title`, -1 if none */
static double speed_shown(const char *title)
{
	lv_obj_t *labels[200], *t = NULL;
	unsigned n = ui_test_find_widgets(&lv_label_class, labels, 200);
	for (unsigned i = 0; i < n && !t; i++)
		if (strcmp(lv_label_get_text(labels[i]), title) == 0)
			t = labels[i];
	if (!t)
		return -1;
	lv_obj_t *row = lv_obj_get_parent(t);
	lv_obj_t *value = lv_obj_get_child(row, (int32_t)lv_obj_get_child_count(row) - 1);
	double v;
	char unit[16];
	if (sscanf(lv_label_get_text(value), "%lf %15s", &v, unit) != 2 || strcmp(unit, "Mbit/s") != 0)
		return -1;
	return v;
}

/* The speed test downloads (timed from the first data, shown as it comes), then
 * uploads what about 4 s take at that speed. The fake network sends a piece a poll
 * of phone-ui's (50 ms, 64 on the tests' 16 ms frames): 1.25 Mbit/s */
TEST(developer_mode_speed_test)
{
	static char body[1000000];
	memset(body, 'x', sizeof(body));
	ui_test_reset();
	ui_test_serve_data(SPEED_DOWN, 200, "application/octet-stream", body, sizeof(body));
	/* Its answer as long as an upload of 20 polls */
	ui_test_serve_data(SPEED_UP, 200, "text/plain", body, 200000);
	ui_test_start();
	ui_test_serve_in_pieces(10000);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_tap_text("Start speed test"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("Testing\xe2\x80\xa6"));
	double live = speed_shown("Download");
	CHECK(live > 1.0 && live < 2.0);
	CHECK(ui_test_requested(SPEED_UP) == NULL);
	ui_test_wait(9000);
	double down = speed_shown("Download");
	CHECK(down > 1.0 && down < 2.0);
	const struct ui_test_request *up = ui_test_requested(SPEED_UP);
	CHECK(up && up->body);
	if (!up || !up->body)
		return;
	size_t sent = strlen(up->body); /* 4 s at the download's speed */
	CHECK(sent > 500000 && sent < 1000000);
	double upload = speed_shown("Upload"); /* what it sent in about 1.3 s */
	CHECK(upload > sent * 8 / 1.6e6 && upload < sent * 8 / 0.9e6);
	CHECK(ui_test_shows("Start speed test"));
}

/* Uploads stay under 8 MB on a fast connection: past that, Cloudflare answered the
 * phone (not a PC with the same headers) "400 Unexpected Server Error" */
TEST(developer_mode_speed_test_upload_limit)
{
	static char body[1000000];
	memset(body, 'x', sizeof(body));
	ui_test_reset();
	ui_test_serve_data(SPEED_DOWN, 200, "application/octet-stream", body, sizeof(body));
	ui_test_serve(SPEED_UP, 200, "text/plain", "");
	ui_test_start();
	ui_test_serve_in_pieces(100000); /* 12.5 Mbit/s: 6.25 MB in 4 s */
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_tap_text("Start speed test"));
	ui_test_wait(2000);
	const struct ui_test_request *up = ui_test_requested(SPEED_UP);
	CHECK(up && up->body);
	if (up && up->body)
		CHECK(strlen(up->body) == 5000000);
}

/* Downloading stops after 10 s, still timed */
TEST(developer_mode_speed_test_stops)
{
	static char body[1000000];
	memset(body, 'x', sizeof(body));
	ui_test_reset();
	ui_test_serve_data(SPEED_DOWN, 200, "application/octet-stream", body, sizeof(body));
	ui_test_serve(SPEED_UP, 200, "text/plain", "");
	ui_test_start();
	ui_test_serve_in_pieces(1000); /* 0.16 Mbit/s: 50 s for all of it */
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_tap_text("Start speed test"));
	ui_test_wait(11000);
	CHECK(ui_test_shows_part("kbit/s"));
	CHECK(ui_test_requested(SPEED_UP) != NULL);
}

TEST(developer_mode_speed_test_offline)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_tap_text("Start speed test"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("No connection"));
	CHECK(ui_test_requested(SPEED_UP) == NULL);
	CHECK(ui_test_shows("Start speed test"));
}

static bool speed_test_start(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Developer mode") && ui_test_tap_text("Speed test") &&
	       ui_test_tap_text("Start speed test");
}

/* The server refusing: its status shown, nothing uploaded */
TEST(developer_mode_speed_test_refused)
{
	ui_test_reset();
	ui_test_serve(SPEED_DOWN, 503, "text/plain", "Service Unavailable");
	CHECK(speed_test_start());
	CHECK(ui_test_wait_for("Failed (HTTP 503)", 2000));
	CHECK(ui_test_requested(SPEED_UP) == NULL);
	CHECK(ui_test_shows("Start speed test"));
}

/* All of the download in its first data: nothing to time it by */
TEST(developer_mode_speed_test_too_short)
{
	ui_test_reset();
	ui_test_serve(SPEED_DOWN, 200, "application/octet-stream", "data");
	CHECK(speed_test_start());
	CHECK(ui_test_wait_for("Failed", 2000));
	CHECK(ui_test_requested(SPEED_UP) == NULL);
	CHECK(ui_test_shows("Start speed test"));
}

static char speed_body[4000000];

/* The upload refused (as Cloudflare does past 8 MB): the download's speed stays */
TEST(developer_mode_speed_test_upload_refused)
{
	memset(speed_body, 'x', 100000);
	ui_test_reset();
	ui_test_serve_data(SPEED_DOWN, 200, "application/octet-stream", speed_body, 100000);
	ui_test_serve(SPEED_UP, 400, "text/plain", "Unexpected Server Error");
	ui_test_start();
	ui_test_serve_in_pieces(10000);
	CHECK(speed_test_start());
	CHECK(ui_test_wait_for("Failed (HTTP 400)", 3000));
	CHECK(speed_shown("Download") > 1.0);
	CHECK(ui_test_shows("Start speed test"));
}

/* An upload still going after 20 s: at most what it sent in that time */
TEST(developer_mode_speed_test_upload_slow)
{
	memset(speed_body, 'x', sizeof(speed_body));
	ui_test_reset();
	ui_test_serve_data(SPEED_DOWN, 200, "application/octet-stream", speed_body, 100000);
	/* Its answer as long as 400 polls: 25 s */
	ui_test_serve_data(SPEED_UP, 200, "text/plain", speed_body, sizeof(speed_body));
	ui_test_start();
	ui_test_serve_in_pieces(10000);
	CHECK(speed_test_start());
	ui_test_wait(2000);
	CHECK(ui_test_requested(SPEED_UP) != NULL);
	CHECK(ui_test_shows("Testing\xe2\x80\xa6"));
	ui_test_wait(20000);
	CHECK(ui_test_shows_part("Under "));
	CHECK(ui_test_shows("Start speed test"));
}

/* Leaving the page cancels it */
TEST(developer_mode_speed_test_left)
{
	ui_test_reset();
	ui_test_serve(SPEED_DOWN, 200, "application/octet-stream", "data");
	ui_test_start();
	ui_test_network_hold(true);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_tap_text("Speed test"));
	CHECK(ui_test_tap_text("Start speed test"));
	ui_test_wait(500);
	ui_test_back();
	ui_test_wait(500);
	ui_test_network_hold(false);
	ui_test_wait(500);
	CHECK(ui_test_shows("Developer mode"));
	CHECK(ui_test_shows("Speed test"));
	CHECK(ui_test_requested(SPEED_UP) == NULL);
}

static int brightness_sum(lv_color_t c)
{
	return c.red + c.green + c.blue;
}

/* The slider's knob can be told from what is around it: on the column through its
 * middle, from above it to its center, a pixel darker than both (an edge). White
 * on the light theme's white cards and shade, it did not show. */
static bool knob_shows(lv_obj_t *slider)
{
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	int32_t min = lv_slider_get_min_value(slider), max = lv_slider_get_max_value(slider);
	int32_t x = a.x1 + (a.x2 - a.x1) * (lv_slider_get_value(slider) - min) / (max - min);
	int32_t y = (a.y1 + a.y2) / 2;
	int around = brightness_sum(ui_test_pixel(x, y - 40)), center = brightness_sum(ui_test_pixel(x, y));
	for (int32_t py = y - 40; py < y; py++)
		if (brightness_sum(ui_test_pixel(x, py)) + 30 < (around < center ? around : center))
			return true;
	TEST_FAIL("knob at (%d, %d): no edge between #%06x above and #%06x in it", (int)x, (int)y,
	          (unsigned)lv_color_to_u32(ui_test_pixel(x, y - 40)) & 0xffffff,
	          (unsigned)lv_color_to_u32(ui_test_pixel(x, y)) & 0xffffff);
	return false;
}

static int color_distance(lv_color_t a, lv_color_t b)
{
	return abs(a.red - b.red) + abs(a.green - b.green) + abs(a.blue - b.blue);
}

/* The shade's sliders have no knob: their level is where the fill ends, on the middle row,
 * the fill before it unlike the track after it, the track unlike the shade around it */
static bool fill_shows(lv_obj_t *slider)
{
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	int32_t min = lv_slider_get_min_value(slider), max = lv_slider_get_max_value(slider);
	int32_t x = a.x1 + (a.x2 - a.x1) * (lv_slider_get_value(slider) - min) / (max - min);
	int32_t y = (a.y1 + a.y2) / 2;
	lv_color_t fill = ui_test_pixel(x - 10, y), track = ui_test_pixel(x + 10, y);
	lv_color_t around = ui_test_pixel(x + 10, a.y1 - 5);
	if (color_distance(fill, track) > 90 && color_distance(track, around) > 30)
		return true;
	TEST_FAIL("level at (%d, %d): fill #%06x, track #%06x, around #%06x", (int)x, (int)y,
	          (unsigned)lv_color_to_u32(fill) & 0xffffff, (unsigned)lv_color_to_u32(track) & 0xffffff,
	          (unsigned)lv_color_to_u32(around) & 0xffffff);
	return false;
}

TEST(slider_knobs_light)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "light=1\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	lv_obj_t *slider = ui_test_find_widget(&lv_slider_class);
	CHECK(slider && knob_shows(slider));

	/* The status bar: the shade, its brightness and volume sliders over Display's */
	ui_test_tap_at(360, 20);
	lv_obj_t *sliders[3];
	CHECK_INT(ui_test_find_widgets(&lv_slider_class, sliders, 3), 3);
	for (int i = 1; i < 3; i++)
		CHECK(fill_shows(sliders[i]));
}

/* Battery use (usage.c): where the charge went since unplugged, by app and part */
#define USAGE "/var/lib/phone-ui/battery-usage"
#define PROC "/tmp/proc" /* usage_sample.c's /proc in the tests */

/* /proc's CPU counters (ticks of 10 ms): all cores' busy time, phone-ui's main thread's */
static void cpu_ticks(long busy, long main_thread)
{
	char path[128], text[256];
	snprintf(text, sizeof(text), "cpu  %ld 0 0 100000 0 0 0 0 0 0\n", busy);
	ui_test_write(PROC "/stat", text);
	snprintf(path, sizeof(path), PROC "/self/task/%d/stat", (int)getpid());
	snprintf(text, sizeof(text), "%d (phone-ui) S 1 1 1 0 -1 0 0 0 0 0 %ld 0 0 0 20 0 1 0 100 0 0\n",
	         (int)getpid(), main_thread);
	ui_test_write(path, text);
}

/* A battery use row's name: of the labels with that text, the one whose row ends with a
 * percentage ("Asleep" and "Screen" are elsewhere on the page too) */
static lv_obj_t *usage_row(const char *name)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(name, found, 8);
	for (unsigned i = 0; i < n; i++) {
		lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(found[i]));
		lv_obj_t *last = row ? lv_obj_get_child(row, -1) : NULL;
		if (last && lv_obj_check_type(last, &lv_label_class) && strchr(lv_label_get_text(last), '%'))
			return found[i];
	}
	return NULL;
}

/* Its percentage and the line under its name, NULL without the row */
static const char *usage_percent(const char *name)
{
	lv_obj_t *label = usage_row(name);
	return label ? lv_label_get_text(lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(label)), -1)) : NULL;
}

static const char *usage_detail(const char *name)
{
	lv_obj_t *label = usage_row(name);
	lv_obj_t *detail = label ? lv_obj_get_child(lv_obj_get_parent(label), 1) : NULL;
	return detail ? lv_label_get_text(detail) : NULL;
}

/* As saved: each entry's share of the charge, how long it was on screen (or the screen on,
 * the phone asleep), and the level used over the period */
TEST(battery_use_shown)
{
	ui_test_reset();
	char text[256];
	time_t now = time(NULL);
	snprintf(text, sizeof(text), "usage 1\n%lld 95 %lld 80 9000 3840\n300 3120 Screen\n150 600 Notes\n"
	         "50 3840 Asleep\n", (long long)(now - 3 * 3600 - 34 * 60), (long long)now);
	ui_test_write(USAGE, text);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Battery use"));
	CHECK(ui_test_shows("19% used in 3 h 34 min")); /* 95% then, 76% now */
	CHECK_STR(usage_percent("Notes"), "30%");
	CHECK_STR(usage_detail("Notes"), "10 min on screen");
	CHECK_STR(usage_percent("Asleep"), "10%");
	CHECK_STR(usage_detail("Asleep"), "1 h 4 min");
	CHECK(ui_test_shows("52 min on"));
	/* Most first */
	lv_obj_t *notes = usage_row("Notes"), *asleep = usage_row("Asleep");
	lv_area_t a, b;
	CHECK(notes && asleep);
	if (notes && asleep) {
		lv_obj_get_coords(notes, &a);
		lv_obj_get_coords(asleep, &b);
		CHECK(a.y1 < b.y1);
	}
}

/* Plugged in: the last time on battery stays, until unplugged again */
TEST(battery_use_last_time)
{
	ui_test_reset();
	char text[256];
	time_t now = time(NULL);
	snprintf(text, sizeof(text), "usage 1\n%lld 95 %lld 69 9000 3840\n150 600 Notes\n",
	         (long long)(now - 7200), (long long)(now - 3600));
	ui_test_write(USAGE, text);
	plugged(true);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Last time on battery"));
	CHECK(ui_test_shows("26% used in 1 h"));
	CHECK_STR(usage_percent("Notes"), "100%");

	plugged(false);
	ui_test_wait(2100);
	CHECK(!ui_test_shows("Last time on battery"));
	CHECK(!usage_percent("Notes"));
}

/* Measured: the app on screen gets the CPU phone-ui's main thread used meanwhile, the
 * screen its own share */
TEST(battery_use_by_app)
{
	ui_test_reset();
	CHECK(system("rm -rf " PROC) == 0);
	cpu_ticks(0, 0);
	CHECK(ui_test_open("Notes"));
	ui_test_wait(5000);
	cpu_ticks(3000, 3000); /* 30 s of a core */
	ui_test_wait(60000);
	CHECK(ui_test_back());
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("Battery use"));
	CHECK(usage_percent("Notes"));
	CHECK_STR(usage_detail("Notes"), "1 min on screen");
	CHECK(usage_percent("Screen"));
	CHECK(usage_percent("Idle"));
	/* Measured, the charge is the current's: 312 mA for about 70 s */
	double total = usage_total_mah();
	CHECK(total > 312 * 60 / 3600.0 && total < 312 * 80 / 3600.0);
}

/* A process of /proc (usage_sample.c's, in the tests' /tmp/proc): its name and CPU ticks,
 * the program it runs (NULL: none, as the kernel's threads) */
static void proc_ticks(int pid, const char *comm, long ticks, const char *exe)
{
	char path[128], text[256];
	snprintf(path, sizeof(path), PROC "/%d/stat", pid);
	snprintf(text, sizeof(text), "%d (%s) S 1 1 1 0 -1 0 0 0 0 0 %ld 0 0 0 20 0 1 0 100 0 0\n", pid, comm,
	         ticks);
	ui_test_write(path, text);
	snprintf(path, sizeof(path), PROC "/%d/exe", pid);
	if (exe && access(path, F_OK) != 0)
		CHECK(symlink(exe, path) == 0);
}

/* Other processes, by the app whose folder holds their program (mpg123: Music's); phone-ui's
 * other threads; what no process says it used: the system's */
TEST(battery_use_by_process)
{
	ui_test_reset();
	CHECK(system("rm -rf " PROC) == 0);
	ui_test_write("/var/lib/apps/doom/app.json", "{\"name\": \"Doom\", \"exec\": \"doom\"}");
	cpu_ticks(0, 0);
	proc_ticks(4242, "doom", 0, "/var/lib/apps/doom/doom");
	proc_ticks(4243, "mpg123", 0, NULL);
	proc_ticks(4244, "kworker/0:1", 0, NULL);
	CHECK(mkdir(PROC "/4245", 0755) == 0); /* ended while listed: no stat */
	char path[128];
	snprintf(path, sizeof(path), PROC "/self/task/%d/stat", (int)getpid() + 1);
	ui_test_write(path, "1 (phone-ui:gl) S 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 100 0 0\n");
	ui_test_start();
	ui_test_wait(5000);
	cpu_ticks(6000, 0); /* 60 s of a core, a third of it no process's */
	proc_ticks(4242, "doom", 2000, NULL);
	proc_ticks(4243, "mpg123", 1000, NULL);
	proc_ticks(4244, "kworker/0:1", 0, NULL);
	ui_test_write(path, "1 (phone-ui:gl) S 1 1 1 0 -1 0 0 0 0 0 1000 0 0 0 20 0 1 0 100 0 0\n");
	ui_test_wait(60000);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(usage_percent("Doom"));
	CHECK(usage_percent("Music"));
	CHECK(usage_percent("System"));
	CHECK(!usage_percent("Notes"));
}

/* Asleep, its share from the current learned for it */
TEST(battery_use_asleep)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	ui_test_start();
	ui_test_wait(2000);
	sleep_for(2 * 3600 * 1000);
	ui_test_wait(2100);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(usage_percent("Asleep"));
	CHECK_STR(usage_detail("Asleep"), "2 h");
}
