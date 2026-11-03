/*
 * Clock: local time with world clocks, a stopwatch and a countdown timer, on
 * pages switched by a segmented control.
 *
 * Running digits (stopwatch, timer) are one label per character, each set to a
 * static string: the GPU draw unit keeps a texture per digit instead of rendering
 * the whole text again on every frame, and the digits do not shift sideways as
 * they change (the font is not monospaced).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

#define COLOR_GREEN 0x30d158
#define COLOR_GREEN_PRESSED 0x6ee38c
#define COLOR_RED 0xff453a
#define COLOR_RED_PRESSED 0xff7d75
#define COLOR_ORANGE 0xff9f0a
#define COLOR_ORANGE_PRESSED 0xffc566

#define TIMER_MAX (99 * 60 + 59) /* seconds */

/* World clocks: POSIX TZ strings, there is no zoneinfo on the phone */
static const struct {
	const char *city, *tz;
} world[] = {
	{ "New York", "EST5EDT,M3.2.0,M11.1.0" },
	{ "London",   "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Tokyo",    "JST-9" },
	{ "Sydney",   "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
#define WORLD_COUNT (sizeof(world) / sizeof(world[0]))

/* Fixed-width digits: one label per character of a pattern like "00:00.00" */
struct digits {
	lv_obj_t *cells[8];
	unsigned count;
};

enum timer_state { TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_DONE };

static struct {
	lv_obj_t *tabs[3], *pages[3];

	/* Clock */
	lv_obj_t *time, *seconds, *date, *zone;
	lv_obj_t *world_time[WORLD_COUNT], *world_day[WORLD_COUNT];
	lv_timer_t *clock_timer;

	/* Stopwatch */
	struct digits sw;
	lv_obj_t *sw_start, *sw_start_label, *sw_lap, *sw_lap_label, *laps;
	lv_timer_t *sw_timer;
	bool sw_running;
	uint64_t sw_since;   /* when it was last started */
	uint64_t sw_elapsed; /* before that, ms */
	uint64_t lap_start;
	unsigned lap_count;

	/* Timer */
	struct digits tm;
	lv_obj_t *tm_bar, *tm_setup, *tm_done;
	lv_obj_t *tm_start, *tm_start_label, *tm_cancel;
	lv_timer_t *tm_timer, *buzz_timer;
	enum timer_state tm_state;
	int tm_set;          /* seconds */
	uint64_t tm_end;     /* when running */
	uint64_t tm_left;    /* ms, when paused */
	unsigned buzz_count;
} clk;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void digits_create(struct digits *d, lv_obj_t *parent, const char *pattern)
{
	static const char *const separators[] = { ":", "." };

	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_text_font(row, &lv_font_montserrat_48, 0);
	lv_obj_set_clickable(row, false);

	d->count = 0;
	for (const char *c = pattern; *c && d->count < 8; c++) {
		lv_obj_t *cell = lv_label_create(row);
		bool sep = *c == ':' || *c == '.';
		lv_obj_set_width(cell, sep ? 20 : 38);
		lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_text_static(cell, sep ? separators[*c == '.'] : "0");
		d->cells[d->count++] = cell;
	}
}

static void digits_set(struct digits *d, const char *text)
{
	static const char *const nums[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };

	for (unsigned i = 0; i < d->count && text[i]; i++) {
		if (text[i] < '0' || text[i] > '9')
			continue;
		const char *s = nums[text[i] - '0'];
		if (lv_label_get_text(d->cells[i]) != s)
			lv_label_set_text_static(d->cells[i], s);
	}
}

static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                              lv_obj_t **label, const char *text)
{
	lv_obj_t *b = lv_obj_create(parent);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(pressed), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(b, 0, 0);
	/* White on colors, the text color on grays */
	lv_obj_set_style_text_color(b, color == UI_COLOR_BUTTON || color == UI_COLOR_CARD ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_white(), 0);
	lv_obj_set_clickable(b, true);
	lv_obj_set_scrollable(b, false);

	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text_static(l, text);
	lv_obj_center(l);
	if (label)
		*label = l;
	return b;
}

static void button_style(lv_obj_t *b, lv_obj_t *label, const char *text, uint32_t color, uint32_t pressed)
{
	lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(pressed), LV_STATE_PRESSED);
	if (lv_label_get_text(label) != text)
		lv_label_set_text_static(label, text);
}

