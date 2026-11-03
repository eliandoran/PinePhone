/*
 * Power menu: holding Power opens a card with Power off and Restart, as on Android.
 * A tap beside it or another Power press closes it. Power with Volume Down is a
 * screenshot instead (volume.c), which cancels the hold.
 *
 * The choice runs BusyBox's poweroff or reboot: init runs the shutdown scripts
 * (rcK, which stops phone-ui) and unmounts the file systems. The card says what is
 * happening until phone-ui is stopped.
 *
 * It goes on the top layer, over the screens and the quick settings, which a
 * backdrop darkens.
 *
 * A short press puts the phone to sleep (s2idle: deep sleep resets the phone,
 * maybe for its dead battery) and the next one wakes it up: the kernel turns the
 * display off and on again with what it showed. No lock screen yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

#define HOLD_MS 800
#define RUN_DELAY_MS 100 /* for the card to say what is happening first */
#define WAKE_IGNORE_MS 1000 /* the press that woke the phone, if it gets through */

static struct {
	lv_timer_t *hold;
	lv_obj_t *scrim, *card;
	const char *command; /* chosen, about to run */
	bool tap;            /* a press that is not a hold yet, nor a screenshot */
	uint32_t woke;       /* lv_tick_get() at the last resume, 0: none */
} pw;

static void menu_close(void)
{
	if (!pw.scrim || pw.command)
		return;
	/* Asynchronously: closed from its own events */
	lv_obj_delete_async(pw.scrim);
	pw.scrim = pw.card = NULL;
}

static void run_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	printf("phone-ui: %s\n", pw.command);
	if (system(pw.command) != 0) {
		printf("phone-ui: %s failed\n", pw.command);
		pw.command = NULL;
		menu_close();
	}
}

static void choose(const char *command, const char *text)
{
	pw.command = command;
	lv_obj_clean(pw.card);
	lv_obj_t *row = ui_row_create(pw.card, 160, false, NULL, NULL);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_timer_set_repeat_count(lv_timer_create(run_cb, RUN_DELAY_MS, NULL), 1);
}

static void poweroff_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	choose("poweroff", "Powering off...");
}

static void reboot_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	choose("reboot", "Restarting...");
}

static void scrim_cb(lv_event_t *e)
{
	/* Not taps on the card */
	if (lv_event_get_target(e) == lv_event_get_current_target(e))
		menu_close();
}

static void option(const char *symbol, uint32_t color, const char *text, bool divider, lv_event_cb_t cb)
{
	lv_obj_t *row = ui_row_create(pw.card, 128, divider, cb, NULL);
	lv_obj_t *icon = lv_label_create(row);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_width(icon, 48);
	lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, &ui_font_26, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
}

/* Made for each opening: it takes the theme's colors of the moment */
static void menu_open(void)
{
	pw.scrim = ui_box(lv_layer_top());
	lv_obj_set_size(pw.scrim, LV_PCT(100), LV_PCT(100));
	/* Darkens the whole screen. Square: semi-transparent fills without radius are
	 * one colored quad (LVGL patch 0005), not a texture of the screen */
	lv_obj_set_style_bg_color(pw.scrim, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pw.scrim, LV_OPA_50, 0);
	lv_obj_set_clickable(pw.scrim, true);
	lv_obj_add_event_cb(pw.scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

	pw.card = ui_card_create(pw.scrim);
	lv_obj_set_width(pw.card, 560);
	lv_obj_center(pw.card);
	option(LV_SYMBOL_POWER, UI_COLOR_RED, "Power off", true, poweroff_cb);
	option(LV_SYMBOL_REFRESH, UI_COLOR_TEXT, "Restart", false, reboot_cb);
	ui_vibrate(40);
}

static void hold_cb(lv_timer_t *timer)
{
	lv_timer_pause(timer);
	pw.tap = false;
	if (!pw.scrim)
		menu_open();
}

/* What woke the phone: the interrupt the kernel recorded (CONFIG_PM_DEBUG), by its
 * name in /proc/interrupts (the last word of its line), e.g. axp803 for Power */
static void woken_by(char *name, size_t size)
{
	snprintf(name, size, "?");
	unsigned irq;
	FILE *f = fopen("/sys/power/pm_wakeup_irq", "r");
	bool found = f && fscanf(f, "%u", &irq) == 1;
	if (f)
		fclose(f);
	if (!found || !(f = fopen("/proc/interrupts", "r")))
		return;
	snprintf(name, size, "IRQ %u", irq);
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *end;
		if (strtoul(line, &end, 10) != irq || *end != ':')
			continue;
		line[strcspn(line, "\n")] = '\0';
		char *last = strrchr(line, ' ');
		if (last && last[1])
			snprintf(name, size, "%s (IRQ %u)", last + 1, irq);
		break;
	}
	fclose(f);
}

static uint64_t boottime_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

/* Blocks until the phone wakes up */
static void sleep_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	printf("phone-ui: sleeping\n");
	uint64_t start = boottime_ms();
	FILE *f = fopen("/sys/power/state", "w");
	bool ok = f && fputs("freeze", f) >= 0;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok) {
		printf("phone-ui: cannot sleep\n");
		return;
	}
	pw.woke = lv_tick_get() | 1;

	char by[64];
	woken_by(by, sizeof(by));
	printf("phone-ui: awake after %.1f s, woken by %s\n", (boottime_ms() - start) / 1000.0, by);
	ui_status_update(); /* the clock */
}

void ui_power_key(bool pressed)
{
	if (!pw.hold) {
		pw.hold = lv_timer_create(hold_cb, HOLD_MS, NULL);
		lv_timer_pause(pw.hold);
	}
	if (pw.woke) {
		/* The press that woke the phone: ignored to its release */
		bool recent = lv_tick_elaps(pw.woke) < WAKE_IGNORE_MS;
		if (!pressed || !recent)
			pw.woke = 0;
		if (recent)
			return;
	}
	if (pressed && pw.scrim) {
		menu_close();
	} else if (pressed) {
		pw.tap = true;
		lv_timer_reset(pw.hold);
		lv_timer_resume(pw.hold);
	} else {
		lv_timer_pause(pw.hold);
		/* After this frame: the release reaches the rest of the UI first */
		if (pw.tap && !pw.scrim)
			lv_timer_set_repeat_count(lv_timer_create(sleep_cb, 0, NULL), 1);
		pw.tap = false;
	}
}

void ui_power_cancel(void)
{
	if (pw.hold)
		lv_timer_pause(pw.hold);
	pw.tap = false;
}
