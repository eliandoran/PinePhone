/* Settings (settings/, the settings file of ui.c), through the UI (ui_test.h), on the
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

/* The AXP803's registers as the regmap's debugfs file lists them: its defaults, the TS pin
 * fed 80 µA, the temperature check on (0x84 bit 2 clear) */
#define PMIC_REGS "/sys/kernel/debug/regmap/sunxi-rsb-3a3/registers"
#define PMIC_DEFAULTS "33: c1\n38: a5\n39: 1f\n3c: fc\n3d: 16\n84: f2\n"

/* Its center on the screen */
static int32_t center_x(lv_obj_t *obj)
{
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	return (a.x1 + a.x2) / 2;
}

/* Its color */
static bool colored(lv_obj_t *label, uint32_t hex)
{
	return label && lv_color_eq(lv_obj_get_style_text_color(label, 0), lv_color_hex(hex));
}

/* The battery's thermistor (3 kΩ at 25 °C) read at the TS pin, on a scale with the cut-offs
 * of the power chip, which assumes 10 kΩ: charging only below 14 °C */
TEST(battery_temperature)
{
	ui_test_reset();
	ui_test_write(PMIC_REGS, PMIC_DEFAULTS);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	lv_obj_t *marker = ui_test_find("20°");        /* 300 mV / 80 µA = 3750 ohm */
	CHECK(marker);
	CHECK(!ui_test_shows("Sensor"));               /* only the scale */
	CHECK(!ui_test_shows("300 mV"));

	/* Charging in blue: 0xa5 and 0x1f x 12.8 mV; the alarm in red: 0xfc and 0x16 */
	lv_obj_t *charge_lo = ui_test_find("-18°"), *charge_hi = ui_test_find("14°");
	lv_obj_t *alarm_lo = ui_test_find("-25°"), *alarm_hi = ui_test_find("21°");
	CHECK(colored(charge_lo, UI_COLOR_ACCENT) && colored(charge_hi, UI_COLOR_ACCENT));
	CHECK(colored(alarm_lo, UI_COLOR_RED) && colored(alarm_hi, UI_COLOR_RED));
	CHECK(ui_test_shows("Charging"));
	CHECK(ui_test_shows("Alarm"));
	if (!marker || !charge_lo || !charge_hi || !alarm_lo || !alarm_hi)
		return;
	/* At their degrees: 1% of the width each */
	CHECK(center_x(alarm_lo) < center_x(charge_lo));
	CHECK(center_x(charge_hi) < center_x(marker));
	CHECK(center_x(marker) < center_x(alarm_hi));

	/* Warmer: further right, every 2 s */
	ui_test_write("/sys/bus/iio/devices/iio:device0/in_voltage0_raw", "250\n"); /* 2500 ohm */
	ui_test_wait(2100);
	marker = ui_test_find("29°");
	CHECK(marker && center_x(marker) > center_x(alarm_hi));
}

/* The check off (0x84 bit 2): the chip ignores the thermistor, it charges at any
 * temperature; the TS current from 0x84 too (bits 5:4, 40 µA here) */
TEST(battery_temperature_check_off)
{
	ui_test_reset();
	ui_test_write(PMIC_REGS, "33: c1\n38: a5\n39: 1f\n3c: fc\n3d: 16\n84: d6\n");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("5°"));                    /* 300 mV / 40 µA = 7500 ohm */
	CHECK(ui_test_shows("Charges at any temperature"));
	CHECK(!ui_test_shows("14°"));
	CHECK(!ui_test_shows("Charging"));
}

/* Without debugfs: the temperature from the default 80 µA, the cut-offs unknown; without
 * the ADC's channel, no temperature */
TEST(battery_temperature_unknown)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	CHECK(ui_test_shows("20°"));
	CHECK(ui_test_shows("Limits unknown"));

	remove("/sys/bus/iio/devices/iio:device0/in_voltage0_raw");
	ui_test_wait(2100);
	CHECK(!ui_test_shows("20°"));
	CHECK(ui_test_shows("Temperature unknown"));
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

/* The accelerometer's raw values, as the mount matrix turns them and as the screen
 * rotation takes them */
TEST(diagnostics_accelerometer)
{
	ui_test_reset();
	ui_test_sensors(true, false);
	const float up[3] = { 0, 1, 0 }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Diagnostics"));
	CHECK(ui_test_shows("iio:device1 (mpu6050)"));
	CHECK(ui_test_shows("0, 1, 0; -1, 0, 0; 0, 0, -1"));
	CHECK(ui_test_shows("-16384"));                /* raw x */
	CHECK(ui_test_shows("+9.80 m/s\xc2\xb2"));     /* the top edge's axis: 16384 x 0.000598 */
	CHECK(ui_test_shows("Top edge"));              /* up, as the matrix gives it */
	CHECK(ui_test_shows("Stays (flat, upside down or between)"));

	const float right[3] = { -1, 0, 0 }; /* its left edge up */
	ui_test_sensors_feed(right, field, 1);
	ui_test_wait(600);
	CHECK(ui_test_shows("Left edge"));
	CHECK(ui_test_shows("Right edge up (90\xc2\xb0)")); /* x and y the other way round */
}

TEST(diagnostics_no_accelerometer)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Diagnostics"));
	CHECK(ui_test_shows("No accelerometer"));
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
		CHECK(knob_shows(sliders[i]));
}
