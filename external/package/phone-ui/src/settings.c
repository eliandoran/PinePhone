/*
 * Settings: a list of sections, each a screen showing what the phone reports
 * (sysfs, procfs, statvfs...). settings_apply() restores the saved settings at
 * startup, before LVGL runs.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq"
#define THERMAL "/sys/class/thermal"
#define IIO "/sys/bus/iio/devices"

#define BRIGHTNESS_MIN 5 /* percent: a black screen cannot be turned back up */

/* Time zones: there is no zoneinfo on the phone, hence POSIX TZ strings */
static const struct zone {
	const char *name, *tz;
} zones[] = {
	{ "Los Angeles",        "PST8PDT,M3.2.0,M11.1.0" },
	{ "Denver",             "MST7MDT,M3.2.0,M11.1.0" },
	{ "Chicago",            "CST6CDT,M3.2.0,M11.1.0" },
	{ "New York",           "EST5EDT,M3.2.0,M11.1.0" },
	{ "Sao Paulo",          "<-03>3" },
	{ "UTC",                "UTC0" },
	{ "London",             "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Paris, Berlin",      "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Bucharest, Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Moscow",             "MSK-3" },
	{ "Dubai",              "<+04>-4" },
	{ "India",              "IST-5:30" },
	{ "Shanghai",           "CST-8" },
	{ "Tokyo",              "JST-9" },
	{ "Sydney",             "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
#define ZONE_COUNT (sizeof(zones) / sizeof(zones[0]))

/* Backlight: the first device of the class */

static const char *backlight(void)
{
	static char path[300];
	static bool searched;

	if (!searched) {
		searched = true;
		DIR *dir = opendir("/sys/class/backlight");
		struct dirent *ent;
		while (dir && !path[0] && (ent = readdir(dir)))
			if (ent->d_name[0] != '.')
				snprintf(path, sizeof(path), "/sys/class/backlight/%s", ent->d_name);
		if (dir)
			closedir(dir);
	}
	return path[0] ? path : NULL;
}

static long backlight_max(void)
{
	char path[320];
	long max;
	if (!backlight())
		return 0;
	snprintf(path, sizeof(path), "%s/max_brightness", backlight());
	return ui_read_long(path, &max) && max > 0 ? max : 0;
}

static void brightness_set(int percent)
{
	long max = backlight_max();
	char path[320];
	if (!max)
		return;
	if (percent < BRIGHTNESS_MIN)
		percent = BRIGHTNESS_MIN;
	if (percent > 100)
		percent = 100;
	long raw = (max * percent + 50) / 100;
	snprintf(path, sizeof(path), "%s/brightness", backlight());
	ui_write_long(path, raw < 1 ? 1 : raw);
}

static int brightness_get(void)
{
	long max = backlight_max(), raw;
	char path[320];
	if (!max)
		return -1;
	snprintf(path, sizeof(path), "%s/brightness", backlight());
	if (!ui_read_long(path, &raw))
		return -1;
	return (int)((raw * 100 + max / 2) / max);
}

/* Time zones */

static void tz_apply(const char *tz)
{
	setenv("TZ", tz, 1);
	tzset();
}

static const struct zone *zone_find(const char *tz)
{
	for (size_t i = 0; i < ZONE_COUNT; i++)
		if (strcmp(zones[i].tz, tz) == 0)
			return &zones[i];
	return NULL;
}

static const char *tz_current(void)
{
	return ui_setting_get("tz", "UTC0");
}

static const char *tz_name(void)
{
	const struct zone *z = zone_find(tz_current());
	return z ? z->name : tz_current();
}

/* UTC offset of `tz` now, in seconds, without leaving it set */
static long tz_offset(const char *tz)
{
	const char *cur = getenv("TZ");
	char *saved = cur ? strdup(cur) : NULL;
	time_t now = time(NULL);
	struct tm tm;

	tz_apply(tz);
	localtime_r(&now, &tm);
	if (saved) {
		tz_apply(saved);
		free(saved);
	} else {
		unsetenv("TZ");
		tzset();
	}
	return tm.tm_gmtoff;
}

static void format_offset(char *buf, size_t size, long seconds)
{
	int off = (int)(seconds / 60 % (24 * 60)); /* minutes */
	char sign = off < 0 ? '-' : '+';
	if (off < 0)
		off = -off;
	if (off == 0)
		snprintf(buf, size, "UTC");
	else if (off % 60)
		snprintf(buf, size, "UTC%c%d:%02d", sign, off / 60, off % 60);
	else
		snprintf(buf, size, "UTC%c%d", sign, off / 60);
}

void settings_apply(void)
{
	const char *tz = ui_setting_get("tz", NULL);
	if (tz)
		tz_apply(tz);
	ui_haptics = ui_setting_get_int("haptics", 1) != 0;
	int brightness = ui_setting_get_int("brightness", -1);
	if (brightness >= 0)
		brightness_set(brightness);
}

/*
 * Building blocks: a scrolling page of cards, cards holding rows separated by
 * lines, rows with a title on the left and a value on the right.
 */

static void timer_delete_cb(lv_event_t *e)
{
	lv_timer_delete(lv_event_get_user_data(e));
}

/* A timer that lives as long as `scr` and runs once right away */
static void screen_timer(lv_obj_t *scr, lv_timer_cb_t cb, uint32_t period)
{
	lv_timer_t *t = lv_timer_create(cb, period, NULL);
	lv_obj_add_event_cb(scr, timer_delete_cb, LV_EVENT_DELETE, t);
	cb(t);
}

static lv_obj_t *page_create(const char *title, lv_obj_t **page)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_set_style_pad_hor(body, 20, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 40, 0);
	lv_obj_set_style_pad_row(body, 16, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	*page = body;
	return scr;
}

static void section_create(lv_obj_t *page, const char *text)
{
	lv_obj_t *label = lv_label_create(page);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_left(label, 16, 0);
	lv_obj_set_style_pad_top(label, 16, 0);
}

/* A card with its content padded, for anything that is not rows */
static lv_obj_t *panel_create(lv_obj_t *parent)
{
	lv_obj_t *card = ui_card_create(parent);
	lv_obj_set_style_pad_all(card, 28, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	return card;
}

static lv_obj_t *row_create(lv_obj_t *card)
{
	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(row, 24, 0);
	lv_obj_set_style_pad_ver(row, 16, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	/* At least 88 px high with the content centered. A min_height would leave it at the top
	 * (LVGL places the line of a content-sized flex at the start), so instead an invisible
	 * strut makes the line 56 px high and the items are centered on it. The negative margin
	 * cancels the column gap after it. */
	lv_obj_t *strut = lv_obj_create(row);
	lv_obj_remove_style_all(strut);
	lv_obj_set_size(strut, 0, 56);
	lv_obj_set_style_margin_right(strut, -20, 0);
	lv_obj_set_clickable(strut, false);
	if (lv_obj_get_child_count(card) > 1) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	}
	return row;
}

static lv_obj_t *value_create(lv_obj_t *row)
{
	lv_obj_t *value = lv_label_create(row);
	ui_set_text(value, "");
	lv_obj_set_flex_grow(value, 1);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_color(value, lv_color_hex(UI_COLOR_DIM), 0);
	lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
	return value;
}

/* A row with a title and a value; returns the value label */
static lv_obj_t *info_row(lv_obj_t *card, const char *title)
{
	lv_obj_t *row = row_create(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, title);
	return value_create(row);
}

/* Settings list */

static struct {
	lv_obj_t *display, *sound, *datetime, *battery, *network, *storage, *about;
} list;

static void display_open(void);
static void sound_open(void);
static void datetime_open(void);
static void battery_open(void);
static void network_open(void);
static void storage_open(void);
static void about_open(void);

static void open_cb(lv_event_t *e)
{
	void (*open)(void) = (void (*)(void))lv_event_get_user_data(e);
	open();
}

static lv_obj_t *nav_row(lv_obj_t *card, const char *symbol, uint32_t color, const char *title,
                         void (*open)(void))
{
	lv_obj_t *row = row_create(card);
	lv_obj_set_style_pad_left(row, 20, 0);
	lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, open_cb, LV_EVENT_CLICKED, (void *)open);

	lv_obj_t *icon = lv_obj_create(row);
	lv_obj_remove_style_all(icon);
	lv_obj_set_size(icon, 60, 60);
	lv_obj_set_style_radius(icon, 16, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_set_clickable(icon, false);
	lv_obj_t *sym = lv_label_create(icon);
	lv_label_set_text_static(sym, symbol);
	lv_obj_set_style_text_color(sym, lv_color_white(), 0);
	lv_obj_center(sym);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, title);

	lv_obj_t *value = value_create(row);

	lv_obj_t *chevron = lv_label_create(row);
	lv_label_set_text_static(chevron, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_font(chevron, &ui_font_20, 0);
	lv_obj_set_style_text_color(chevron, lv_color_hex(UI_COLOR_DIM), 0);
	return value;
}

static bool battery_read(int *capacity, char *status, size_t size)
{
	long v;
	if (!ui_read_long(UI_BATTERY "/capacity", &v) || !ui_battery_present())
		return false;
	*capacity = (int)v;
	if (!ui_read_line(UI_BATTERY "/status", status, size))
		status[0] = '\0';
	return true;
}

/* IPv4 address of the first interface whose name starts with `prefix` */
static bool if_address(const char *prefix, char *buf, size_t size)
{
	struct ifaddrs *ifs, *i;
	bool found = false;
	if (getifaddrs(&ifs) < 0)
		return false;
	for (i = ifs; i && !found; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
		    strncmp(i->ifa_name, prefix, strlen(prefix)) == 0)
			found = inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
			                  buf, (socklen_t)size) != NULL;
	freeifaddrs(ifs);
	return found;
}

static void list_update(void)
{
	char buf[64];

	int b = brightness_get();
	if (b >= 0)
		ui_set_text_fmt(list.display, "%d%%", b);
	else
		ui_set_text(list.display, "");
	ui_set_text(list.sound, ui_haptics ? "Vibrate" : "Silent");
	ui_set_text(list.datetime, tz_name());

	int capacity;
	char status[32];
	if (battery_read(&capacity, status, sizeof(status)))
		ui_set_text_fmt(list.battery, "%d%%", capacity);
	else
		ui_set_text(list.battery, "None");

	ui_set_text(list.network, if_address("usb", buf, sizeof(buf)) ? "USB" : "Offline");

	struct statvfs vfs;
	if (statvfs("/data", &vfs) == 0) {
		ui_size_format(buf, sizeof(buf), (unsigned long long)vfs.f_bavail * vfs.f_frsize);
		ui_set_text_fmt(list.storage, "%s free", buf);
	}
}

static void list_loaded_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	list_update();
}

void settings_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Settings", &page);

	lv_obj_t *card = ui_card_create(page);
	list.display = nav_row(card, LV_SYMBOL_IMAGE, 0x0a84ff, "Display", display_open);
	list.sound = nav_row(card, LV_SYMBOL_VOLUME_MAX, 0xff375f, "Sound & vibration", sound_open);
	list.datetime = nav_row(card, LV_SYMBOL_BELL, 0xff9f0a, "Date & time", datetime_open);

	card = ui_card_create(page);
	list.battery = nav_row(card, LV_SYMBOL_BATTERY_3, 0x34c759, "Battery", battery_open);
	list.network = nav_row(card, LV_SYMBOL_WIFI, 0x30b0c7, "Network", network_open);
	list.storage = nav_row(card, LV_SYMBOL_SD_CARD, 0x8e8e93, "Storage", storage_open);

	card = ui_card_create(page);
	list.about = nav_row(card, LV_SYMBOL_LIST, 0x5e5ce6, "About phone", about_open);
	ui_set_text(list.about, "PinePhone");

	/* Coming back from a section: its value may have changed */
	lv_obj_add_event_cb(scr, list_loaded_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	list_update();
	ui_push(scr);
}

/* Display */

static lv_obj_t *brightness_label;

static void brightness_cb(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target_obj(e);
	int v = (int)lv_slider_get_value(slider);
	brightness_set(v);
	ui_set_text_fmt(brightness_label, "%d%%", v);
	/* Saving writes and syncs the file: once the finger is up */
	if (lv_event_get_code(e) == LV_EVENT_RELEASED)
		ui_setting_set_int("brightness", v);
}

static lv_obj_t *slider_create(lv_obj_t *parent)
{
	lv_obj_t *slider = lv_slider_create(parent);
	lv_obj_set_height(slider, 14);
	lv_obj_set_flex_grow(slider, 1);
	lv_obj_set_ext_click_area(slider, 36);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(slider, 16, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
	return slider;
}

/* The screens are rebuilt in the new theme's colors, this one included */
static void display_reopen(void)
{
	settings_open();
	display_open();
}

static void theme_cb(lv_event_t *e)
{
	ui_set_light((uintptr_t)lv_event_get_user_data(e) != 0, display_reopen);
}

static void display_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Display", &page);

	section_create(page, "Brightness");
	lv_obj_t *card = panel_create(page);
	int b = brightness_get();
	if (b < 0) {
		lv_obj_t *label = lv_label_create(card);
		lv_label_set_text_static(label, "No backlight found");
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	} else {
		lv_obj_t *top = lv_obj_create(card);
		lv_obj_remove_style_all(top);
		lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_t *label = lv_label_create(top);
		lv_label_set_text_static(label, "Screen brightness");
		brightness_label = lv_label_create(top);
		ui_set_text_fmt(brightness_label, "%d%%", b);
		lv_obj_set_style_text_color(brightness_label, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_align(brightness_label, LV_ALIGN_RIGHT_MID, 0, 0);

		lv_obj_t *row = lv_obj_create(card);
		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, LV_PCT(100), 88);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 32, 0);

		lv_obj_t *dim = lv_label_create(row);
		lv_label_set_text_static(dim, LV_SYMBOL_IMAGE);
		lv_obj_set_style_text_font(dim, &ui_font_20, 0);
		lv_obj_set_style_text_color(dim, lv_color_hex(UI_COLOR_DIM), 0);

		lv_obj_t *slider = slider_create(row);
		lv_slider_set_range(slider, BRIGHTNESS_MIN, 100);
		lv_slider_set_value(slider, b, LV_ANIM_OFF);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, NULL);

		lv_obj_t *bright = lv_label_create(row);
		lv_label_set_text_static(bright, LV_SYMBOL_IMAGE);
		lv_obj_set_style_text_font(bright, &ui_font_32, 0);
	}

	section_create(page, "Appearance");
	card = ui_card_create(page);
	for (uintptr_t light = 0; light <= 1; light++) {
		lv_obj_t *row = row_create(card);
		lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_clickable(row, true);
		lv_obj_add_event_cb(row, theme_cb, LV_EVENT_CLICKED, (void *)light);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, light ? "Light" : "Dark");
		lv_obj_set_flex_grow(label, 1);
		if (light == ui_light) {
			lv_obj_t *check = lv_label_create(row);
			lv_label_set_text_static(check, LV_SYMBOL_OK);
			lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
		}
	}

	section_create(page, "Screen");
	card = ui_card_create(page);
	lv_display_t *disp = lv_display_get_default();
	ui_set_text_fmt(info_row(card, "Resolution"), "%d x %d",
	                (int)lv_display_get_horizontal_resolution(disp),
	                (int)lv_display_get_vertical_resolution(disp));
	ui_set_text_fmt(info_row(card, "Density"), "%d dpi", (int)lv_display_get_dpi(disp));
	ui_set_text_fmt(info_row(card, "Animations"), "%u ms", (unsigned)ui_anim_ms);
	ui_push(scr);
}

