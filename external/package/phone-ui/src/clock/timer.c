/*
 * Clock: a countdown timer of up to 99:59, set with -/+ buttons or presets. When
 * it runs out, it vibrates a few times.
 */

#include <stdio.h>
#include <string.h>

#include "clock.h"

#define TIMER_MAX (99 * 60 + 59) /* seconds */

enum state { IDLE, RUNNING, PAUSED, DONE };

static struct {
	struct digits digits;
	lv_obj_t *bar, *setup, *done, *start;
	lv_timer_t *timer, *buzz_timer;
	enum state state;
	int set;           /* seconds */
	uint64_t end;      /* when running */
	uint64_t left;     /* ms, when paused */
	unsigned buzz_count;
} cd;

static uint64_t left(void)
{
	switch (cd.state) {
	case RUNNING: {
		uint64_t now = clock_now_ms();
		return cd.end > now ? cd.end - now : 0;
	}
	case PAUSED: return cd.left;
	case DONE: return 0;
	default: return (uint64_t)cd.set * 1000;
	}
}

static void show(void)
{
	char buf[8];
	/* Rounded up: it shows 00:01 during the last second, 00:00 when done */
	unsigned s = (unsigned)((left() + 999) / 1000);
	snprintf(buf, sizeof(buf), "%02u:%02u", s / 60 % 100, s % 60);
	digits_set(&cd.digits, buf);

	int32_t v = cd.state == IDLE || !cd.set ? 1000 : (int32_t)(left() / cd.set);
	/* Not shorter than it is high: LVGL clips a shorter rounded indicator with a mask,
	 * which the GPU draw unit cannot draw */
	int32_t w = lv_obj_get_width(cd.bar), h = lv_obj_get_height(cd.bar);
	if (w > h && v * w < h * 1000)
		v = (h * 1000 + w - 1) / w;
	lv_bar_set_value(cd.bar, v, LV_ANIM_OFF);
}

static void buttons_update(void)
{
	switch (cd.state) {
	case RUNNING:
		clock_button_set(cd.start, "Pause", UI_COLOR_ORANGE, UI_COLOR_ORANGE_PRESSED);
		break;
	case PAUSED:
		clock_button_set(cd.start, "Resume", UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);
		break;
	default:
		clock_button_set(cd.start, "Start", UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);
		break;
	}
	lv_obj_set_hidden(cd.setup, cd.state != IDLE);
	lv_obj_set_hidden(cd.done, cd.state != DONE);
	lv_obj_set_hidden(cd.bar, cd.state == IDLE || cd.state == DONE);
}

static void buzz_stop(void)
{
	if (cd.buzz_timer) {
		lv_timer_delete(cd.buzz_timer);
		cd.buzz_timer = NULL;
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
	if (++cd.buzz_count >= 5)
		buzz_stop();
}

static void tick(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (cd.state == RUNNING && !left()) {
		cd.state = DONE;
		lv_timer_pause(cd.timer);
		buttons_update();
		buzz_stop();
		cd.buzz_count = 0;
		cd.buzz_timer = lv_timer_create(buzz, 800, NULL);
		buzz(NULL);
	}
	show();
}

static void start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	buzz_stop();
	switch (cd.state) {
	case RUNNING:
		cd.left = left();
		cd.state = PAUSED;
		lv_timer_pause(cd.timer);
		break;
	case PAUSED:
		cd.end = clock_now_ms() + cd.left;
		cd.state = RUNNING;
		lv_timer_resume(cd.timer);
		break;
	default:
		if (!cd.set)
			return;
		cd.end = clock_now_ms() + (uint64_t)cd.set * 1000;
		cd.state = RUNNING;
		lv_timer_resume(cd.timer);
		break;
	}
	buttons_update();
	show();
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	buzz_stop();
	cd.state = IDLE;
	lv_timer_pause(cd.timer);
	buttons_update();
	show();
}

