/*
 * Back gesture, as on the iPhone: a swipe from the left edge drags the screen away,
 * uncovering the previous one. Let go past half way or flicked to the right, it goes
 * back; else it returns. A strip along the left edge, on the top layer, takes the
 * touches while there is a screen to go back to (not on the home screen or the menu),
 * except over a keyboard: its left keys reach under it.
 *
 * Home gesture, as on the iPhone: a swipe up from the bottom edge lifts the screen
 * away, uncovering the home screen; the screens between are closed. Let go past a
 * quarter of the way or flicked up, it goes home; else it comes back. A strip along
 * the bottom edge with a handle bar, on the top layer, shown with the back gesture's;
 * over a keyboard too, whose keys end above it.
 */

#include "ui_internal.h"
#include "src/core/lv_obj_event_private.h" /* the back gesture strip's hit test */

#define EDGE_W 28     /* px: the strip (the back buttons start at 12) */
#define EDGE_SLOP 12  /* px moved before the screen follows */
#define EDGE_FLICK 400 /* px/s */

#define HANDLE_W 140
#define HANDLE_H 6

static lv_obj_t *edge, *home_edge; /* home_edge: the home gesture's */
static struct {
	int32_t x0;    /* where the screen started following */
	int32_t x, v;  /* last position, speed (px/s), along the gesture (y for home_edge) */
	uint32_t t;    /* of the last position */
} swipe;

void gestures_update(void)
{
	bool none = ui_at_home() || nav_chrome_hidden();
	if (edge)
		lv_obj_set_hidden(edge, none);
	if (home_edge)
		lv_obj_set_hidden(home_edge, none);
}

void gestures_recolor(void)
{
	if (home_edge)
		lv_obj_set_style_bg_color(lv_obj_get_child(home_edge, 0), lv_color_hex(UI_COLOR_TEXT), 0);
}

void gestures_resize(void)
{
	if (edge)
		lv_obj_set_size(edge, EDGE_W, ui_screen_h - UI_STATUS_H);
}

static void edge_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		swipe.x0 = swipe.x = p.x;
		swipe.v = 0;
		swipe.t = lv_tick_get();
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (!transition_dragging()) {
			/* Not while a transition runs, or before the finger moved enough */
			lv_obj_t *below = nav_back_target();
			if (!below || p.x - swipe.x0 < EDGE_SLOP ||
			    !transition_drag_begin(below, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT))
				return;
			swipe.x0 = p.x;
		}
		uint32_t dt = lv_tick_elaps(swipe.t);
		if (dt) {
			swipe.v = (swipe.v + (p.x - swipe.x) * 1000 / (int32_t)dt) / 2;
			swipe.x = p.x;
			swipe.t = lv_tick_get();
		}
		transition_drag(p.x - swipe.x0);
		return;
	}
	/* Released or lost: the slide finishes from where it is */
	if (!transition_dragging())
		return;
	if (lv_tick_elaps(swipe.t) > 100) /* held still before letting go */
		swipe.v = 0;
	bool done = swipe.v > EDGE_FLICK || (swipe.v > -EDGE_FLICK && transition_pos() > ui_screen_w / 2);
	transition_drag_end(done, true);
	if (done) {
		nav_went_back();
		ui_chrome_update();
	}
}

static void edge_hit_test_cb(lv_event_t *e)
{
	lv_hit_test_info_t *hit = lv_event_get_hit_test_info(e);
	lv_point_t p = *hit->point;
	for (lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &p); o; o = lv_obj_get_parent(o))
		if (lv_obj_check_type(o, &lv_keyboard_class)) {
			hit->res = false;
			return;
		}
}

static void edge_init(void)
{
	edge = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(edge);
	lv_obj_set_size(edge, EDGE_W, ui_screen_h - UI_STATUS_H);
	lv_obj_set_pos(edge, 0, UI_STATUS_H);
	lv_obj_set_scrollable(edge, false);
	lv_obj_set_scroll_chain(edge, false);
	lv_obj_set_gesture_bubble(edge, false);
	lv_obj_set_adv_hittest(edge, true);
	lv_obj_add_event_cb(edge, edge_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESS_LOST, NULL);
	gestures_update();
}

static void home_edge_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		swipe.x0 = swipe.x = p.y;
		swipe.v = 0;
		swipe.t = lv_tick_get();
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (!transition_dragging()) {
			lv_obj_t *home = nav_home_target();
			if (!home || swipe.x0 - p.y < EDGE_SLOP ||
			    !transition_drag_begin(home, LV_SCREEN_LOAD_ANIM_OUT_TOP))
				return;
			swipe.x0 = p.y;
		}
		uint32_t dt = lv_tick_elaps(swipe.t);
		if (dt) {
			swipe.v = (swipe.v + (swipe.x - p.y) * 1000 / (int32_t)dt) / 2; /* up */
			swipe.x = p.y;
			swipe.t = lv_tick_get();
		}
		transition_drag(swipe.x0 - p.y);
		return;
	}
	if (!transition_dragging())
		return;
	if (lv_tick_elaps(swipe.t) > 100)
		swipe.v = 0;
	bool done = swipe.v > EDGE_FLICK || (swipe.v > -EDGE_FLICK && transition_pos() > ui_screen_h / 4);
	/* The top one deleted: never the menu, which has no strip */
	transition_drag_end(done, true);
	if (done) {
		nav_went_home();
		ui_chrome_update();
	}
}

static void home_edge_init(void)
{
	home_edge = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(home_edge);
	lv_obj_set_size(home_edge, LV_PCT(100), UI_HOME_EDGE_H);
	lv_obj_align(home_edge, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_scrollable(home_edge, false);
	lv_obj_set_scroll_chain(home_edge, false);
	lv_obj_set_gesture_bubble(home_edge, false);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(home_edge, home_edge_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *handle = ui_box(home_edge); /* colored by gestures_recolor() */
	lv_obj_set_size(handle, HANDLE_W, HANDLE_H);
	lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(handle, LV_OPA_50, 0);
	lv_obj_set_style_bg_color(handle, lv_color_hex(UI_COLOR_TEXT), 0);
	gestures_update();
}

void gestures_init(void)
{
	edge_init();
	home_edge_init();
}
