/*
 * Power menu: holding Power (or the shade's button) opens a card with Power off and
 * Restart, as on Android. A tap beside it or another Power press closes it. Power with
 * Volume Down is a screenshot instead (volume.c), which cancels the hold.
 *
 * The choice runs BusyBox's poweroff or reboot: init runs the shutdown scripts
 * (rcK, which stops phone-ui) and unmounts the file systems. The card says what is
 * happening until phone-ui is stopped.
 *
 * It goes on the top layer, over the screens and the quick settings, which a
 * backdrop darkens.
 *
 * A short press puts the phone to sleep ("mem": the kind the board's kernel chose in
 * /sys/power/mem_sleep, deep sleep where it has one, through Crust on the PinePhone,
 * s2idle otherwise) and the next one wakes it up: the kernel turns the
 * display off and on again with what it showed: the lock screen (lock.c), put up
 * before sleeping. The backlight fades out and goes off first, so that the lock
 * screen is not seen being put up. Its brightness goes to 0 while it is off: the
 * kernel turns it on again when it resumes the panel, before phone-ui runs, and it
 * would show the lock screen there, then what woke the phone (a call's screen) over it.
 * phone-ui sets the brightness back once it drew what it shows on waking: the calls
 * asked for (modem_woke()) known or one ringing, nothing left to draw, and that frame
 * on the display (at most DARK_MAX_MS after waking).
 *
 * Presses that start within a second of waking are ignored, with their release:
 * the one that woke the phone, and the ones made while the kernel was suspending,
 * which did not wake it and arrive with it. A press that stopped the suspend (the
 * write fails) arrives the same way.
 *
 * Woken by something else than Power (a message, a call, an alarm of alarms.c through the
 * RTC: no press arrives), the phone sleeps again once left on the lock screen AGAIN_MS
 * without a touch or a key: not while a call is on or an alert rings (alert.c: their
 * screens hide the lock screen), nor once unlocked. Woken by Power, it stays awake until
 * the next press. A press of Power while an alert rings stops it, and does not sleep.
 * Nor while the system is being installed on the eMMC (emmc_install.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i18n.h"
#include "log.h"
#include "modem/modem.h"
#include "apps/settings/settings.h"
#include "ui.h"
#include "src/display/lv_display_private.h" /* what is left to draw (inv_p) */

#define HOLD_MS 800
#define RUN_DELAY_MS 100 /* for the card to say what is happening first */
#define WAKE_IGNORE_MS 1000 /* presses that start this soon after waking */
#define LOCK_SHOW_MS 50 /* for the lock screen's frame to reach the display before sleeping */
#define FADE_MS 120 /* the backlight down before sleeping */
/* Wi-Fi up again this long after waking: bringing wlan0 up blocks a few hundred ms (the
 * chip's firmware), which would hold back what woke the phone (a call's screen). Longer
 * than WAKE_IGNORE_MS, so that the phone can sleep again before it */
#define WIFI_UP_MS 2000
#define DARK_MAX_MS 500 /* dark at most this long after waking (a modem not answering) */
#define EXTRA_FRAMES 2 /* drawn after it, for what waking shows to be on the panel whole */
#define AGAIN_MS 10000 /* asleep again, woken by something else than Power and left locked */
#define AGAIN_CHECK_MS 500

static struct {
	lv_timer_t *hold;
	lv_obj_t *scrim, *card;
	const char *command; /* chosen, about to run */
	bool tap;            /* a press that is not a hold yet, nor a screenshot */
	bool sleeping;       /* fading out, then asleep */
	int brightness;      /* to set back once lit, -1: no backlight */
	lv_timer_t *light;   /* the screen lit again once drawn, after waking */
	int extra;           /* frames drawn after what waking shows, for it to reach the display */
	uint32_t light_from; /* lv_tick_get() on waking */
	uint32_t woke;       /* lv_tick_get() at the last resume, 0: none */
	bool ignored;        /* pressed soon after waking: ignored to its release */
	lv_timer_t *wifi_up; /* Wi-Fi up again, WIFI_UP_MS after waking */
	lv_timer_t *again;   /* woken without Power: asleep again once left locked */
} pw;

static void sleep_start(void);

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
	choose("poweroff", _("Powering off..."));
}

static void reboot_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	choose("reboot", _("Restarting..."));
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
	lv_obj_set_style_text_font(label, UI_FONT_TITLE, 0);
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
	option(LV_SYMBOL_POWER, UI_COLOR_RED, _("Power off"), true, poweroff_cb);
	option(LV_SYMBOL_REFRESH, UI_COLOR_TEXT, _("Restart"), false, reboot_cb);
	ui_vibrate(UI_VIBRATE_TAP);
}

static void hold_cb(lv_timer_t *timer)
{
	lv_timer_pause(timer);
	pw.tap = false;
	ui_power_menu();
}

void ui_power_menu(void)
{
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

static void brightness_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	ui_brightness_set(v);
}

static void fade(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &pw.brightness);
	lv_anim_set_exec_cb(&a, brightness_exec_cb);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, FADE_MS);
	lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

/* Blocks until the phone wakes up */
static void wifi_up_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	pw.wifi_up = NULL; /* deleted after this, running once */
	double start = log_time();
	wifi_sleep(false);
	printf("phone-ui: [%.3f] wlan0 up in %.0f ms\n", start, (log_time() - start) * 1000);
}

static void lit(void)
{
	if (pw.light)
		lv_timer_delete(pw.light);
	pw.light = NULL;
	if (pw.brightness >= 0)
		ui_brightness_set(pw.brightness);
}