static void adjust_cb(lv_event_t *e)
{
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	int v = cd.set + step;
	cd.set = v < 0 ? 0 : v > TIMER_MAX ? TIMER_MAX : v;
	ui_vibrate(UI_VIBRATE_TAP);
	show();
}

static void preset_cb(lv_event_t *e)
{
	cd.set = (int)(intptr_t)lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	show();
}

/* "Minutes" or "Seconds" with a - and a + button */
static void adjuster_create(lv_obj_t *parent, const char *title, int step)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	ui_dim_label(col, title, &ui_font_20);

	lv_obj_t *row = ui_box(col);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 20, 0);

	for (int sign = -1; sign <= 1; sign += 2) {
		lv_obj_t *b = ui_button(row, 96, 96, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
		                        sign < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS, NULL, NULL);
		lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &ui_font_32, 0);
		/* Holding it repeats */
		lv_obj_add_event_cb(b, adjust_cb, LV_EVENT_PRESSED, (void *)(intptr_t)(sign * step));
		lv_obj_add_event_cb(b, adjust_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(sign * step));
	}
}

/* The -/+ buttons and the presets, while it is not running */
static void setup_create(lv_obj_t *page)
{
	static const struct { const char *text; int seconds; } presets[] = {
		{ "1 min", 60 }, { "3 min", 180 }, { "5 min", 300 }, { "10 min", 600 },
	};

	cd.setup = ui_box(page);
	lv_obj_set_size(cd.setup, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cd.setup, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cd.setup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(cd.setup, 40, 0);

	lv_obj_t *adjusters = clock_button_row(cd.setup);
	adjuster_create(adjusters, "Minutes", 60);
	adjuster_create(adjusters, "Seconds", 1);

	lv_obj_t *chips = clock_button_row(cd.setup);
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		ui_button(chips, 152, 80, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, presets[i].text,
		          preset_cb, (void *)(intptr_t)presets[i].seconds);
	}
}

void countdown_create(lv_obj_t *page)
{
	clock_spacer(page, 72);
	digits_create(&cd.digits, page, "00:00");

	/* Fixed height: the bar and the "Time's up" text come and go below the digits */
	lv_obj_t *status = ui_box(page);
	lv_obj_set_size(status, LV_PCT(100), 88);
	cd.bar = lv_bar_create(status);
	lv_obj_set_size(cd.bar, LV_PCT(70), 12);
	lv_obj_align(cd.bar, LV_ALIGN_TOP_MID, 0, 24);
	lv_obj_set_style_bg_color(cd.bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(cd.bar, lv_color_hex(UI_COLOR_ORANGE), LV_PART_INDICATOR);
	lv_bar_set_range(cd.bar, 0, 1000);
	cd.done = lv_label_create(status);
	lv_label_set_text_static(cd.done, LV_SYMBOL_BELL "  Time's up");
	lv_obj_set_style_text_font(cd.done, &ui_font_32, 0);
	lv_obj_set_style_text_color(cd.done, lv_color_hex(UI_COLOR_RED), 0);
	lv_obj_align(cd.done, LV_ALIGN_BOTTOM_MID, 0, 0);

	setup_create(page);

	/* Pushes the buttons to the bottom */
	lv_obj_set_flex_grow(clock_spacer(page, 0), 1);

	lv_obj_t *row = clock_button_row(page);
	ui_button(row, 168, 168, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, "Cancel", cancel_cb, NULL);
	cd.start = ui_button(row, 168, 168, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, "Start", start_cb, NULL);

	cd.set = 5 * 60;
	cd.state = IDLE;
	cd.timer = lv_timer_create(tick, 100, NULL);
	lv_timer_pause(cd.timer);
	buttons_update();
	show();
}

void countdown_delete(void)
{
	buzz_stop();
	lv_timer_delete(cd.timer);
	memset(&cd, 0, sizeof(cd));
}
