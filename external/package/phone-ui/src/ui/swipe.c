/*
 * Rows swiped away and lists pulled down to refresh them.
 */

#include <stdlib.h>

#include "ui.h"

/*
 * Swiping a row away: the row moves by its translation, over a box of the color
 * made in its place in the parent (outside the layout, just before it) for the
 * swipe. It starts once the finger went SWIPE_SLOP to the left, more sideways than
 * up or down, and the list did not start scrolling: from then on the row does not
 * chain vertical scrolls to the list, and its clicks are stopped before its own
 * callbacks see them (LV_EVENT_PREPROCESS).
 */

#define SWIPE_SLOP 12   /* px to the left before the row follows */
#define SWIPE_FLICK 400 /* px/s */
#define SWIPE_MS 150    /* sliding out or back */
#define CLOSE_MS 150    /* the gap closing up */

struct swipe_away {
	lv_obj_t *row, *under;
	const char *symbol;
	uint32_t color;
	void (*done)(lv_obj_t *row, void *user_data);
	void *user_data;
	int32_t x0, y0; /* pressed at, then where it started following (x0) */
	int32_t x, v;   /* last x, speed (px/s) */
	uint32_t t;     /* of the last x */
	bool swiping;
	bool swiped; /* since pressed: no click */
};

static void under_delete_cb(lv_event_t *e)
{
	struct swipe_away *s = lv_event_get_user_data(e);
	s->under = NULL;
}

/* The color under the row, where it stands */
static void under_create(struct swipe_away *s)
{
	lv_obj_t *row = s->row;
	lv_obj_update_layout(row);
	lv_obj_t *under = s->under = ui_box(lv_obj_get_parent(row));
	lv_obj_set_ignore_layout(under, true);
	lv_obj_set_pos(under, lv_obj_get_x(row), lv_obj_get_y(row));
	lv_obj_set_size(under, lv_obj_get_width(row), lv_obj_get_height(row));
	lv_obj_set_style_radius(under, 24, 0);
	lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(under, lv_color_hex(s->color), 0);
	lv_obj_t *icon = lv_label_create(under);
	lv_label_set_text(icon, s->symbol);
	lv_obj_set_style_text_font(icon, UI_FONT_BODY_LARGE, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -40, 0);
	lv_obj_move_to_index(under, lv_obj_get_index(row));
	lv_obj_add_event_cb(under, under_delete_cb, LV_EVENT_DELETE, s);
}

static void swipe_translate_cb(void *row, int32_t x)
{
	lv_obj_set_style_translate_x(row, x, 0);
}

static void swipe_height_cb(void *row, int32_t h)
{
	lv_obj_set_height(row, h);
}

static void swipe_closed_cb(lv_anim_t *a)
{
	struct swipe_away *s = lv_anim_get_user_data(a);
	s->done(s->row, s->user_data);
}

static void swipe_out_cb(lv_anim_t *a)
{
	struct swipe_away *s = lv_anim_get_user_data(a);
	if (s->under)
		lv_obj_delete(s->under);
	/* The gap closes up */
	lv_obj_t *row = s->row;
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_anim_t c;
	lv_anim_init(&c);
	lv_anim_set_var(&c, row);
	lv_anim_set_user_data(&c, s);
	lv_anim_set_exec_cb(&c, swipe_height_cb);
	lv_anim_set_values(&c, lv_obj_get_height(row), 0);
	lv_anim_set_duration(&c, CLOSE_MS);
	lv_anim_set_path_cb(&c, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&c, swipe_closed_cb);
	lv_anim_start(&c);
}

static void swipe_back_cb(lv_anim_t *a)
{
	struct swipe_away *s = lv_anim_get_user_data(a);
	if (s->under)
		lv_obj_delete(s->under);
}

