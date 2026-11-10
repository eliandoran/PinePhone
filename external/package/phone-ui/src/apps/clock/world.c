/*
 * Clock: the local time, date and time zone, and world clocks below them.
 */

#include <string.h>
#include <time.h>

#include "clock.h"

/* POSIX TZ strings, there is no zoneinfo on the phone */
static const struct {
	const char *city, *tz;
} world[] = {
	{ N_("New York"), "EST5EDT,M3.2.0,M11.1.0" },
	{ N_("London"),   "GMT0BST,M3.5.0/1,M10.5.0" },
	{ N_("Tokyo"),    "JST-9" },
	{ N_("Sydney"),   "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
#define WORLD_COUNT (sizeof(world) / sizeof(world[0]))

static struct {
	lv_obj_t *time, *seconds, *date, *zone;
	lv_obj_t *world_time[WORLD_COUNT], *world_day[WORLD_COUNT];
	lv_timer_t *timer;
} wc;

static void update(lv_timer_t *t)
{
	LV_UNUSED(t);
	ui_clock_align(wc.timer);
	char buf[64], off[16];
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);

	strftime(buf, sizeof(buf), "%H:%M", &local);
	ui_set_text(wc.time, buf);
	strftime(buf, sizeof(buf), "%S", &local);
	ui_set_text(wc.seconds, buf);
	ui_set_text_fmt(wc.date, "%s %d %s %d", ui_day_name(local.tm_wday, false), local.tm_mday,
	                ui_month_name(local.tm_mon, false), local.tm_year + 1900);
	const char *abbr = local.tm_zone && local.tm_zone[0] ? local.tm_zone : "UTC";
	ui_offset_format(off, sizeof(off), local.tm_gmtoff);
	if (strcmp(abbr, off))
		ui_set_text_fmt(wc.zone, "%s, %s", abbr, off);
	else
		ui_set_text(wc.zone, abbr);

	for (size_t i = 0; i < WORLD_COUNT; i++) {
		struct tm tm;
		ui_tz_localtime(world[i].tz, now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(wc.world_time[i], buf);

		int days = tm.tm_year != local.tm_year ? tm.tm_year - local.tm_year : tm.tm_yday - local.tm_yday;
		const char *day = days == 0 ? _("Today") : days > 0 ? _("Tomorrow") : _("Yesterday");
		long diff = tm.tm_gmtoff - local.tm_gmtoff;
		if (!diff) {
			ui_set_text_fmt(wc.world_day[i], _("%s, same time"), day);
		} else {
			ui_offset_format(off, sizeof(off), diff);
			ui_set_text_fmt(wc.world_day[i], "%s, %s h", day, off + 3); /* without "UTC" */
		}
	}
}

void world_create(lv_obj_t *page)
{
	clock_spacer(page, 40);

	lv_obj_t *row = ui_box(page);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(row, 8, 0);
	wc.time = lv_label_create(row);
	ui_set_text(wc.time, "");
	lv_obj_set_style_text_font(wc.time, UI_FONT_DISPLAY, 0);
	wc.seconds = ui_dim_label(row, "", UI_FONT_HEADLINE);
	lv_obj_set_width(wc.seconds, 44);
	lv_obj_set_style_pad_bottom(wc.seconds, 4, 0);

	wc.date = lv_label_create(page);
	ui_set_text(wc.date, "");
	lv_obj_set_style_text_font(wc.date, UI_FONT_TITLE, 0);
	lv_obj_set_style_margin_top(wc.date, 12, 0);
	wc.zone = ui_dim_label(page, "", UI_FONT_CAPTION);
	lv_obj_set_style_margin_top(wc.zone, 8, 0);

	clock_spacer(page, 32);
	lv_obj_t *title = ui_section_label(page, _("World clock"));
	lv_obj_set_width(title, LV_PCT(100));
	lv_obj_set_style_margin_bottom(title, 12, 0);

	lv_obj_t *card = ui_card_create(page);
	for (size_t i = 0; i < WORLD_COUNT; i++) {
		lv_obj_t *r = ui_row_create(card, 112, i + 1 < WORLD_COUNT, NULL, NULL);
		lv_obj_t *text = ui_row_text(r);
		lv_obj_t *city = lv_label_create(text);
		lv_label_set_text_static(city, _(world[i].city));
		wc.world_day[i] = ui_dim_label(text, "", UI_FONT_CAPTION);
		wc.world_time[i] = lv_label_create(r);
		ui_set_text(wc.world_time[i], "");
		lv_obj_set_style_text_font(wc.world_time[i], UI_FONT_HEADLINE, 0);
	}

	wc.timer = lv_timer_create(update, 1000, NULL);
	update(NULL);
}

void world_delete(void)
{
	lv_timer_delete(wc.timer);
	memset(&wc, 0, sizeof(wc));
}
