/*
 * Clock: a stopwatch with laps, newest first. It runs on when Clock is closed and while the
 * phone sleeps (CLOCK_BOOTTIME); not kept across a restart of phone-ui.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"

/* The stopwatch, kept while Clock is closed */
static struct {
	bool running;
	uint64_t since;   /* when it was last started, CLOCK_BOOTTIME ms */
	uint64_t elapsed; /* before that, ms */
	uint64_t lap_start;
	uint64_t *laps;   /* each lap's time, ms, the first first */
	unsigned lap_count;
} sw;

/* Its page, while Clock is open */
static struct {
	struct digits digits;
	lv_obj_t *start, *lap, *laps;
	lv_timer_t *timer;
} pg;

static uint64_t elapsed(void)
{
	return sw.elapsed + (sw.running ? clock_now_ms() - sw.since : 0);
}

static void format_ms(char *buf, size_t size, uint64_t ms)
{
	unsigned cs = (unsigned)(ms / 10);
	snprintf(buf, size, "%02u:%02u.%02u", cs / 6000 % 100, cs / 100 % 60, cs % 100);
}

static void update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[16];
	format_ms(buf, sizeof(buf), elapsed());
	digits_set(&pg.digits, buf);
}

static void buttons_update(void)
{
	if (sw.running)
		clock_button_set(pg.start, _("Stop"), UI_COLOR_RED, UI_COLOR_RED_PRESSED);
	else
		clock_button_set(pg.start, _("Start"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);

	lv_obj_t *label = lv_obj_get_child(pg.lap, 0);
	const char *lap = sw.running || !elapsed() ? _("Lap") : _("Reset");
	if (lv_label_get_text(label) != lap)
		lv_label_set_text_static(label, lap);
	lv_obj_set_style_text_color(label, sw.running || elapsed() ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_hex(UI_COLOR_DIM), 0);
}

static void start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	if (sw.running) {
		sw.elapsed = elapsed();
		sw.running = false;
		lv_timer_pause(pg.timer);
		update(NULL);
	} else {
		sw.since = clock_now_ms();
		sw.running = true;
		lv_timer_resume(pg.timer);
	}
	buttons_update();
}

/* Lap i's row, at the top */
static void lap_row(unsigned i)
{
	char buf[16];
	lv_obj_t *row = ui_row_create(pg.laps, 80, true, NULL, NULL);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *name = lv_label_create(row);
	ui_set_text_fmt(name, _("Lap %u"), i + 1);
	lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_DIM), 0);
	/* Lap times stay: each distinct text is kept, as for other limited values */
	lv_obj_t *time = lv_label_create(row);
	format_ms(buf, sizeof(buf), sw.laps[i]);
	ui_set_text(time, buf);
	lv_obj_move_to_index(row, 0);
}

static void lap_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	uint64_t total = elapsed();

	if (!sw.running) {
		if (!total)
			return;
		ui_vibrate(UI_VIBRATE_TAP);
		sw.elapsed = 0;
		sw.lap_start = 0;
		sw.lap_count = 0;
		lv_obj_clean(pg.laps);
		update(NULL);
		buttons_update();
		return;
	}

	ui_vibrate(UI_VIBRATE_TAP);
	sw.laps = realloc(sw.laps, (sw.lap_count + 1) * sizeof(*sw.laps));
	sw.laps[sw.lap_count++] = total - sw.lap_start;
	sw.lap_start = total;
	lap_row(sw.lap_count - 1);
	lv_obj_scroll_to_y(pg.laps, 0, LV_ANIM_ON);
}

void stopwatch_create(lv_obj_t *page)
{
	clock_spacer(page, 72);
	digits_create(&pg.digits, page, "00:00.00");
	clock_spacer(page, 64);

	lv_obj_t *row = clock_button_row(page);
	pg.lap = clock_round_button(row, _("Lap"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, lap_cb);
	pg.start = clock_round_button(row, _("Start"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, start_cb);

	pg.laps = lv_obj_create(page);
	lv_obj_remove_style_all(pg.laps);
	lv_obj_set_width(pg.laps, LV_PCT(100));
	lv_obj_set_flex_grow(pg.laps, 1);
	lv_obj_set_style_margin_top(pg.laps, 48, 0);
	lv_obj_set_flex_flow(pg.laps, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(pg.laps, LV_DIR_VER);

	/* As it was left: its laps, it may be running */
	for (unsigned i = 0; i < sw.lap_count; i++)
		lap_row(i);
	/* Every 30 ms while running: about every other frame */
	pg.timer = lv_timer_create(update, 30, NULL);
	if (!sw.running)
		lv_timer_pause(pg.timer);
	update(NULL);
	buttons_update();
}

/* The page goes; the stopwatch runs on */
void stopwatch_delete(void)
{
	lv_timer_delete(pg.timer);
	memset(&pg, 0, sizeof(pg));
}