static lv_obj_t *page_create(lv_obj_t *parent)
{
	lv_obj_t *page = lv_obj_create(parent);
	lv_obj_remove_style_all(page);
	lv_obj_set_width(page, LV_PCT(100));
	lv_obj_set_flex_grow(page, 1);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(page, 20, 0);
	lv_obj_set_style_pad_bottom(page, 40, 0);
	lv_obj_set_scrollable(page, false);
	return page;
}

static lv_obj_t *spacer(lv_obj_t *parent, int32_t height)
{
	lv_obj_t *s = lv_obj_create(parent);
	lv_obj_remove_style_all(s);
	lv_obj_set_size(s, 1, height);
	lv_obj_set_clickable(s, false);
	return s;
}

/* Clock page */

/* Local time of `tz` without leaving it set */
static void tz_localtime(const char *tz, time_t t, struct tm *tm)
{
	const char *cur = getenv("TZ");
	char *saved = cur ? strdup(cur) : NULL;

	setenv("TZ", tz, 1);
	tzset();
	localtime_r(&t, tm);
	if (saved) {
		setenv("TZ", saved, 1);
		free(saved);
	} else {
		unsetenv("TZ");
	}
	tzset();
}

static void clock_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[64];
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);

	strftime(buf, sizeof(buf), "%H:%M", &local);
	ui_set_text(clk.time, buf);
	strftime(buf, sizeof(buf), "%S", &local);
	ui_set_text(clk.seconds, buf);
	strftime(buf, sizeof(buf), "%A %-d %B %Y", &local);
	ui_set_text(clk.date, buf);
	int off = (int)(local.tm_gmtoff / 60 % (24 * 60));
	int aoff = off < 0 ? -off : off;
	const char *abbr = local.tm_zone && local.tm_zone[0] ? local.tm_zone : "UTC";
	if (!off)
		ui_set_text_fmt(clk.zone, strcmp(abbr, "UTC") ? "%s, UTC" : "%s", abbr);
	else if (aoff % 60)
		ui_set_text_fmt(clk.zone, "%s, UTC%c%d:%02d", abbr, off < 0 ? '-' : '+', aoff / 60, aoff % 60);
	else
		ui_set_text_fmt(clk.zone, "%s, UTC%c%d", abbr, off < 0 ? '-' : '+', aoff / 60);

	for (size_t i = 0; i < WORLD_COUNT; i++) {
		struct tm tm;
		tz_localtime(world[i].tz, now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(clk.world_time[i], buf);

		int days = tm.tm_year != local.tm_year ? tm.tm_year - local.tm_year : tm.tm_yday - local.tm_yday;
		const char *day = days == 0 ? "Today" : days > 0 ? "Tomorrow" : "Yesterday";
		int diff = (int)(tm.tm_gmtoff - local.tm_gmtoff) / 60;
		int adiff = diff < 0 ? -diff : diff;
		if (!diff)
			ui_set_text_fmt(clk.world_day[i], "%s, same time", day);
		else if (adiff % 60)
			ui_set_text_fmt(clk.world_day[i], "%s, %c%d:%02d h", day, diff < 0 ? '-' : '+',
			                adiff / 60, adiff % 60);
		else
			ui_set_text_fmt(clk.world_day[i], "%s, %c%d h", day, diff < 0 ? '-' : '+', adiff / 60);
	}
}