static void swipe_cb(lv_event_t *e)
{
	struct swipe_away *s = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p = {0};
	if (indev)
		lv_indev_get_point(indev, &p);

	switch (code) {
	case LV_EVENT_PRESSED:
		s->x0 = s->x = p.x;
		s->y0 = p.y;
		s->v = 0;
		s->t = lv_tick_get();
		s->swiped = false;
		return;
	case LV_EVENT_CLICKED:
	case LV_EVENT_SHORT_CLICKED:
	case LV_EVENT_LONG_PRESSED:
	case LV_EVENT_LONG_PRESSED_REPEAT:
		if (s->swiped) {
			lv_event_stop_processing(e);
			lv_event_stop_bubbling(e);
		}
		return;
	case LV_EVENT_PRESSING:
		if (!s->swiping) {
			int32_t dx = p.x - s->x0, dy = p.y - s->y0;
			if (s->swiped || lv_anim_get(s->row, NULL) || (indev && lv_indev_get_scroll_obj(indev)) ||
			    dx > -SWIPE_SLOP || -dx < LV_ABS(dy))
				return;
			s->swiping = s->swiped = true;
			lv_obj_remove_state(s->row, LV_STATE_PRESSED);
			lv_obj_set_scroll_chain_ver(s->row, false);
			under_create(s);
			s->x0 = p.x;
		}
		uint32_t dt = lv_tick_elaps(s->t);
		if (dt) {
			s->v = (s->v + (p.x - s->x) * 1000 / (int32_t)dt) / 2;
			s->x = p.x;
			s->t = lv_tick_get();
		}
		lv_obj_set_style_translate_x(s->row, LV_MIN(0, p.x - s->x0), 0);
		return;
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: {
		if (!s->swiping)
			return;
		s->swiping = false;
		lv_obj_set_scroll_chain_ver(s->row, true);
		if (lv_tick_elaps(s->t) > 100) /* held still before letting go */
			s->v = 0;
		int32_t w = lv_obj_get_width(s->row), at = lv_obj_get_style_translate_x(s->row, 0);
		bool out = s->v < -SWIPE_FLICK || (s->v < SWIPE_FLICK && -at > w / 3);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, s->row);
		lv_anim_set_user_data(&a, s);
		lv_anim_set_exec_cb(&a, swipe_translate_cb);
		lv_anim_set_values(&a, at, out ? -w : 0);
		lv_anim_set_duration(&a, SWIPE_MS);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_set_completed_cb(&a, out ? swipe_out_cb : swipe_back_cb);
		lv_anim_start(&a);
		return;
	}
	case LV_EVENT_DELETE:
		if (s->under)
			lv_obj_delete(s->under);
		free(s);
		return;
	default:
		return;
	}
}

/*
 * Pulling a list down to refresh it: LVGL's elastic scroll past the top (the list
 * moves half the finger's way), which LVGL only gives a list with something to
 * scroll: the list says it is 1 px higher than its content area (its self size),
 * which keeps it so without a child its owner could delete. The sign floats over
 * the list (it does not scroll), in the gap the pull opens, made again when the
 * list was emptied (lv_obj_clean()). The pull is the finger's from SCROLL_BEGIN
 * (without an animation) to SCROLL_THROW_BEGIN, its release.
 */

#define PULL_SIGN 64     /* px: the sign's circle */
#define PULL_FAR 120     /* px of the list's pull (half the finger's) to refresh */
#define PULL_REST 16     /* px from the top, while refreshing */
#define PULL_TIMEOUT 15000

struct pull_refresh {
	lv_obj_t *list, *sign;
	void (*refresh)(void *user_data);
	void *user_data;
	lv_timer_t *timeout;
	bool pulling, far, refreshing;
};

static void sign_delete_cb(lv_event_t *e)
{
	struct pull_refresh *r = lv_event_get_user_data(e);
	r->sign = NULL;
}

