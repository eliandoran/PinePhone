/* Settings (settings.c, the settings file of ui.c), through the UI (ui_test.h), on the
 * sysfs of ui_test_reset(): backlight 500 of 1000, battery 76% discharging */

#include <time.h>

#include "test.h"
#include "ui_test.h"

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
	CHECK(ui_test_shows("Yes"));
	CHECK(ui_test_shows("Li-ion"));

	/* Updated every 2 s */
	ui_test_write(BATTERY "capacity", "75\n");
	ui_test_write(BATTERY "status", "Charging\n");
	ui_test_wait(2100);
	CHECK(ui_test_shows("75%"));
	CHECK(ui_test_shows("Charging"));
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
	CHECK(ui_test_shows("No"));
}

/* Present, but under 2.5 V: dead */
TEST(dead_battery)
{
	ui_test_reset();
	ui_test_write(BATTERY "voltage_now", "1200000\n");
	ui_test_start();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("No battery"));
	CHECK(ui_test_shows("Dead"));
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
	CHECK(ui_test_shows("Modem not set up yet"));
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
	CHECK(ui_test_shows_part("MB free of"));
}