static void clock_page(lv_obj_t *page)
{
	spacer(page, 40);

	lv_obj_t *row = lv_obj_create(page);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(row, 8, 0);
	clk.time = lv_label_create(row);
	ui_set_text(clk.time, "");
	lv_obj_set_style_text_font(clk.time, &lv_font_montserrat_48, 0);
	clk.seconds = lv_label_create(row);
	ui_set_text(clk.seconds, "");
	lv_obj_set_width(clk.seconds, 44);
	lv_obj_set_style_text_font(clk.seconds, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(clk.seconds, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_bottom(clk.seconds, 4, 0);

	clk.date = lv_label_create(page);
	ui_set_text(clk.date, "");
	lv_obj_set_style_text_font(clk.date, &lv_font_montserrat_26, 0);
	lv_obj_set_style_margin_top(clk.date, 12, 0);
	clk.zone = lv_label_create(page);
	ui_set_text(clk.zone, "");
	lv_obj_set_style_text_font(clk.zone, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(clk.zone, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(clk.zone, 8, 0);

	spacer(page, 48);
	lv_obj_t *title = lv_label_create(page);
	lv_label_set_text_static(title, "World clock");
	lv_obj_set_width(title, LV_PCT(100));
	lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_left(title, 16, 0);
	lv_obj_set_style_margin_bottom(title, 12, 0);

	lv_obj_t *card = lv_obj_create(page);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);

	for (size_t i = 0; i < WORLD_COUNT; i++) {
		lv_obj_t *r = lv_obj_create(card);
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), 112);
		lv_obj_set_style_pad_hor(r, 28, 0);
		lv_obj_set_scrollable(r, false);
		if (i) {
			lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
			lv_obj_set_style_border_width(r, 1, 0);
			lv_obj_set_style_border_color(r, lv_color_hex(UI_COLOR_LINE), 0);
		}

		lv_obj_t *city = lv_label_create(r);
		lv_label_set_text_static(city, world[i].city);
		lv_obj_align(city, LV_ALIGN_LEFT_MID, 0, -16);
		clk.world_day[i] = lv_label_create(r);
		ui_set_text(clk.world_day[i], "");
		lv_obj_set_style_text_font(clk.world_day[i], &lv_font_montserrat_20, 0);
		lv_obj_set_style_text_color(clk.world_day[i], lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_align(clk.world_day[i], LV_ALIGN_LEFT_MID, 0, 18);
		clk.world_time[i] = lv_label_create(r);
		ui_set_text(clk.world_time[i], "");
		lv_obj_set_style_text_font(clk.world_time[i], &lv_font_montserrat_32, 0);
		lv_obj_align(clk.world_time[i], LV_ALIGN_RIGHT_MID, 0, 0);
	}

	clk.clock_timer = lv_timer_create(clock_update, 1000, NULL);
	clock_update(NULL);
}

/* Stopwatch page */

static uint64_t sw_elapsed(void)
{
	return clk.sw_elapsed + (clk.sw_running ? now_ms() - clk.sw_since : 0);
}

static void format_ms(char *buf, size_t size, uint64_t ms)
{
	unsigned cs = (unsigned)(ms / 10);
	snprintf(buf, size, "%02u:%02u.%02u", cs / 6000 % 100, cs / 100 % 60, cs % 100);
}

static void sw_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[16];
	format_ms(buf, sizeof(buf), sw_elapsed());
	digits_set(&clk.sw, buf);
}

static void sw_buttons(void)
{
	if (clk.sw_running)
		button_style(clk.sw_start, clk.sw_start_label, "Stop", COLOR_RED, COLOR_RED_PRESSED);
	else
		button_style(clk.sw_start, clk.sw_start_label, "Start", COLOR_GREEN, COLOR_GREEN_PRESSED);
	const char *lap = clk.sw_running || !sw_elapsed() ? "Lap" : "Reset";
	if (lv_label_get_text(clk.sw_lap_label) != lap)
		lv_label_set_text_static(clk.sw_lap_label, lap);
	lv_obj_set_style_text_color(clk.sw_lap_label, clk.sw_running || sw_elapsed() ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_hex(UI_COLOR_DIM), 0);
}

static void sw_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	if (clk.sw_running) {
		clk.sw_elapsed = sw_elapsed();
		clk.sw_running = false;
		lv_timer_pause(clk.sw_timer);
		sw_update(NULL);
	} else {
		clk.sw_since = now_ms();
		clk.sw_running = true;
		lv_timer_resume(clk.sw_timer);
	}
	sw_buttons();
}