/* Sound & vibration */

static void haptics_cb(lv_event_t *e)
{
	ui_haptics = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
	ui_setting_set_int("haptics", ui_haptics);
	ui_vibrate(30);
}

static void test_vibration_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* Testing works with touch vibration off as well */
	bool haptics = ui_haptics;
	ui_haptics = true;
	ui_vibrate(300);
	ui_haptics = haptics;
}

static lv_obj_t *switch_create(lv_obj_t *parent, bool on)
{
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_size(sw, 100, 56);
	lv_obj_set_ext_click_area(sw, 16);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(0x34c759), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUSED);
	lv_obj_set_state(sw, LV_STATE_CHECKED, on);
	return sw;
}

static lv_obj_t *wide_button(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_size(button, LV_PCT(100), 96);
	lv_obj_set_style_radius(button, 24, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_center(label);
	return button;
}

static void sound_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Sound & vibration", &page);

	section_create(page, "Vibration");
	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = row_create(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, "Vibrate on touch");
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = switch_create(row, ui_haptics);
	lv_obj_add_event_cb(sw, haptics_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *button = wide_button(page, LV_SYMBOL_BELL "  Test vibration", UI_COLOR_ACCENT);
	lv_obj_add_event_cb(button, test_vibration_cb, LV_EVENT_CLICKED, NULL);

	section_create(page, "Sound");
	card = ui_card_create(page);
	ui_set_text(info_row(card, "Ringtone"), "Not set up yet");
	ui_push(scr);
}

/* Date & time */

static struct {
	lv_obj_t *time, *date, *zone;
	lv_obj_t *checks[ZONE_COUNT];
} dt;

static void datetime_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[64], off[16];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(dt.time, buf);
	strftime(buf, sizeof(buf), "%A %-d %B %Y", &tm);
	ui_set_text(dt.date, buf);
	format_offset(off, sizeof(off), tm.tm_gmtoff);
	ui_set_text_fmt(dt.zone, "%s (%s)", tz_name(), off);
}

