/*
 * Calendar: a month grid (month.c: weeks start on Monday) with today highlighted, browsed
 * with the arrows or by swiping sideways. Tapping a day selects it and shows its
 * date, ISO week and day of the year below the grid. No events yet, nothing stored.
 */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "ui.h"

static struct {
	lv_obj_t *month;
	lv_obj_t *date, *details, *today_button;
	lv_timer_t *timer;
	long selected; /* day number of the selected day */
	long today;
} cal;

static void info_update(void)
{
	struct tm tm;
	char buf[96], rel[32];
	long diff = cal.selected - cal.today;

	ui_civil_from_days(cal.selected, &tm);
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
	int year_days = ui_days_from_civil(tm.tm_year + 1901, 0, 1) - ui_days_from_civil(tm.tm_year + 1900, 0, 1);
	char week[8];
	strftime(week, sizeof(week), "%V", &tm);
	snprintf(buf, sizeof(buf), _("%s \xc2\xb7 Week %d \xc2\xb7 Day %d of %d"), rel, atoi(week),
	         tm.tm_yday + 1, year_days);
	lv_label_set_text(cal.details, buf);

	lv_obj_set_hidden(cal.today_button, diff == 0);
}

static void selected_cb(long day, void *user_data)
{
	LV_UNUSED(user_data);
	cal.selected = day;
	info_update();
}

static void today_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	cal.selected = cal.today;
	ui_month_select(cal.month, cal.today);
	info_update();
}

/* After midnight, today is another day */
static void timer_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	long today = ui_today();
	if (today != cal.today) {
		cal.today = today;
		ui_month_set_today(cal.month, today);
		info_update();
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cal.timer)
		lv_timer_delete(cal.timer);
	memset(&cal, 0, sizeof(cal));
}

void calendar_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Calendar"), NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_scrollable(body, false);

	cal.today = cal.selected = ui_today();
	cal.month = ui_month_create(body, cal.today, selected_cb, NULL);

	/* The selected day */
	lv_obj_t *card = ui_card_create(body);
	lv_obj_t *row = ui_row_create(card, 120, false, NULL, NULL);
	lv_obj_t *text = ui_row_text(row);
	cal.date = lv_label_create(text);
	lv_obj_set_style_text_font(cal.date, UI_FONT_TITLE, 0);
	cal.details = ui_dim_label(text, "", UI_FONT_CAPTION);
	cal.today_button = ui_button(row, 136, 72, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Today"),
	                             today_cb, NULL);

	info_update();
	cal.timer = lv_timer_create(timer_cb, 60 * 1000, NULL);
	ui_push(scr);
}