static void pull_sign_set(struct pull_refresh *r, int32_t y, bool accent)
{
	if (!r->sign) {
		lv_obj_t *sign = r->sign = ui_box(r->list);
		lv_obj_set_floating(sign, true);
		lv_obj_set_size(sign, PULL_SIGN, PULL_SIGN);
		lv_obj_set_align(sign, LV_ALIGN_TOP_MID);
		lv_obj_set_style_radius(sign, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(sign, LV_OPA_COVER, 0);
		lv_obj_t *icon = lv_label_create(sign);
		lv_label_set_text_static(icon, LV_SYMBOL_REFRESH);
		lv_obj_set_style_text_font(icon, UI_FONT_BODY_LARGE, 0);
		lv_obj_center(icon);
		lv_obj_add_event_cb(sign, sign_delete_cb, LV_EVENT_DELETE, r);
	}
	/* Over the rows */
	lv_obj_move_to_index(r->sign, -1);
	lv_obj_set_hidden(r->sign, false);
	lv_obj_set_y(r->sign, y);
	lv_obj_set_style_bg_color(r->sign, lv_color_hex(accent ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
	lv_obj_set_style_text_color(r->sign, accent ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
}

static void pull_opa_cb(void *sign, int32_t opa)
{
	lv_obj_set_style_opa(sign, opa, 0);
}

static void pull_end(struct pull_refresh *r)
{
	r->refreshing = false;
	if (r->timeout) {
		lv_timer_delete(r->timeout);
		r->timeout = NULL;
	}
	if (r->sign) {
		lv_anim_delete(r->sign, pull_opa_cb);
		lv_obj_set_style_opa(r->sign, LV_OPA_COVER, 0);
		lv_obj_set_hidden(r->sign, true);
	}
}

static void pull_sign_hide(struct pull_refresh *r)
{
	if (r->sign)
		lv_obj_set_hidden(r->sign, true);
}

static void pull_timeout_cb(lv_timer_t *t)
{
	struct pull_refresh *r = lv_timer_get_user_data(t);
	r->timeout = NULL; /* deleted after this, run once */
	pull_end(r);
}

/* After the release's events: refresh() may delete the row pressed, as it reads a
 * list again at once */
static void pull_async_cb(void *data)
{
	struct pull_refresh *r = data;
	r->refresh(r->user_data);
}

static void pull_cb(lv_event_t *e)
{
	struct pull_refresh *r = lv_event_get_user_data(e);
	lv_obj_t *list = r->list;
	switch (lv_event_get_code(e)) {
	case LV_EVENT_GET_SELF_SIZE: {
		/* One px to scroll */
		lv_point_t *size = lv_event_get_param(e);
		size->y = LV_MAX(size->y, lv_obj_get_content_height(list) + 1);
		return;
	}
	case LV_EVENT_SCROLL_BEGIN:
		if (!lv_event_get_param(e))
			r->pulling = !r->refreshing;
		return;
	case LV_EVENT_SCROLL: {
		if (!r->pulling)
			return;
		int32_t pull = -lv_obj_get_scroll_y(list);
		r->far = pull >= PULL_FAR;
		if (pull <= 0)
			pull_sign_hide(r);
		else
			pull_sign_set(r, LV_MIN(pull, PULL_FAR + PULL_REST) - PULL_SIGN, r->far);
		return;
	}
	case LV_EVENT_SCROLL_THROW_BEGIN:
		if (!r->pulling)
			return;
		r->pulling = false;
		if (!r->far) {
			pull_sign_hide(r);
			return;
		}
		r->far = false;
		r->refreshing = true;
		pull_sign_set(r, PULL_REST, true);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, r->sign);
		lv_anim_set_exec_cb(&a, pull_opa_cb);
		lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_40);
		lv_anim_set_duration(&a, 500);
		lv_anim_set_reverse_duration(&a, 500);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
		r->timeout = lv_timer_create(pull_timeout_cb, PULL_TIMEOUT, r);
		lv_timer_set_repeat_count(r->timeout, 1);
		lv_async_call(pull_async_cb, r);
		return;
	case LV_EVENT_DELETE:
		if (r->timeout)
			lv_timer_delete(r->timeout);
		lv_async_call_cancel(pull_async_cb, r);
		if (r->sign)
			lv_obj_remove_event_cb_with_user_data(r->sign, sign_delete_cb, r);
		free(r);
		return;
	default:
		return;
	}
}

void ui_pull_refresh(lv_obj_t *list, void (*refresh)(void *user_data), void *user_data)
{
	struct pull_refresh *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	*r = (struct pull_refresh){.list = list, .refresh = refresh, .user_data = user_data};
	/* Pressed between its rows too (a ui_box() is not), or the drag goes past it */
	lv_obj_set_clickable(list, true);
	static const lv_event_code_t codes[] = {LV_EVENT_GET_SELF_SIZE, LV_EVENT_SCROLL_BEGIN, LV_EVENT_SCROLL,
						LV_EVENT_SCROLL_THROW_BEGIN, LV_EVENT_DELETE};
	for (size_t i = 0; i < LV_ARRAYLEN(codes); i++)
		lv_obj_add_event_cb(list, pull_cb, codes[i], r);
}

void ui_pull_refresh_done(lv_obj_t *list)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(list); i++) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(list, i);
		if (lv_event_dsc_get_cb(d) == pull_cb) {
			struct pull_refresh *r = lv_event_dsc_get_user_data(d);
			if (r->refreshing)
				pull_end(r);
			return;
		}
	}
}

void ui_swipe_away(lv_obj_t *row, const char *symbol, uint32_t color, void (*done)(lv_obj_t *row, void *user_data),
                   void *user_data)
{
	struct swipe_away *s = calloc(1, sizeof(*s));
	if (!s)
		return;
	*s = (struct swipe_away){.row = row, .symbol = symbol, .color = color, .done = done, .user_data = user_data};
	static const lv_event_code_t codes[] = {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
						LV_EVENT_PRESS_LOST, LV_EVENT_DELETE};
	for (size_t i = 0; i < LV_ARRAYLEN(codes); i++)
		lv_obj_add_event_cb(row, swipe_cb, codes[i], s);
	static const lv_event_code_t clicks[] = {LV_EVENT_CLICKED, LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED,
						 LV_EVENT_LONG_PRESSED_REPEAT};
	for (size_t i = 0; i < LV_ARRAYLEN(clicks); i++)
		lv_obj_add_event_cb(row, swipe_cb, clicks[i] | LV_EVENT_PREPROCESS, s);
}