static void zone_cb(lv_event_t *e)
{
	const struct zone *z = lv_event_get_user_data(e);
	tz_apply(z->tz);
	ui_setting_set("tz", z->tz);
	for (size_t i = 0; i < ZONE_COUNT; i++)
		lv_obj_set_hidden(dt.checks[i], &zones[i] != z);
	datetime_update(NULL);
	ui_status_update();
}

static void datetime_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Date & time", &page);

	lv_obj_t *card = panel_create(page);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_ver(card, 36, 0);
	dt.time = lv_label_create(card);
	ui_set_text(dt.time, "");
	lv_obj_set_style_text_font(dt.time, &ui_font_48, 0);
	dt.date = lv_label_create(card);
	ui_set_text(dt.date, "");
	lv_obj_set_style_text_font(dt.date, &ui_font_26, 0);
	dt.zone = lv_label_create(card);
	ui_set_text(dt.zone, "");
	lv_obj_set_style_text_color(dt.zone, lv_color_hex(UI_COLOR_DIM), 0);

	section_create(page, "Time zone");
	card = ui_card_create(page);
	const struct zone *cur = zone_find(tz_current());
	for (size_t i = 0; i < ZONE_COUNT; i++) {
		char off[16];
		lv_obj_t *row = row_create(card);
		lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_clickable(row, true);
		lv_obj_add_event_cb(row, zone_cb, LV_EVENT_CLICKED, (void *)&zones[i]);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, zones[i].name);
		lv_obj_t *value = value_create(row);
		format_offset(off, sizeof(off), tz_offset(zones[i].tz));
		ui_set_text(value, off);

		/* Takes its room when hidden too, so the offsets stay aligned */
		lv_obj_t *box = lv_obj_create(row);
		lv_obj_remove_style_all(box);
		lv_obj_set_size(box, 32, 32);
		lv_obj_set_clickable(box, false);
		dt.checks[i] = lv_label_create(box);
		lv_label_set_text_static(dt.checks[i], LV_SYMBOL_OK);
		lv_obj_set_style_text_color(dt.checks[i], lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_center(dt.checks[i]);
		lv_obj_set_hidden(dt.checks[i], &zones[i] != cur);
	}

	screen_timer(scr, datetime_update, 1000);
	ui_push(scr);
}

