/*
 * A month grid (Calendar, Tasks' due date): the month and its arrows, then the days from
 * Monday, today in the accent color, the selected day on a gray, browsed with the arrows
 * or by swiping sideways. Days are day numbers (days since 1970-01-01, no time zone).
 */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "ui.h"

#define CELL 88      /* day cell diameter */
#define GAP_COL 12   /* 7 cells and 6 gaps fill the 688 px between the margins */
#define GAP_ROW 8
#define WEEKS 6      /* always 6 rows, so the layout does not jump between months */

struct month {
	lv_obj_t *title;
	lv_obj_t *cells[WEEKS * 7];
	int year, month; /* shown, month 0-11 */
	long first;      /* day number of the first cell */
	long selected, today;
	void (*selected_cb)(long day, void *user_data);
	void *user_data;
};

long ui_days_from_civil(int y, int m, int d)
{
	y -= m < 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* In UTC, so that no time zone or DST shifts it */
void ui_civil_from_days(long days, struct tm *tm)
{
	time_t t = (time_t)days * 86400;
	gmtime_r(&t, tm);
}

int ui_weekday(long days)
{
	return (int)(((days + 3) % 7 + 7) % 7); /* 1970-01-01 was a Thursday */
}

long ui_today(void)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return ui_days_from_civil(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday);
}

