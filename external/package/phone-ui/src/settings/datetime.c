/* Settings, Date & time: the clock and the time zone */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "settings.h"

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
	struct tm tm;
	ui_tz_localtime(tz, time(NULL), &tm);
	return tm.tm_gmtoff;
}

void datetime_apply(void)
{
	const char *tz = ui_setting_get("tz", NULL);
	if (tz)
		tz_apply(tz);
}

void datetime_summary(lv_obj_t *value)
{
	ui_set_text(value, tz_name());
}

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
	ui_offset_format(off, sizeof(off), tm.tm_gmtoff);
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

void datetime_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Date & time", &page);

	lv_obj_t *card = settings_panel(page);
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

	ui_section_label(page, "Time zone");
	card = ui_card_create(page);
	const struct zone *cur = zone_find(tz_current());
	for (size_t i = 0; i < ZONE_COUNT; i++) {
		char off[16];
		lv_obj_t *row = settings_row(card);
		settings_row_tappable(row, zone_cb, (void *)&zones[i]);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text_static(label, zones[i].name);
		lv_obj_t *value = settings_value(row);
		ui_offset_format(off, sizeof(off), tz_offset(zones[i].tz));
		ui_set_text(value, off);

		/* Takes its room when hidden too, so the offsets stay aligned */
		lv_obj_t *box = ui_box(row);
		lv_obj_set_size(box, 32, 32);
		dt.checks[i] = lv_label_create(box);
		lv_label_set_text_static(dt.checks[i], LV_SYMBOL_OK);
		lv_obj_set_style_text_color(dt.checks[i], lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_center(dt.checks[i]);
		lv_obj_set_hidden(dt.checks[i], &zones[i] != cur);
	}

	settings_timer(scr, datetime_update, 1000);
	ui_push(scr);
}