static void sw_lap_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	char buf[16];
	uint64_t total = sw_elapsed();

	if (!clk.sw_running) {
		if (!total)
			return;
		ui_vibrate(15);
		clk.sw_elapsed = 0;
		clk.lap_start = 0;
		clk.lap_count = 0;
		lv_obj_clean(clk.laps);
		sw_update(NULL);
		sw_buttons();
		return;
	}

	ui_vibrate(15);
	clk.lap_count++;
	lv_obj_t *row = lv_obj_create(clk.laps);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), 80);
	lv_obj_set_style_pad_hor(row, 28, 0);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);

	lv_obj_t *name = lv_label_create(row);
	ui_set_text_fmt(name, "Lap %u", clk.lap_count);
	lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
	/* Lap times stay: each distinct text is kept, as for other limited values */
	lv_obj_t *time = lv_label_create(row);
	format_ms(buf, sizeof(buf), total - clk.lap_start);
	ui_set_text(time, buf);
	lv_obj_align(time, LV_ALIGN_RIGHT_MID, 0, 0);

	clk.lap_start = total;
	lv_obj_move_to_index(row, 0);
	lv_obj_scroll_to_y(clk.laps, 0, LV_ANIM_ON);
}

static void stopwatch_page(lv_obj_t *page)
{
	spacer(page, 72);
	digits_create(&clk.sw, page, "00:00.00");
	spacer(page, 64);

	lv_obj_t *row = lv_obj_create(page);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	clk.sw_lap = round_button(row, 168, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, &clk.sw_lap_label, "Lap");
	lv_obj_add_event_cb(clk.sw_lap, sw_lap_cb, LV_EVENT_CLICKED, NULL);
	clk.sw_start = round_button(row, 168, COLOR_GREEN, COLOR_GREEN_PRESSED, &clk.sw_start_label, "Start");
	lv_obj_add_event_cb(clk.sw_start, sw_start_cb, LV_EVENT_CLICKED, NULL);

	clk.laps = lv_obj_create(page);
	lv_obj_remove_style_all(clk.laps);
	lv_obj_set_width(clk.laps, LV_PCT(100));
	lv_obj_set_flex_grow(clk.laps, 1);
	lv_obj_set_style_margin_top(clk.laps, 48, 0);
	lv_obj_set_flex_flow(clk.laps, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(clk.laps, LV_DIR_VER);

	/* Every 30 ms while running: about every other frame */
	clk.sw_timer = lv_timer_create(sw_update, 30, NULL);
	lv_timer_pause(clk.sw_timer);
	sw_buttons();
}

/* Timer page */

static uint64_t tm_left(void)
{
	switch (clk.tm_state) {
	case TIMER_RUNNING: {
		uint64_t now = now_ms();
		return clk.tm_end > now ? clk.tm_end - now : 0;
	}
	case TIMER_PAUSED: return clk.tm_left;
	case TIMER_DONE: return 0;
	default: return (uint64_t)clk.tm_set * 1000;
	}
}

static void tm_show(void)
{
	char buf[8];
	/* Rounded up: it shows 00:01 during the last second, 00:00 when done */
	unsigned s = (unsigned)((tm_left() + 999) / 1000);
	snprintf(buf, sizeof(buf), "%02u:%02u", s / 60 % 100, s % 60);
	digits_set(&clk.tm, buf);

	int32_t v = clk.tm_state == TIMER_IDLE || !clk.tm_set ? 1000 :
	            (int32_t)(tm_left() / clk.tm_set);
	lv_bar_set_value(clk.tm_bar, v, LV_ANIM_OFF);
}

static void tm_buttons(void)
{
	switch (clk.tm_state) {
	case TIMER_RUNNING:
		button_style(clk.tm_start, clk.tm_start_label, "Pause", COLOR_ORANGE, COLOR_ORANGE_PRESSED);
		break;
	case TIMER_PAUSED:
		button_style(clk.tm_start, clk.tm_start_label, "Resume", COLOR_GREEN, COLOR_GREEN_PRESSED);
		break;
	default:
		button_style(clk.tm_start, clk.tm_start_label, "Start", COLOR_GREEN, COLOR_GREEN_PRESSED);
		break;
	}
	lv_obj_set_hidden(clk.tm_setup, clk.tm_state != TIMER_IDLE);
	lv_obj_set_hidden(clk.tm_done, clk.tm_state != TIMER_DONE);
	lv_obj_set_hidden(clk.tm_bar, clk.tm_state == TIMER_IDLE);
}

static void buzz_stop(void)
{
	if (clk.buzz_timer) {
		lv_timer_delete(clk.buzz_timer);
		clk.buzz_timer = NULL;
	}
}

/* The alarm vibrates even with touch vibration off */
static void buzz(lv_timer_t *t)
{
	LV_UNUSED(t);
	bool haptics = ui_haptics;
	ui_haptics = true;
	ui_vibrate(400);
	ui_haptics = haptics;
	if (++clk.buzz_count >= 5)
		buzz_stop();
}

static void tm_tick(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (clk.tm_state == TIMER_RUNNING && !tm_left()) {
		clk.tm_state = TIMER_DONE;
		lv_timer_pause(clk.tm_timer);
		tm_buttons();
		buzz_stop();
		clk.buzz_count = 0;
		clk.buzz_timer = lv_timer_create(buzz, 800, NULL);
		buzz(NULL);
	}
	tm_show();
}

static void tm_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	buzz_stop();
	switch (clk.tm_state) {
	case TIMER_RUNNING:
		clk.tm_left = tm_left();
		clk.tm_state = TIMER_PAUSED;
		lv_timer_pause(clk.tm_timer);
		break;
	case TIMER_PAUSED:
		clk.tm_end = now_ms() + clk.tm_left;
		clk.tm_state = TIMER_RUNNING;
		lv_timer_resume(clk.tm_timer);
		break;
	default:
		if (!clk.tm_set)
			return;
		clk.tm_end = now_ms() + (uint64_t)clk.tm_set * 1000;
		clk.tm_state = TIMER_RUNNING;
		lv_timer_resume(clk.tm_timer);
		break;
	}
	tm_buttons();
	tm_show();
}