/* Battery */

static struct {
	lv_obj_t *capacity, *status, *bar;
	lv_obj_t *health, *voltage, *current, *present, *technology;
} bat;

static void battery_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[64];
	long v;
	int capacity;

	if (!battery_read(&capacity, buf, sizeof(buf))) {
		ui_set_text(bat.capacity, "--");
		ui_set_text(bat.status, ui_battery_present() ? "No battery driver" : "No battery");
		lv_bar_set_value(bat.bar, 0, LV_ANIM_OFF);
	} else {
		ui_set_text_fmt(bat.capacity, "%d%%", capacity);
		ui_set_text(bat.status, buf[0] ? buf : "Unknown");
		lv_bar_set_value(bat.bar, capacity, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(bat.bar, capacity < 15 ? lv_palette_main(LV_PALETTE_RED) :
		                          lv_color_hex(0x34c759), LV_PART_INDICATOR);
	}

	ui_set_text(bat.health, ui_read_line(UI_BATTERY "/health", buf, sizeof(buf)) ? buf : "Unknown");
	ui_set_text(bat.technology, ui_read_line(UI_BATTERY "/technology", buf, sizeof(buf)) ? buf : "Unknown");
	if (ui_read_long(UI_BATTERY "/voltage_now", &v))
		ui_set_text_fmt(bat.voltage, "%.2f V", v / 1e6);
	else
		ui_set_text(bat.voltage, "Unknown");
	/* Rounded to 10 mA: each distinct text is kept */
	if (ui_read_long(UI_BATTERY "/current_now", &v))
		ui_set_text_fmt(bat.current, "%ld mA", (v / 1000 + (v < 0 ? -5 : 5)) / 10 * 10);
	else
		ui_set_text(bat.current, "Unknown");
	if (ui_read_long(UI_BATTERY "/present", &v))
		ui_set_text(bat.present, !v ? "No" : ui_battery_present() ? "Yes" : "Dead");
	else
		ui_set_text(bat.present, "Unknown");
}

