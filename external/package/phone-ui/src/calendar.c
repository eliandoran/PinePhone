/*
 * Calendar: a month grid (weeks start on Monday) with today highlighted, browsed
 * with the arrows or by swiping sideways. Tapping a day selects it and shows its
 * date, ISO week and day of the year below the grid. No events yet, nothing stored.
 */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "ui.h"

#define CELL 88      /* day cell diameter */
#define GAP_COL 12   /* 7 cells and 6 gaps fill the 688 px between the margins */
#define GAP_ROW 8
#define WEEKS 6      /* always 6 rows, so the layout does not jump between months */

static struct {
	lv_obj_t *title;
	lv_obj_t *cells[WEEKS * 7];
	lv_obj_t *date, *details, *today_button;
	lv_timer_t *timer;

	int year, month; /* shown, month 0-11 */
	long first;      /* day number of the first cell */
	long selected;   /* day number of the selected day */
	long today;
} cal;

/* Days since 1970-01-01 of a proleptic Gregorian date (month 0-11, day 1-31) */
static long days_from_civil(int y, int m, int d)
{
	y -= m < 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* The date of a day number, in UTC so that no time zone or DST shifts it */
static void civil_from_days(long days, struct tm *tm)
{
	time_t t = (time_t)days * 86400;
	gmtime_r(&t, tm);
}

/* 0 for Monday (1970-01-01 was a Thursday) */
static int weekday(long days)
{
	return (int)(((days + 3) % 7 + 7) % 7);
}

static long today_days(void)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return days_from_civil(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday);
}

static void cell_style(lv_obj_t *cell, long day)
{
	struct tm tm;
	civil_from_days(day, &tm);
	bool in_month = tm.tm_mon == cal.month;
	uint32_t bg = 0, text = in_month ? UI_COLOR_TEXT : UI_COLOR_DIM;
	bool fill = true;

	if (day == cal.today) {
		bg = UI_COLOR_ACCENT;
		text = 0xffffff;
	} else if (day == cal.selected) {
		bg = UI_COLOR_BUTTON;
	} else {
		fill = false;
	}
	lv_obj_set_style_bg_opa(cell, fill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (fill)
		lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
	lv_obj_set_style_text_color(cell, lv_color_hex(text), 0);
	/* Today stays blue when selected: a ring shows the selection */
	lv_obj_set_style_border_width(cell, day == cal.today && day == cal.selected ? 4 : 0, 0);
}

static void info_update(void)
{
	struct tm tm;
	char buf[96], rel[32];
	long diff = cal.selected - cal.today;

	civil_from_days(cal.selected, &tm);
	snprintf(buf, sizeof(buf), "%s %d %s %d", ui_day_name(tm.tm_wday, false), tm.tm_mday,
	         ui_month_name(tm.tm_mon, false), tm.tm_year + 1900);
	/* Changes on taps only, from an unbounded set: plain text */
	lv_label_set_text(cal.date, buf);

	if (diff == 0)
		snprintf(rel, sizeof(rel), "%s", _("Today"));
	else if (diff == 1)
		snprintf(rel, sizeof(rel), "%s", _("Tomorrow"));
	else if (diff == -1)
		snprintf(rel, sizeof(rel), "%s", _("Yesterday"));
	else if (diff > 0)
		snprintf(rel, sizeof(rel), P_("In %ld day", "In %ld days", diff), diff);
	else
		snprintf(rel, sizeof(rel), P_("%ld day ago", "%ld days ago", -diff), -diff);
	int year_days = days_from_civil(tm.tm_year + 1901, 0, 1) - days_from_civil(tm.tm_year + 1900, 0, 1);
	char week[8];
	strftime(week, sizeof(week), "%V", &tm);
	snprintf(buf, sizeof(buf), _("%s \xc2\xb7 Week %d \xc2\xb7 Day %d of %d"), rel, atoi(week),
	         tm.tm_yday + 1, year_days);
	lv_label_set_text(cal.details, buf);

	lv_obj_set_hidden(cal.today_button, diff == 0);
}

static void refresh(void)
{
	char buf[32];
	struct tm tm = { .tm_year = cal.year - 1900, .tm_mon = cal.month, .tm_mday = 1 };

	snprintf(buf, sizeof(buf), "%s %d", ui_month_name(cal.month, false), cal.year);
	ui_set_text(cal.title, buf);

	long first = days_from_civil(cal.year, cal.month, 1);
	cal.first = first - weekday(first);
	for (int i = 0; i < WEEKS * 7; i++) {
		long day = cal.first + i;
		civil_from_days(day, &tm);
		ui_set_text_fmt(lv_obj_get_child(cal.cells[i], 0), "%d", tm.tm_mday);
		cell_style(cal.cells[i], day);
	}
	info_update();
}

static void show_month(int year, int month)
{
	while (month < 0) {
		month += 12;
		year--;
	}
	while (month > 11) {
		month -= 12;
		year++;
	}
	cal.year = year;
	cal.month = month;
	refresh();
}

/* Shows the month of `day` and selects it */
static void select_day(long day)
{
	struct tm tm;
	civil_from_days(day, &tm);
	cal.selected = day;
	show_month(tm.tm_year + 1900, tm.tm_mon);
}

static void cell_cb(lv_event_t *e)
{
	long day = cal.first + (long)(intptr_t)lv_event_get_user_data(e);
	struct tm tm;
	civil_from_days(day, &tm);

	if (tm.tm_mon != cal.month) {
		select_day(day);
		return;
	}
	long old = cal.selected;
	cal.selected = day;
	if (old >= cal.first && old < cal.first + WEEKS * 7)
		cell_style(cal.cells[old - cal.first], old);
	cell_style(lv_event_get_target_obj(e), day);
	info_update();
}

static void month_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	show_month(cal.year, cal.month + (int)(intptr_t)lv_event_get_user_data(e));
}