static void tm_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	buzz_stop();
	clk.tm_state = TIMER_IDLE;
	lv_timer_pause(clk.tm_timer);
	tm_buttons();
	tm_show();
}

static void tm_adjust_cb(lv_event_t *e)
{
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	int v = clk.tm_set + step;
	clk.tm_set = v < 0 ? 0 : v > TIMER_MAX ? TIMER_MAX : v;
	ui_vibrate(10);
	tm_show();
}

static void tm_preset_cb(lv_event_t *e)
{
	clk.tm_set = (int)(intptr_t)lv_event_get_user_data(e);
	ui_vibrate(15);
	tm_show();
}

/* "Minutes" or "Seconds" with a - and a + button */
static void tm_adjuster(lv_obj_t *parent, const char *title, int step)
{
	lv_obj_t *col = lv_obj_create(parent);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	lv_obj_set_scrollable(col, false);

	lv_obj_t *label = lv_label_create(col);
	lv_label_set_text_static(label, title);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *row = lv_obj_create(col);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_set_scrollable(row, false);

	for (int sign = -1; sign <= 1; sign += 2) {
		lv_obj_t *b = round_button(row, 96, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, &label,
		                           sign < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS);
		lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
		/* Holding it repeats */
		lv_obj_add_event_cb(b, tm_adjust_cb, LV_EVENT_PRESSED, (void *)(intptr_t)(sign * step));
		lv_obj_add_event_cb(b, tm_adjust_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(sign * step));
	}
}