static void battery_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Battery", &page);

	lv_obj_t *card = panel_create(page);
	lv_obj_set_style_pad_row(card, 8, 0);
	bat.capacity = lv_label_create(card);
	ui_set_text(bat.capacity, "");
	lv_obj_set_style_text_font(bat.capacity, &ui_font_48, 0);
	bat.status = lv_label_create(card);
	ui_set_text(bat.status, "");
	lv_obj_set_style_text_color(bat.status, lv_color_hex(UI_COLOR_DIM), 0);
	bat.bar = lv_bar_create(card);
	lv_obj_set_size(bat.bar, LV_PCT(100), 20);
	lv_obj_set_style_margin_top(bat.bar, 16, 0);
	lv_obj_set_style_bg_color(bat.bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_bar_set_range(bat.bar, 0, 100);

	section_create(page, "Details");
	card = ui_card_create(page);
	bat.health = info_row(card, "Health");
	bat.voltage = info_row(card, "Voltage");
	bat.current = info_row(card, "Current");
	bat.present = info_row(card, "Present");
	bat.technology = info_row(card, "Technology");

	screen_timer(scr, battery_update, 2000);
	ui_push(scr);
}

/* Network */

static void network_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Network", &page);

	section_create(page, "Connections");
	lv_obj_t *card = ui_card_create(page);
	struct ifaddrs *ifs, *i;
	unsigned count = 0;
	if (getifaddrs(&ifs) == 0) {
		for (i = ifs; i; i = i->ifa_next) {
			char addr[INET_ADDRSTRLEN], title[64];
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK))
				continue;
			if (!inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof(addr)))
				continue;
			snprintf(title, sizeof(title), "%s (%s)",
			         strncmp(i->ifa_name, "usb", 3) == 0 ? "USB" : "Network", i->ifa_name);
			ui_set_text(info_row(card, ui_intern(title)), addr);
			count++;
		}
		freeifaddrs(ifs);
	}
	if (!count)
		ui_set_text(info_row(card, "USB"), "Not connected");

	section_create(page, "Wireless");
	card = ui_card_create(page);
	ui_set_text(info_row(card, "Mobile network"), "Modem not set up yet");
	ui_set_text(info_row(card, "Wi-Fi"), "Not available");
	ui_set_text(info_row(card, "Bluetooth"), "Not available");
	ui_push(scr);
}

