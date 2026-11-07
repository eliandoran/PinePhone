/*
 * Alerts: what must be seen at its time (Clock's timer; its alarm to come) on a screen
 * of its own over everything, the lock screen too, as a call coming in: a symbol, the
 * app's name, a large text, a red Stop and another choice ("+1 min", "Snooze"). The
 * vibrator rings as for a call, even with touch vibration off, and the phone stays awake
 * (power.c) until Stop or the other choice is tapped, Power is pressed (Stop), or RING_MS
 * passes (missed). One at a time: a new one ends the one showing, as missed. Woken for it
 * (alarms.c, the RTC), the phone shows it as it lights.
 */

#include <string.h>

#include "i18n.h"
#include "ui.h"

#define DIM LV_OPA_80          /* the wallpaper, as the call screen's */
#define VIBRATE_EVERY_MS 1500  /* as a call ringing */
#define VIBRATE_MS 600
#define RING_MS 60000          /* unanswered: missed */

static struct {
	lv_obj_t *scr;
	lv_timer_t *ring, *give_up;
	bool lock_hidden; /* came over the lock screen, which comes back after it */
	ui_alert_fn done;
	void *data;
} al;

/* Even with touch vibration off */
static void vibrate(void)
{
	bool haptics = ui_haptics;
	ui_haptics = true;
	ui_vibrate(VIBRATE_MS);
	ui_haptics = haptics;
}

static void ring_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	vibrate();
}

/* Its timers stopped and the lock screen back: what it was for is the app's */
static ui_alert_fn finish(void **data)
{
	ui_alert_fn done = al.done;
	*data = al.data;
	if (al.ring)
		lv_timer_delete(al.ring);
	if (al.give_up)
		lv_timer_delete(al.give_up);
	if (al.lock_hidden)
		ui_lock_hide(false);
	memset(&al, 0, sizeof(al));
	return done;
}

static void end(enum ui_alert_end how)
{
	if (!al.scr)
		return;
	lv_obj_t *scr = al.scr;
	void *data;
	ui_alert_fn done = finish(&data);
	printf("phone-ui: alert %s\n", how == UI_ALERT_STOP ? "stopped" : how == UI_ALERT_OTHER ? "answered" : "missed");
	if (ui_current() == scr)
		ui_pop();
	if (done)
		done(how, data);
}

/* Gone another way (home): stopped */
static void delete_cb(lv_event_t *e)
{
	if (lv_event_get_target_obj(e) != al.scr)
		return;
	void *data;
	ui_alert_fn done = finish(&data);
	if (done)
		done(UI_ALERT_STOP, data);
}

static void stop_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	end(UI_ALERT_STOP);
}

static void other_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	end(UI_ALERT_OTHER);
}

static void give_up_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	al.give_up = NULL; /* deleted after this, running once */
	end(UI_ALERT_MISSED);
}

void ui_alert(const char *symbol, const char *title, const char *text, const char *other, ui_alert_fn done,
              void *data)
{
	end(UI_ALERT_MISSED);
	al.done = done;
	al.data = data;

	lv_obj_t *content;
	al.scr = ui_screen_create(DIM, &content);
	lv_obj_add_event_cb(al.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(content, 32, 0);
	lv_obj_set_style_pad_top(content, 200, 0);
	lv_obj_set_style_pad_bottom(content, 120, 0);
	lv_obj_set_style_pad_row(content, 24, 0);
	lv_obj_set_scrollable(content, false);

	lv_obj_t *icon = lv_label_create(content);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ORANGE), 0);
	ui_dim_label(content, title, UI_FONT_TITLE);
	lv_obj_t *big = lv_label_create(content);
	lv_label_set_text_static(big, text);
	lv_label_set_long_mode(big, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(big, LV_PCT(100));
	lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(big, UI_FONT_DISPLAY, 0);

	lv_obj_t *space = ui_box(content);
	lv_obj_set_width(space, 1);
	lv_obj_set_flex_grow(space, 1);

	lv_obj_t *row = ui_box(content);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	if (other) {
		lv_obj_t *b = ui_button(row, 300, 128, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, other, other_cb, NULL);
		lv_obj_set_style_text_font(lv_obj_get_child(b, 0), UI_FONT_HEADLINE, 0);
	}
	lv_obj_t *stop = ui_button(row, other ? 300 : 400, 128, UI_COLOR_RED, UI_COLOR_RED_PRESSED, _("Stop"), stop_cb,
	                           NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(stop, 0), UI_FONT_HEADLINE, 0);

	/* Over the lock screen, as a call: it comes back once stopped */
	if (ui_locked()) {
		ui_lock_hide(true);
		al.lock_hidden = true;
	}
	ui_push_now(al.scr);
	lv_display_trigger_activity(NULL);
	vibrate();
	al.ring = lv_timer_create(ring_cb, VIBRATE_EVERY_MS, NULL);
	al.give_up = lv_timer_create(give_up_cb, RING_MS, NULL);
	lv_timer_set_repeat_count(al.give_up, 1);
	printf("phone-ui: alert: %s, %s\n", title, text);
}

bool ui_alert_ringing(void)
{
	return al.scr != NULL;
}

void ui_alert_stop(void)
{
	end(UI_ALERT_STOP);
}