static void timer_page(lv_obj_t *page)
{
	static const struct { const char *text; int seconds; } presets[] = {
		{ "1 min", 60 }, { "3 min", 180 }, { "5 min", 300 }, { "10 min", 600 },
	};

	spacer(page, 72);
	digits_create(&clk.tm, page, "00:00");

	/* Fixed height: the bar and the "Time's up" text come and go below the digits */
	lv_obj_t *status = lv_obj_create(page);
	lv_obj_remove_style_all(status);
	lv_obj_set_size(status, LV_PCT(100), 88);
	lv_obj_set_scrollable(status, false);
	clk.tm_bar = lv_bar_create(status);
	lv_obj_set_size(clk.tm_bar, LV_PCT(70), 12);
	lv_obj_align(clk.tm_bar, LV_ALIGN_TOP_MID, 0, 24);
	lv_obj_set_style_bg_color(clk.tm_bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(clk.tm_bar, lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
	lv_bar_set_range(clk.tm_bar, 0, 1000);
	clk.tm_done = lv_label_create(status);
	lv_label_set_text_static(clk.tm_done, LV_SYMBOL_BELL "  Time's up");
	lv_obj_set_style_text_font(clk.tm_done, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(clk.tm_done, lv_color_hex(COLOR_RED), 0);
	lv_obj_align(clk.tm_done, LV_ALIGN_BOTTOM_MID, 0, 0);

	clk.tm_setup = lv_obj_create(page);
	lv_obj_remove_style_all(clk.tm_setup);
	lv_obj_set_size(clk.tm_setup, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(clk.tm_setup, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clk.tm_setup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(clk.tm_setup, 40, 0);
	lv_obj_set_scrollable(clk.tm_setup, false);

	lv_obj_t *adjusters = lv_obj_create(clk.tm_setup);
	lv_obj_remove_style_all(adjusters);
	lv_obj_set_size(adjusters, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(adjusters, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(adjusters, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(adjusters, false);
	tm_adjuster(adjusters, "Minutes", 60);
	tm_adjuster(adjusters, "Seconds", 1);

	lv_obj_t *chips = lv_obj_create(clk.tm_setup);
	lv_obj_remove_style_all(chips);
	lv_obj_set_size(chips, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(chips, false);
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		lv_obj_t *chip = round_button(chips, 80, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, NULL, presets[i].text);
		lv_obj_set_width(chip, 152);
		lv_obj_add_event_cb(chip, tm_preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)presets[i].seconds);
	}

	/* Pushes the buttons to the bottom */
	lv_obj_set_flex_grow(spacer(page, 0), 1);

	lv_obj_t *row = lv_obj_create(page);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	clk.tm_cancel = round_button(row, 168, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, NULL, "Cancel");
	lv_obj_add_event_cb(clk.tm_cancel, tm_cancel_cb, LV_EVENT_CLICKED, NULL);
	clk.tm_start = round_button(row, 168, COLOR_GREEN, COLOR_GREEN_PRESSED, &clk.tm_start_label, "Start");
	lv_obj_add_event_cb(clk.tm_start, tm_start_cb, LV_EVENT_CLICKED, NULL);

	clk.tm_set = 5 * 60;
	clk.tm_state = TIMER_IDLE;
	clk.tm_timer = lv_timer_create(tm_tick, 100, NULL);
	lv_timer_pause(clk.tm_timer);
	tm_buttons();
	tm_show();
}

/* Segmented control */

static void tab_select(uintptr_t sel)
{
	for (uintptr_t i = 0; i < 3; i++) {
		lv_obj_set_state(clk.tabs[i], LV_STATE_CHECKED, i == sel);
		lv_obj_set_hidden(clk.pages[i], i != sel);
	}
}

static void tab_cb(lv_event_t *e)
{
	tab_select((uintptr_t)lv_event_get_user_data(e));
}

static void segments_create(lv_obj_t *parent)
{
	static const char *const names[] = { "Clock", "Stopwatch", "Timer" };

	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_pad_bottom(bar, 8, 0);
	lv_obj_set_scrollable(bar, false);

	lv_obj_t *seg = lv_obj_create(bar);
	lv_obj_remove_style_all(seg);
	lv_obj_set_size(seg, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(seg, 6, 0);
	lv_obj_set_style_pad_column(seg, 6, 0);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollable(seg, false);

	for (uintptr_t i = 0; i < 3; i++) {
		lv_obj_t *tab = lv_obj_create(seg);
		lv_obj_remove_style_all(tab);
		lv_obj_set_height(tab, 80);
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_white(), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_set_scrollable(tab, false);
		lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, (void *)i);

		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, names[i]);
		lv_obj_center(label);
		clk.tabs[i] = tab;
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	buzz_stop();
	lv_timer_delete(clk.clock_timer);
	lv_timer_delete(clk.sw_timer);
	lv_timer_delete(clk.tm_timer);
	memset(&clk, 0, sizeof(clk));
}

void clock_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Clock", NULL, &body);
	lv_obj_set_scrollable(body, false);

	memset(&clk, 0, sizeof(clk));
	segments_create(body);
	for (int i = 0; i < 3; i++)
		clk.pages[i] = page_create(body);
	clock_page(clk.pages[0]);
	stopwatch_page(clk.pages[1]);
	timer_page(clk.pages[2]);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

	tab_select(0);
	ui_push(scr);
}