static void cell_style(struct month *m, lv_obj_t *cell, long day)
{
	struct tm tm;
	ui_civil_from_days(day, &tm);
	bool in_month = tm.tm_mon == m->month;
	uint32_t bg = 0, text = in_month ? UI_COLOR_TEXT : UI_COLOR_DIM;
	bool fill = true;

	if (day == m->today) {
		bg = UI_COLOR_ACCENT;
		text = 0xffffff;
	} else if (day == m->selected) {
		bg = UI_COLOR_BUTTON;
	} else {
		fill = false;
	}
	lv_obj_set_style_bg_opa(cell, fill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (fill)
		lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
	lv_obj_set_style_text_color(cell, lv_color_hex(text), 0);
	/* Today stays blue when selected: a ring shows the selection */
	lv_obj_set_style_border_width(cell, day == m->today && day == m->selected ? 4 : 0, 0);
}

static void refresh(struct month *m)
{
	char buf[32];
	struct tm tm;

	snprintf(buf, sizeof(buf), "%s %d", ui_month_name(m->month, false), m->year);
	ui_set_text(m->title, buf);

	long first = ui_days_from_civil(m->year, m->month, 1);
	m->first = first - ui_weekday(first);
	for (int i = 0; i < WEEKS * 7; i++) {
		long day = m->first + i;
		ui_civil_from_days(day, &tm);
		ui_set_text_fmt(lv_obj_get_child(m->cells[i], 0), "%d", tm.tm_mday);
		cell_style(m, m->cells[i], day);
	}
}

static void show_month(struct month *m, int year, int month)
{
	while (month < 0) {
		month += 12;
		year--;
	}
	while (month > 11) {
		month -= 12;
		year++;
	}
	m->year = year;
	m->month = month;
	refresh(m);
}

static void select_day(struct month *m, long day)
{
	struct tm tm;
	ui_civil_from_days(day, &tm);
	m->selected = day;
	show_month(m, tm.tm_year + 1900, tm.tm_mon);
}

static void cell_cb(lv_event_t *e)
{
	lv_obj_t *cell = lv_event_get_target_obj(e);
	struct month *m = lv_obj_get_user_data(lv_obj_get_parent(cell));
	long day = m->first + (long)(intptr_t)lv_event_get_user_data(e);
	struct tm tm;
	ui_civil_from_days(day, &tm);

	if (tm.tm_mon != m->month) {
		select_day(m, day);
	} else {
		long old = m->selected;
		m->selected = day;
		if (old >= m->first && old < m->first + WEEKS * 7)
			cell_style(m, m->cells[old - m->first], old);
		cell_style(m, cell, day);
	}
	if (m->selected_cb)
		m->selected_cb(day, m->user_data);
}

static void month_cb(lv_event_t *e)
{
	lv_obj_t *button = lv_event_get_target_obj(e);
	struct month *m = lv_obj_get_user_data(lv_obj_get_parent(lv_obj_get_parent(button)));
	ui_vibrate(UI_VIBRATE_TAP);
	show_month(m, m->year, m->month + (int)(intptr_t)lv_event_get_user_data(e));
}

static void swipe_cb(lv_event_t *e)
{
	struct month *m = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
	/* The swipe started on a day: no tap when it ends */
	lv_indev_wait_release(lv_indev_active());
	show_month(m, m->year, m->month + (lv_event_get_code(e) == LV_EVENT_GESTURE_LEFT ? 1 : -1));
}

static void delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static lv_obj_t *arrow_button(lv_obj_t *parent, const char *symbol, int step)
{
	lv_obj_t *b = ui_button(parent, 96, 80, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, symbol,
	                        month_cb, (void *)(intptr_t)step);
	lv_obj_set_style_text_font(b, &ui_font_32, 0);
	return b;
}

lv_obj_t *ui_month_create(lv_obj_t *parent, long selected, void (*selected_cb)(long day, void *user_data),
                          void *user_data)
{
	struct month *m = calloc(1, sizeof(*m));
	m->selected_cb = selected_cb;
	m->user_data = user_data;

	lv_obj_t *box = ui_box(parent);
	lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_user_data(box, m);
	lv_obj_add_event_cb(box, delete_cb, LV_EVENT_DELETE, m);

	/* Month and arrows */
	lv_obj_t *bar = ui_box(box);
	lv_obj_set_size(bar, LV_PCT(100), 96);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	arrow_button(bar, LV_SYMBOL_LEFT, -1);
	m->title = lv_label_create(bar);
	lv_obj_set_style_text_font(m->title, UI_FONT_HEADLINE, 0);
	lv_obj_set_flex_grow(m->title, 1);
	lv_obj_set_style_text_align(m->title, LV_TEXT_ALIGN_CENTER, 0);
	arrow_button(bar, LV_SYMBOL_RIGHT, 1);

	/* Swiping anywhere on the names or the grid changes the month */
	lv_obj_t *grid = ui_box(box);
	lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(grid, GAP_COL, 0);
	lv_obj_set_style_pad_row(grid, GAP_ROW, 0);
	lv_obj_set_style_pad_bottom(grid, 24, 0);
	lv_obj_set_clickable(grid, true);
	lv_obj_set_gesture_bubble(grid, false); /* the gestures stop here */
	lv_obj_set_user_data(grid, m);
	lv_obj_add_event_cb(grid, swipe_cb, LV_EVENT_GESTURE_LEFT, NULL);
	lv_obj_add_event_cb(grid, swipe_cb, LV_EVENT_GESTURE_RIGHT, NULL);

	for (int i = 0; i < 7; i++) {
		/* From Monday */
		lv_obj_t *name = ui_dim_label(grid, ui_day_name((i + 1) % 7, true), UI_FONT_CAPTION);
		lv_obj_set_width(name, CELL);
		lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	}
	for (int i = 0; i < WEEKS * 7; i++) {
		lv_obj_t *cell = lv_obj_create(grid);
		lv_obj_remove_style_all(cell);
		lv_obj_set_size(cell, CELL, CELL);
		lv_obj_set_style_radius(cell, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_color(cell, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_font(cell, UI_FONT_TITLE, 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_center(lv_label_create(cell));
		m->cells[i] = cell;
	}

	m->today = ui_today();
	/* None: the month of today */
	select_day(m, selected < 0 ? m->today : selected);
	if (selected < 0) {
		m->selected = -1;
		refresh(m);
	}
	return box;
}

void ui_month_select(lv_obj_t *month, long day)
{
	select_day(lv_obj_get_user_data(month), day);
}

void ui_month_set_today(lv_obj_t *month, long today)
{
	struct month *m = lv_obj_get_user_data(month);
	if (m->today != today) {
		m->today = today;
		refresh(m);
	}
}