/* Storage */

static void storage_card(lv_obj_t *page, const char *title, const char *path)
{
	char used[32], total[32], avail[32];
	struct statvfs vfs;
	struct stat st, root;

	lv_obj_t *card = panel_create(page);
	lv_obj_t *top = lv_obj_create(card);
	lv_obj_remove_style_all(top);
	lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_t *label = lv_label_create(top);
	lv_label_set_text_static(label, title);
	lv_obj_t *value = lv_label_create(top);
	lv_obj_set_style_text_color(value, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);

	/* /data unmounted is just a directory of the root file system */
	bool mounted = stat(path, &st) == 0 && stat("/", &root) == 0 &&
	               (strcmp(path, "/") == 0 || st.st_dev != root.st_dev);
	if (!mounted || statvfs(path, &vfs) != 0 || !vfs.f_blocks) {
		ui_set_text(value, "Not mounted");
		return;
	}

	unsigned long long t = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
	unsigned long long u = (unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize;
	unsigned long long a = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
	ui_size_format(used, sizeof(used), u);
	ui_size_format(total, sizeof(total), t);
	ui_size_format(avail, sizeof(avail), a);
	ui_set_text_fmt(value, "%s of %s", used, total);

	lv_obj_t *bar = lv_bar_create(card);
	lv_obj_set_size(bar, LV_PCT(100), 20);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_bar_set_range(bar, 0, 1000);
	lv_bar_set_value(bar, (int32_t)(t ? u * 1000 / t : 0), LV_ANIM_OFF);

	lv_obj_t *free_label = lv_label_create(card);
	ui_set_text_fmt(free_label, "%s free%s", avail, vfs.f_flag & ST_RDONLY ? ", read-only" : "");
	lv_obj_set_style_text_font(free_label, &ui_font_20, 0);
	lv_obj_set_style_text_color(free_label, lv_color_hex(UI_COLOR_DIM), 0);
}

static void storage_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("Storage", &page);
	storage_card(page, "System", "/");
	storage_card(page, "Data", "/data");
	ui_push(scr);
}