/*
 * Each frame after waking until what it shows is drawn. A frame drawn is only queued
 * for the display: its page flip comes at a vblank once the GPU finished it, and the
 * panel then receives it line by line over the next refresh (lit at the flip, the lower
 * part of the screen still showed the lock screen). So EXTRA_FRAMES more frames are
 * drawn (a pixel each): the display's flush waits for the previous flip before queueing
 * its own, so once the second is drawn, the first was flipped a refresh after the frame
 * that matters, which the panel then showed whole.
 */
static void light_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	lv_display_t *disp = lv_display_get_default();
	bool waited = lv_tick_elaps(pw.light_from) >= DARK_MAX_MS;
	if (!waited && disp && disp->inv_p)
		return; /* still to draw */
	if (pw.extra == EXTRA_FRAMES || waited) {
		printf("phone-ui: [%.3f] screen lit\n", log_time());
		lit();
		return;
	}
	/* The calls known: asked for, or one already ringing (its RING heard) */
	if (!pw.extra && modem_woke_pending() && modem_call_state() == MODEM_CALL_NONE)
		return;
	pw.extra++;
	lv_area_t pixel = { 0, 0, 0, 0 };
	lv_obj_invalidate_area(lv_screen_active(), &pixel);
}

static void again_stop(void)
{
	if (pw.again)
		lv_timer_delete(pw.again);
	pw.again = NULL;
}

/* Inactive: since the last touch or key (LVGL's), the lock screen shown and no call */
static void again_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (!ui_locked() || modem_call_state() != MODEM_CALL_NONE || ui_alert_ringing() || pw.scrim || pw.sleeping) {
		lv_display_trigger_activity(NULL); /* counted from when it is back */
		return;
	}
	if (lv_display_get_inactive_time(NULL) < AGAIN_MS)
		return;
	printf("phone-ui: left %d s on the lock screen: asleep again\n", AGAIN_MS / 1000);
	again_stop();
	sleep_start();
}

static void sleep_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	printf("phone-ui: [%.3f] sleeping\n", log_time());
	again_stop();
	if (pw.wifi_up) { /* asleep again before it came up: it stays down */
		lv_timer_delete(pw.wifi_up);
		pw.wifi_up = NULL;
	}
	if (pw.light) { /* asleep again before it was lit: dark still */
		lv_timer_delete(pw.light);
		pw.light = NULL;
	}
	wifi_sleep(true);
	uint64_t start = boottime_ms();
	FILE *f = fopen("/sys/power/state", "w");
	bool ok = f && fputs("mem", f) >= 0;
	if (f && fclose(f) != 0)
		ok = false;
	double back = log_time(); /* the kernel resumed */
	uint64_t slept = boottime_ms() - start;
	if (ok)
		modem_woke(); /* a call ringing is asked for */
	/* Before the first frame, which the screen is lit for (light_cb()): the clocks still
	 * say when it went to sleep, the lock screen's and the home screen's under it */
	ui_clocks_update();
	pw.wifi_up = lv_timer_create(wifi_up_cb, WIFI_UP_MS, NULL);
	lv_timer_set_repeat_count(pw.wifi_up, 1);
	pw.sleeping = false;
	ui_backlight_on(true); /* the kernel did, unless the sleep failed: at brightness 0 */
	pw.woke = lv_tick_get() | 1;
	if (!ok) {
		printf("phone-ui: cannot sleep\n");
		lit();
		return;
	}
	pw.extra = 0;
	pw.light_from = lv_tick_get();
	pw.light = lv_timer_create(light_cb, LV_DEF_REFR_PERIOD, NULL);
	/* Until a press of Power says the user woke it */
	lv_display_trigger_activity(NULL);
	pw.again = lv_timer_create(again_cb, AGAIN_CHECK_MS, NULL);

	char by[64];
	woken_by(by, sizeof(by));
	printf("phone-ui: [%.3f] awake after %.1f s, woken by %s\n", back, slept / 1000.0, by);
}

/* Faded out: the lock screen is drawn in the dark, then the phone sleeps once it
 * reached the display, and wakes up with it */
static void dark_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	ui_backlight_on(false);
	if (pw.brightness >= 0)
		backlight_dark(); /* until phone-ui drew what it shows on waking */
	ui_lock();
	call_screen_prepare(); /* in the dark, rather than when a call wakes the phone */
	lv_timer_set_repeat_count(lv_timer_create(sleep_cb, LOCK_SHOW_MS, NULL), 1);
}

static void sleep_start(void)
{
	pw.sleeping = true;
	pw.brightness = ui_brightness_get();
	if (pw.brightness >= 0)
		fade(pw.brightness, UI_BRIGHTNESS_MIN, dark_cb);
	else
		dark_cb(NULL);
}

void ui_power_key(bool pressed)
{
	if (!pw.hold) {
		pw.hold = lv_timer_create(hold_cb, HOLD_MS, NULL);
		lv_timer_pause(pw.hold);
	}
	if (pw.sleeping)
		return;
	if (pw.woke && lv_tick_elaps(pw.woke) >= WAKE_IGNORE_MS)
		pw.woke = 0;
	if (pressed)
		again_stop(); /* the user's: awake until the next press */
	if (pressed && pw.woke)
		pw.ignored = true;
	if (pw.ignored) {
		if (!pressed)
			pw.ignored = false;
		return;
	}
	/* An alert ringing: Power stops it, and the phone stays awake */
	if (pressed && ui_alert_ringing()) {
		ui_alert_stop();
		pw.ignored = true; /* its release */
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
		/* From the next frame: the release reaches the rest of the UI first */
		if (pw.tap && !pw.scrim && !emmc_install_running())
			sleep_start();
		pw.tap = false;
	}
}

void ui_power_cancel(void)
{
	if (pw.hold)
		lv_timer_pause(pw.hold);
	pw.tap = false;
}