static void swipe_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	/* The swipe started on a day: no tap when it ends */
	lv_indev_wait_release(lv_indev_active());
	show_month(cal.year, cal.month + (code == LV_EVENT_GESTURE_LEFT ? 1 : -1));
}

static void today_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	select_day(cal.today);
}

/* After midnight, today is another day */
static void timer_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	long today = today_days();
	if (today != cal.today) {
		cal.today = today;
		refresh();
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cal.timer)
		lv_timer_delete(cal.timer);
	memset(&cal, 0, sizeof(cal));
}

static lv_obj_t *arrow_button(lv_obj_t *parent, const char *symbol, int step)
{
	lv_obj_t *b = ui_button(parent, 96, 80, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, symbol,
	                        month_cb, (void *)(intptr_t)step);
	lv_obj_set_style_text_font(b, &ui_font_32, 0);
	return b;
}

void calendar_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Calendar"), NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_scrollable(body, false);

	/* Month and arrows */
	lv_obj_t *bar = ui_box(body);
	lv_obj_set_size(bar, LV_PCT(100), 96);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	arrow_button(bar, LV_SYMBOL_LEFT, -1);
	cal.title = lv_label_create(bar);
	lv_obj_set_style_text_font(cal.title, &ui_font_32, 0);
	lv_obj_set_flex_grow(cal.title, 1);
	lv_obj_set_style_text_align(cal.title, LV_TEXT_ALIGN_CENTER, 0);
	arrow_button(bar, LV_SYMBOL_RIGHT, 1);

	/* Swiping anywhere on the names or the grid changes the month */
	lv_obj_t *month = ui_box(body);
	lv_obj_set_size(month, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(month, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(month, GAP_COL, 0);
	lv_obj_set_style_pad_row(month, GAP_ROW, 0);
	lv_obj_set_style_pad_bottom(month, 24, 0);
	lv_obj_set_clickable(month, true);
	lv_obj_set_gesture_bubble(month, false); /* the gestures stop here */
	lv_obj_add_event_cb(month, swipe_cb, LV_EVENT_GESTURE_LEFT, NULL);
	lv_obj_add_event_cb(month, swipe_cb, LV_EVENT_GESTURE_RIGHT, NULL);

	for (int i = 0; i < 7; i++) {
		/* From Monday */
		lv_obj_t *name = ui_dim_label(month, ui_day_name((i + 1) % 7, true), &ui_font_20);
		lv_obj_set_width(name, CELL);
		lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	}
	for (int i = 0; i < WEEKS * 7; i++) {
		lv_obj_t *cell = lv_obj_create(month);
		lv_obj_remove_style_all(cell);
		lv_obj_set_size(cell, CELL, CELL);
		lv_obj_set_style_radius(cell, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_color(cell, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_font(cell, &ui_font_26, 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_center(lv_label_create(cell));
		cal.cells[i] = cell;
	}

	/* The selected day */
	lv_obj_t *card = ui_card_create(body);
	lv_obj_t *row = ui_row_create(card, 120, false, NULL, NULL);
	lv_obj_t *text = ui_row_text(row);
	cal.date = lv_label_create(text);
	lv_obj_set_style_text_font(cal.date, &ui_font_26, 0);
	cal.details = ui_dim_label(text, "", &ui_font_20);
	cal.today_button = ui_button(row, 136, 72, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Today"),
	                             today_cb, NULL);

	cal.today = today_days();
	select_day(cal.today);
	cal.timer = lv_timer_create(timer_cb, 60 * 1000, NULL);
	ui_push(scr);
}