/* About phone */

static struct {
	lv_obj_t *uptime, *memory, *freq;
	lv_obj_t *cpu_temp, *gpu_temp, *pmic_temp;
} about;

/* The AXP803 ADC: the IIO device whose sysfs path goes through axp813-adc */
static const char *pmic_adc(void)
{
	static char path[300];
	static bool searched;

	if (!searched) {
		searched = true;
		DIR *dir = opendir(IIO);
		struct dirent *ent;
		char link[300], real[PATH_MAX];
		while (dir && !path[0] && (ent = readdir(dir))) {
			snprintf(link, sizeof(link), IIO "/%s", ent->d_name);
			if (ent->d_name[0] != '.' && realpath(link, real) && strstr(real, "axp813-adc"))
				snprintf(path, sizeof(path), "%s", link);
		}
		if (dir)
			closedir(dir);
	}
	return path[0] ? path : NULL;
}

/* Thermal zone in millidegrees: 0 cpu0, 1 gpu0, 2 gpu1 */
static bool zone_temp(int zone, long *mc)
{
	char path[64];
	snprintf(path, sizeof(path), THERMAL "/thermal_zone%d/temp", zone);
	return ui_read_long(path, mc);
}

/* IIO temperature channel in millidegrees: (raw + offset) * scale */
static bool iio_temp(const char *dev, long *mc)
{
	char path[340];
	long raw, offset, scale;
	if (!dev)
		return false;
	snprintf(path, sizeof(path), "%s/in_temp_raw", dev);
	if (!ui_read_long(path, &raw))
		return false;
	snprintf(path, sizeof(path), "%s/in_temp_offset", dev);
	if (!ui_read_long(path, &offset))
		return false;
	snprintf(path, sizeof(path), "%s/in_temp_scale", dev);
	if (!ui_read_long(path, &scale))
		return false;
	*mc = (raw + offset) * scale;
	return true;
}

/* Whole degrees: each distinct text is kept */
static void temp_text(lv_obj_t *label, bool ok, long mc)
{
	if (ok)
		ui_set_text_fmt(label, "%ld °C", (mc + (mc < 0 ? -500 : 500)) / 1000);
	else
		ui_set_text(label, "Unknown");
}

