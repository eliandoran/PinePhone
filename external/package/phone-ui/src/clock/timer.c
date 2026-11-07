/*
 * Clock: a countdown timer of up to 99:59, set with -/+ buttons or presets. It runs on
 * when Clock is closed and while the phone sleeps (CLOCK_BOOTTIME; alarms.c wakes the
 * phone for it); when it runs out, an alert (alert.c) rings over whatever shows, as a
 * call: Stop, or +1 min to run it again for a minute. Not answered, a notification
 * stays. Not kept across a restart of phone-ui.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "clock.h"
#include "../ui_symbols.h"

#define TIMER_MAX (99 * 60 + 59) /* seconds */
#define APP "Clock"              /* NON-NLS: its name in ui_apps */
#define KEY "timer"              /* NON-NLS: its alarm's, its notification's */

enum state { IDLE, RUNNING, PAUSED };

/* The timer, kept while Clock is closed */
static struct {
	enum state state;
	int set;       /* seconds */
	uint64_t end;  /* CLOCK_BOOTTIME ms, when running */
	uint64_t left; /* ms, when paused */
} tmr = { IDLE, 5 * 60, 0, 0 };

/* Its page, while Clock is open */
static struct {
	struct digits digits;
	lv_obj_t *bar, *setup, *start;
	lv_timer_t *timer;
} cd;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t left(void)
{
	switch (tmr.state) {
	case RUNNING: {
		uint64_t now = now_ms();
		return tmr.end > now ? tmr.end - now : 0;
	}
	case PAUSED: return tmr.left;
	default: return (uint64_t)tmr.set * 1000;
	}
}

static void show(void)
{
	if (!cd.bar)
		return;
	char buf[8];
	/* Rounded up: it shows 00:01 during the last second */
	unsigned s = (unsigned)((left() + 999) / 1000);
	snprintf(buf, sizeof(buf), "%02u:%02u", s / 60 % 100, s % 60);
	digits_set(&cd.digits, buf);

	int32_t v = tmr.state == IDLE || !tmr.set ? 1000 : (int32_t)(left() / tmr.set);
	/* Not shorter than it is high: LVGL clips a shorter rounded indicator with a mask,
	 * which the GPU draw unit cannot draw */
	int32_t w = lv_obj_get_width(cd.bar), h = lv_obj_get_height(cd.bar);
	if (w > h && v * w < h * 1000)
		v = (h * 1000 + w - 1) / w;
	lv_bar_set_value(cd.bar, v, LV_ANIM_OFF);
}

