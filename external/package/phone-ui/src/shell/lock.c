/*
 * Lock screen: put up when the phone goes to sleep (power.c), so that waking it
 * shows the time and date over the wallpaper. Dragged up from anywhere, it follows
 * the finger over the screen it hides; let go past a third of the way or flicked
 * up, it goes, else it falls back.
 *
 * Below the clock, the notifications (notify.c): tapped, one unlocks and opens; a
 * drag that starts on one still unlocks, without opening it.
 *
 * It goes on the top layer, over the screens and the back gesture's strip, under
 * the status bar: the quick settings still open over it (their theme switch, which
 * would rebuild the screens under it, is disabled while locked). The power menu
 * and the volume slider are made later and go over it.
 *
 * An incoming call's screen hides it (ui_lock_hide(), phone.c) and puts it back once
 * the call is over: meanwhile the phone counts as unlocked.
 */

#include <string.h>

#include "data/data.h"
#include "i18n.h"
#include "ui.h"

#define SLOP 12    /* px moved before it follows */
#define FLICK 400  /* px/s */
#define TICK_MS 1000
#define NOTIFICATIONS 4 /* shown, then how many more */

static struct {
	lv_obj_t *scr, *clock;
	lv_obj_t *area;          /* the clock's, and the notifications' */
	lv_obj_t *notifications; /* NULL: none */
	unsigned version;        /* data_version they show */
	lv_timer_t *timer;
	int32_t h;
	int32_t pos;   /* how far up it is, 0 to h */
	int32_t y0, y, v; /* where it started following, last position, speed (px/s) */
	uint32_t t;    /* of the last position */
	bool dragging;
	bool dragged;  /* since the press: a notification's tap does not open it */
	bool hidden;   /* by ui_lock_hide() */
} lk;

/* A notification tapped, not dragged, on the lock screen still there */
static bool may_open(void)
{
	return lk.scr && !lk.dragged;
}

static void notifications_update(void)
{
	if (lk.notifications)
		lv_obj_delete(lk.notifications);
	lk.version = data_version;
	lk.notifications = notifications_card_create(lk.area, NOTIFICATIONS, may_open, NULL);
	if (!lk.notifications)
		return;
	lv_obj_update_layout(lk.clock);
	lv_obj_align_to(lk.notifications, lk.clock, LV_ALIGN_OUT_BOTTOM_MID, 0, 48);
}

void ui_lock_update(void)
{
	if (!lk.scr)
		return;
	ui_clock_align(lk.timer);
	ui_clock_update(lk.clock);
	if (lk.version != data_version)
		notifications_update();
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	ui_lock_update();
}

static void pos_set(int32_t pos)
{
	lk.pos = LV_CLAMP(0, pos, lk.h);
	lv_obj_set_y(lk.scr, -lk.pos);
}

static void pos_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	pos_set(v);
}

/* Asynchronously: unlocked from its own events */
static void destroy(void)
{
	lv_anim_delete(&lk, NULL);
	lv_timer_delete(lk.timer);
	lv_obj_delete_async(lk.scr);
	memset(&lk, 0, sizeof(lk));
	ui_chrome_update(); /* the status bar's time back */
}

static void gone_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	destroy();
}

/* Slides to `target` (0: locked, h: gone) from where it is, slowing down */
static void slide_to(int32_t target)
{
	lv_anim_delete(&lk, NULL);
	if (!ui_anim_ms || lk.pos == target) {
		pos_set(target);
		if (target)
			destroy();
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &lk);
	lv_anim_set_exec_cb(&a, pos_exec_cb);
	lv_anim_set_values(&a, lk.pos, target);
	lv_anim_set_duration(&a, LV_MAX(ui_anim_ms * LV_ABS(target - lk.pos) / lk.h, 60));
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (target)
		lv_anim_set_completed_cb(&a, gone_cb);
	lv_anim_start(&a);
}

static void drag_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		lk.y0 = lk.y = p.y;
		lk.v = 0;
		lk.t = lv_tick_get();
		lk.dragging = lk.dragged = false;
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (!lk.dragging) {
			if (lk.y0 - p.y < SLOP)
				return;
			lv_anim_delete(&lk, NULL);
			lk.dragging = lk.dragged = true;
			lk.y0 = p.y + lk.pos;
		}
		uint32_t dt = lv_tick_elaps(lk.t);
		if (dt) {
			lk.v = (lk.v + (lk.y - p.y) * 1000 / (int32_t)dt) / 2;
			lk.y = p.y;
			lk.t = lv_tick_get();
		}
		pos_set(lk.y0 - p.y);
		return;
	}
	if (!lk.dragging)
		return;
	lk.dragging = false;
	if (lv_tick_elaps(lk.t) > 100) /* held still before letting go */
		lk.v = 0;
	bool unlock = lk.v > FLICK || (lk.v > -FLICK && lk.pos > lk.h / 3);
	slide_to(unlock ? lk.h : 0);
}

void ui_lock(void)
{
	ui_shade_close(); /* or it would be over what shows instead of the lock screen (a call) */
	if (lk.scr) {
		/* Half dragged away, or hidden: back down */
		ui_lock_hide(false);
		lk.dragging = false;
		lv_anim_delete(&lk, NULL);
		pos_set(0);
		return;
	}
	ui_upright(); /* made for the display's size, upright (turn.c turns nothing while locked) */
	lv_obj_t *top = lv_layer_top();
	lv_obj_t *s = lk.scr = ui_box(top);
	lv_obj_set_size(s, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_text_color(s, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_clickable(s, true);
	lv_obj_set_gesture_bubble(s, false);
	lv_obj_add_event_cb(s, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(s, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(s, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(s, drag_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *img = lv_image_create(s);
	lv_image_set_src(img, ui_wallpaper(LV_OPA_30));

	/* Where the home screen has it: in an area under the status bar, like its content */
	lv_obj_t *area = lk.area = ui_box(s);
	lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_pad_top(area, UI_STATUS_H, 0);
	lv_obj_set_style_pad_hor(area, 24, 0); /* the notifications' card */
	lv_obj_set_event_bubble(area, true);   /* their presses on to the drag */
	lk.clock = ui_clock_create(area);
	notifications_update();

	lv_obj_t *hint = ui_box(s);
	lv_obj_set_size(hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(hint, 12, 0);
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -96);
	lv_obj_t *arrow = lv_label_create(hint);
	lv_label_set_text_static(arrow, LV_SYMBOL_UP);
	lv_obj_set_style_text_font(arrow, &ui_font_32, 0);
	ui_dim_label(hint, _("Swipe up to unlock"), UI_FONT_BODY);

	lk.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	ui_clock_align(lk.timer);
	lk.h = lv_obj_get_height(top);
	pos_set(0);

	/* Under the status bar, over everything else (an open quick settings panel too);
	 * the bar without its time, the lock screen shows it */
	ui_status_bar_foreground();
	ui_chrome_update();
}

bool ui_locked(void)
{
	return lk.scr && !lk.hidden;
}

void ui_unlock(void)
{
	if (lk.scr) {
		lk.dragging = false;
		slide_to(lk.h);
	}
}

void ui_lock_hide(bool hide)
{
	if (!lk.scr || lk.hidden == hide)
		return;
	if (!hide) {
		ui_upright(); /* the call's screen may have turned */
		lk.dragging = false;
		lv_anim_delete(&lk, NULL);
		pos_set(0);
	}
	lk.hidden = hide;
	lv_obj_set_hidden(lk.scr, hide);
	ui_chrome_update(); /* the status bar's time: the lock screen's own while it shows */
}