static void temp_update(void)
{
	long cpu, gpu0, gpu1, pmic, trip;

	/* Orange within 10 °C of the first trip point (throttling starts), red past it */
	bool ok = zone_temp(0, &cpu);
	temp_text(about.cpu_temp, ok, cpu);
	if (!ui_read_long(THERMAL "/thermal_zone0/trip_point_0_temp", &trip))
		trip = 75000;
	lv_obj_set_style_text_color(about.cpu_temp,
	                            !ok || cpu < trip - 10000 ? lv_color_hex(UI_COLOR_DIM) :
	                            cpu < trip ? lv_color_hex(0xff9f0a) : lv_palette_main(LV_PALETTE_RED), 0);

	/* Two sensors on the GPU: the hotter one */
	bool ok0 = zone_temp(1, &gpu0), ok1 = zone_temp(2, &gpu1);
	temp_text(about.gpu_temp, ok0 || ok1, ok0 && (!ok1 || gpu0 > gpu1) ? gpu0 : gpu1);

	ok = iio_temp(pmic_adc(), &pmic);
	temp_text(about.pmic_temp, ok, pmic);
}

static void about_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	struct sysinfo si;
	char buf[64];
	long khz;

	if (sysinfo(&si) == 0) {
		long up = si.uptime;
		if (up >= 86400)
			ui_set_text_fmt(about.uptime, "%ld d %ld h %ld min", up / 86400, up / 3600 % 24, up / 60 % 60);
		else if (up >= 3600)
			ui_set_text_fmt(about.uptime, "%ld h %ld min", up / 3600, up / 60 % 60);
		else
			ui_set_text_fmt(about.uptime, "%ld min", up / 60);
		unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
		ui_set_text_fmt(about.memory, "%llu MB free of %llu MB",
		                (si.freeram + si.bufferram) * unit >> 20, si.totalram * unit >> 20);
	}

	if (ui_read_long(CPUFREQ "/scaling_cur_freq", &khz)) {
		if (!ui_read_line(CPUFREQ "/scaling_governor", buf, sizeof(buf)))
			buf[0] = '\0';
		ui_set_text_fmt(about.freq, buf[0] ? "%ld MHz (%s)" : "%ld MHz", khz / 1000, buf);
	} else {
		ui_set_text(about.freq, "Unknown");
	}

	temp_update();
}

static const char *boot_slot(void)
{
	char cmdline[1024];
	if (!ui_read_line("/proc/cmdline", cmdline, sizeof(cmdline)))
		return "Unknown";
	const char *root = strstr(cmdline, "root=PARTUUID=50480001-0");
	if (!root)
		return "Unknown";
	switch (root[strlen("root=PARTUUID=50480001-0")]) {
	case '1': return "A (partition 1)";
	case '2': return "B (partition 2)";
	default:  return "Unknown";
	}
}

static void about_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = page_create("About phone", &page);
	char buf[128];
	struct utsname un;

	/* Big model name */
	lv_obj_t *card = panel_create(page);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *icon = lv_label_create(card);
	lv_label_set_text_static(icon, LV_SYMBOL_CALL);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_t *model = lv_label_create(card);
	lv_obj_set_style_text_font(model, &ui_font_32, 0);
	/* The device tree string ends with a NUL, not a newline */
	if (!ui_read_line("/proc/device-tree/model", buf, sizeof(buf)))
		snprintf(buf, sizeof(buf), "PinePhone");
	ui_set_text(model, buf);

	section_create(page, "Software");
	card = ui_card_create(page);
	if (uname(&un) == 0)
		ui_set_text_fmt(info_row(card, "Kernel"), "Linux %s (%s)", un.release, un.machine);
	ui_set_text_fmt(info_row(card, "LVGL"), "%d.%d.%d", lv_version_major(), lv_version_minor(),
	                lv_version_patch());
	ui_set_text(info_row(card, "Boot slot"), boot_slot());
	about.uptime = info_row(card, "Uptime");

	section_create(page, "Hardware");
	card = ui_card_create(page);
	ui_set_text(info_row(card, "SoC"), "Allwinner A64");
	ui_set_text_fmt(info_row(card, "CPU"), "%ld x Cortex-A53", sysconf(_SC_NPROCESSORS_ONLN));
	about.freq = info_row(card, "CPU frequency");
	ui_set_text(info_row(card, "GPU"), "Mali-400 MP2 (Mesa lima)");
	about.memory = info_row(card, "Memory");

	section_create(page, "Temperature");
	card = ui_card_create(page);
	about.cpu_temp = info_row(card, "CPU");
	about.gpu_temp = info_row(card, "GPU");
	about.pmic_temp = info_row(card, "Power chip");

	screen_timer(scr, about_update, 2000);
	ui_push(scr);
}