static void buttons_update(void)
{
	if (!cd.bar)
		return;
	switch (tmr.state) {
	case RUNNING:
		clock_button_set(cd.start, _("Pause"), UI_COLOR_ORANGE, UI_COLOR_ORANGE_PRESSED);
		break;
	case PAUSED:
		clock_button_set(cd.start, _("Resume"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);
		break;
	default:
		clock_button_set(cd.start, _("Start"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);
		break;
	}
	lv_obj_set_hidden(cd.setup, tmr.state != IDLE);
	lv_obj_set_hidden(cd.bar, tmr.state == IDLE);
	if (tmr.state == RUNNING)
		lv_timer_resume(cd.timer);
	else
		lv_timer_pause(cd.timer);
}

static void refresh(void)
{
	buttons_update();
	show();
}

static void fired(const char *key, void *data);

/* Running for `ms`, the phone woken then */
static void run(uint64_t ms)
{
	tmr.end = now_ms() + ms;
	tmr.state = RUNNING;
	ui_alarm_in(APP, KEY, (uint32_t)ms, fired, NULL);
	ui_notify(APP, KEY, NULL, NULL, 0, 0, false, NULL); /* one missed earlier: seen */
}

static void notification_open(const char *key)
{
	LV_UNUSED(key);
	ui_notify(APP, KEY, NULL, NULL, 0, 0, false, NULL);
	clock_open_page(CLOCK_TIMER);
}

static void alert_done(enum ui_alert_end how, void *data)
{
	LV_UNUSED(data);
	if (how == UI_ALERT_OTHER) {
		run(60 * 1000);
	} else if (how == UI_ALERT_MISSED) {
		char when[8];
		time_t now = time(NULL);
		strftime(when, sizeof(when), "%H:%M", localtime(&now));
		ui_notify(APP, KEY, _("Timer"), _("Time's up"), 1, now, false, notification_open);
		printf("phone-ui: timer: not stopped, ran out at %s\n", when);
	}
	refresh();
}

/* Run out (alarms.c): ready again as set, and the alert rings */
static void fired(const char *key, void *data)
{
	LV_UNUSED(key);
	LV_UNUSED(data);
	tmr.state = IDLE;
	refresh();
	ui_alert(UI_SYMBOL_HOURGLASS_HALF, _("Timer"), _("Time's up"), "+1 min" /* NON-NLS */, alert_done, NULL);
}

static void tick(lv_timer_t *t)
{
	LV_UNUSED(t);
	show();
}

static void start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	switch (tmr.state) {
	case RUNNING:
		tmr.left = left();
		tmr.state = PAUSED;
		ui_alarm_cancel(APP, KEY);
		break;
	case PAUSED:
		run(tmr.left);
		break;
	default:
		if (!tmr.set)
			return;
		run((uint64_t)tmr.set * 1000);
		break;
	}
	refresh();
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	tmr.state = IDLE;
	ui_alarm_cancel(APP, KEY);
	refresh();
}

static void adjust_cb(lv_event_t *e)
{
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	int v = tmr.set + step;
	tmr.set = v < 0 ? 0 : v > TIMER_MAX ? TIMER_MAX : v;
	ui_vibrate(UI_VIBRATE_TAP);
	show();
}

static void preset_cb(lv_event_t *e)
{
	tmr.set = (int)(intptr_t)lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	show();
}

/* The -/+ buttons and the presets, while it is not running */
static void setup_create(lv_obj_t *page)
{
	static const struct { const char *text; int seconds; } presets[] = {
		{ "1 min", 60 }, { "3 min", 180 }, { "5 min", 300 }, { "10 min", 600 },
	};

	cd.setup = clock_setup_create(page);
	lv_obj_t *adjusters = clock_button_row(cd.setup);
	clock_adjuster_create(adjusters, _("Minutes"), adjust_cb, 60);
	clock_adjuster_create(adjusters, _("Seconds"), adjust_cb, 1);

	lv_obj_t *chips = clock_button_row(cd.setup);
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		ui_button(chips, 152, 80, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, presets[i].text,
		          preset_cb, (void *)(intptr_t)presets[i].seconds);
	}
}

void countdown_create(lv_obj_t *page)
{
	/* The bar comes and goes below the digits */
	lv_obj_t *status = clock_display_create(page, &cd.digits, "00:00");
	cd.bar = lv_bar_create(status);
	lv_obj_set_size(cd.bar, LV_PCT(70), 12);
	lv_obj_align(cd.bar, LV_ALIGN_TOP_MID, 0, 24);
	lv_obj_set_style_bg_color(cd.bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(cd.bar, lv_color_hex(UI_COLOR_ORANGE), LV_PART_INDICATOR);
	lv_bar_set_range(cd.bar, 0, 1000);

	setup_create(page);

	/* Pushes the buttons to the bottom */
	lv_obj_set_flex_grow(clock_spacer(page, 0), 1);

	lv_obj_t *row = clock_button_row(page);
	clock_round_button(row, _("Cancel"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, cancel_cb);
	cd.start = clock_round_button(row, _("Start"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, start_cb);

	/* As it was left: it may be running */
	cd.timer = lv_timer_create(tick, 100, NULL);
	refresh();
}

/* The page goes; the timer runs on */
void countdown_delete(void)
{
	lv_timer_delete(cd.timer);
	memset(&cd, 0, sizeof(cd));
}
