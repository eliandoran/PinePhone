/*
 * Widgets and formatting shared by the apps.
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i18n.h"
#include "ui.h"

lv_obj_t *ui_one_line_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	/* Clipped to one line: no dots, they would be written into the static text */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

lv_obj_t *ui_empty_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(label, 200, 0);
	return label;
}

lv_obj_t *ui_section_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = ui_dim_label(parent, text, UI_FONT_CAPTION);
	/* In line with the rows' content */
	lv_obj_set_style_pad_left(label, 24, 0);
	lv_obj_set_style_margin_top(label, 16, 0);
	return label;
}

lv_obj_t *ui_card_create(lv_obj_t *parent)
{
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_clickable(card, false);
	lv_obj_set_scrollable(card, false);
	return card;
}

lv_obj_t *ui_slider_create(lv_obj_t *parent)
{
	lv_obj_t *slider = lv_slider_create(parent);
	lv_obj_set_height(slider, 14);
	lv_obj_set_flex_grow(slider, 1);
	lv_obj_set_ext_click_area(slider, 36);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	/* On the light theme's white cards and shade, the edge shows it */
	lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
	lv_obj_set_style_border_color(slider, lv_color_hex(UI_COLOR_KNOB_EDGE), LV_PART_KNOB);
	lv_obj_set_style_pad_all(slider, 16, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
	return slider;
}

lv_obj_t *ui_switch_create(lv_obj_t *parent, bool on)
{
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_size(sw, 100, 56);
	lv_obj_set_ext_click_area(sw, 16);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUSED);
	lv_obj_set_state(sw, LV_STATE_CHECKED, on);
	return sw;
}

lv_obj_t *ui_box(lv_obj_t *parent)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_set_clickable(box, false);
	lv_obj_set_scrollable(box, false);
	return box;
}

#define TILE_PAD 12 /* around a tile's text */

lv_obj_t *ui_tile_grid_create(lv_obj_t *parent)
{
	lv_obj_t *grid = ui_box(parent);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(grid, UI_TILE_GAP, 0);
	lv_obj_set_style_pad_bottom(grid, 32, 0);
	lv_obj_set_style_pad_row(grid, UI_TILE_GAP, 0);
	lv_obj_set_style_pad_column(grid, UI_TILE_GAP, 0);
	lv_obj_set_scrollable(grid, true);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
	return grid;
}

int32_t ui_tile_width(int columns)
{
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	return (w - (columns + 1) * UI_TILE_GAP) / columns;
}

lv_obj_t *ui_tile_create(lv_obj_t *grid, int32_t w, int32_t pic_h, lv_event_cb_t cb, void *user_data,
                         lv_obj_t **pic, lv_obj_t **text)
{
	lv_obj_t *tile = ui_box(grid);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_bottom(tile, TILE_PAD, 0);
	lv_obj_set_style_radius(tile, UI_TILE_RADIUS, 0);
	lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_CARD), 0);
	if (cb) {
		lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_clickable(tile, true);
		lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, user_data);
	}

#if LV_USE_3DTEXTURE
	*pic = lv_3dtexture_create(tile);
#else
	*pic = ui_box(tile);
#endif
	lv_obj_set_clickable(*pic, false);

	*text = ui_box(tile);
	lv_obj_set_size(*text, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(*text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(*text, TILE_PAD, 0);
	lv_obj_set_style_pad_top(*text, 10, 0);
	lv_obj_set_style_pad_row(*text, 4, 0);
	ui_tile_set_size(tile, w, pic_h);
	return tile;
}

void ui_tile_set_size(lv_obj_t *tile, int32_t w, int32_t pic_h)
{
	lv_obj_set_size(tile, w, LV_SIZE_CONTENT);
	lv_obj_set_size(lv_obj_get_child(tile, 0), w, pic_h);
}

lv_obj_t *ui_row_create(lv_obj_t *card, int32_t height, bool divider, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *row = ui_box(card);
	lv_obj_set_size(row, LV_PCT(100), height);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(row, 24, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	/* The divider is a border without radius, see ui.h */
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	ui_row_set_divider(row, divider);
	if (cb) {
		lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_clickable(row, true);
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	}
	return row;
}

void ui_row_set_divider(lv_obj_t *row, bool divider)
{
	lv_obj_set_style_border_width(row, divider ? 1 : 0, 0);
}

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

lv_obj_t *ui_row_text(lv_obj_t *row)
{
	lv_obj_t *text = ui_box(row);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 6, 0);
	return text;
}

lv_obj_t *ui_row_line(lv_obj_t *text)
{
	lv_obj_t *line = ui_box(text);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 16, 0);
	return line;
}

lv_obj_t *ui_dim_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

struct ui_row_lines ui_row_lines(lv_obj_t *row, const char *title, const char *time, const char *preview)
{
	struct ui_row_lines l;
	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *top = ui_row_line(text);
	l.title = ui_one_line_label(top, title, UI_FONT_TITLE);
	lv_obj_set_flex_grow(l.title, 1);
	l.time = ui_dim_label(top, time, UI_FONT_CAPTION);
	/* Read as much as the title: body text, only the time smaller */
	l.preview = ui_one_line_label(ui_row_line(text), preview, UI_FONT_BODY);
	lv_obj_set_flex_grow(l.preview, 1);
	lv_obj_set_style_text_color(l.preview, lv_color_hex(UI_COLOR_DIM), 0);
	return l;
}

lv_obj_t *ui_count_badge(lv_obj_t *parent, unsigned count)
{
	lv_obj_t *badge = lv_label_create(parent);
	ui_set_text_fmt(badge, "%u", count);
	lv_obj_set_style_text_font(badge, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(badge, lv_color_white(), 0);
	lv_obj_set_style_bg_color(badge, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(badge, 10, 0);
	lv_obj_set_style_pad_ver(badge, 2, 0);
	lv_obj_set_style_min_width(badge, 36, 0);
	lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_margin_left(badge, 12, 0);
	return badge;
}

lv_obj_t *ui_round_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                          const char *symbol, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *button = ui_button(parent, size, size, color, pressed, symbol, cb, user_data);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), size >= 120 ? &ui_font_48 : &ui_font_32, 0);
	return button;
}

lv_obj_t *ui_action_button(lv_obj_t *body, const char *symbol, lv_event_cb_t cb, void *user_data)
{
	enum { SIZE = 104, MARGIN = 32 /* from the right edge, above the home gesture's strip */ };
	lv_obj_t *button = ui_round_button(lv_obj_get_parent(body), SIZE, UI_COLOR_ACCENT,
	                                   UI_COLOR_ACCENT_PRESSED, symbol, cb, user_data);
	lv_obj_set_floating(button, true);
	lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -(UI_HOME_EDGE_H + MARGIN));
	if (lv_obj_is_scrollable(body))
		lv_obj_set_style_pad_bottom(body, UI_ACTION_CLEAR, 0);
	return button;
}

lv_obj_t *ui_captioned_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                              const char *symbol, const char *caption, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, 160, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	lv_obj_t *button = ui_round_button(col, size, color, pressed, symbol, cb, user_data);
	ui_dim_label(col, caption, UI_FONT_BODY);
	return button;
}

lv_obj_t *ui_button(lv_obj_t *parent, int32_t w, int32_t h, uint32_t color, uint32_t pressed,
                    const char *text, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *button = lv_obj_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, w, h);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	ui_button_set_color(button, color, pressed);
	lv_obj_set_scrollable(button, false);
	if (cb)
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_center(label);
	return button;
}

void ui_button_set_color(lv_obj_t *button, uint32_t color, uint32_t pressed)
{
	lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(pressed), LV_STATE_PRESSED);
	/* White on colors, the text color on grays */
	lv_obj_set_style_text_color(button, color == UI_COLOR_BUTTON || color == UI_COLOR_CARD ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_white(), 0);
}

lv_obj_t *ui_bar_create(lv_obj_t *parent)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_column(bar, 12, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);
	return bar;
}

lv_obj_t *ui_nav_bar_create(lv_obj_t *parent, const struct ui_nav_tab *tabs, unsigned n,
                            int32_t icon_size, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), 128);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);

	for (unsigned i = 0; i < n; i++) {
		lv_obj_t *tab = ui_box(bar);
		lv_obj_set_height(tab, LV_PCT(100));
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(tab, 8, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, cb, LV_EVENT_CLICKED, user_data);

		lv_obj_t *icon = lv_label_create(tab);
		lv_label_set_text_static(icon, tabs[i].symbol);
		lv_obj_set_style_text_font(icon, icon_size >= 48 ? &ui_font_48 : &ui_font_32, 0);
		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, tabs[i].name);
		lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	}
	return bar;
}

void ui_nav_bar_select(lv_obj_t *bar, unsigned index)
{
	for (unsigned i = 0; i < lv_obj_get_child_count(bar); i++)
		lv_obj_set_state(lv_obj_get_child(bar, (int32_t)i), LV_STATE_CHECKED, i == index);
}

unsigned ui_nav_bar_index(lv_event_t *e)
{
	return (unsigned)lv_obj_get_index(lv_event_get_current_target_obj(e));
}

lv_obj_t *ui_textarea_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *ta = lv_textarea_create(parent);
	lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(ta, 0, 0);
	lv_obj_set_style_border_width(ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(ta, 0, 0);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_CLICKED, user_data);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_READY, user_data);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_CANCEL, user_data);
	return ta;
}

lv_obj_t *ui_text_field(lv_obj_t *parent, int32_t height, uint32_t max_length, lv_event_cb_t cb,
                        void *user_data)
{
	lv_obj_t *ta = ui_textarea_create(parent, cb, user_data);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_max_length(ta, max_length);
	lv_obj_set_height(ta, height);
	lv_obj_set_style_radius(ta, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(ta, 28, 0);
	/* The line of text centered */
	int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(ta, LV_PART_MAIN));
	lv_obj_set_style_pad_ver(ta, (height - line) / 2, 0);
	lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_CARD), 0);
	return ta;
}

lv_obj_t *ui_compose_bar(lv_obj_t *parent, uint32_t max_length, lv_event_cb_t ta_cb,
                         lv_event_cb_t send_cb, void *user_data)
{
	lv_obj_t *bar = ui_bar_create(parent);
	/* As tall as its field, the send button at its last line */
	lv_obj_set_height(bar, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(bar, (112 - 80) / 2, 0);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_t *ta = ui_text_field(bar, 80, max_length, ta_cb, user_data);
	lv_textarea_set_placeholder_text(ta, _("Message"));
	lv_obj_set_flex_grow(ta, 1);
	/* Wrapped, growing up to COMPOSE_LINES lines, then scrolling; Enter still sends */
	int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(ta, LV_PART_MAIN));
	lv_textarea_set_one_line(ta, false);
	ui_textarea_set_enter_sends(ta);
	lv_obj_set_height(ta, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(ta, 80, 0);
	lv_obj_set_style_max_height(ta, 80 + (COMPOSE_LINES - 1) * line, 0);
	lv_obj_set_style_radius(ta, 40, 0);

	lv_obj_t *send = ui_button(bar, 80, 80, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, LV_SYMBOL_UP,
	                           send_cb, user_data);
	lv_obj_set_style_text_font(lv_obj_get_child(send, 0), &ui_font_32, 0);
	return ta;
}

char *ui_one_line(const char *prefix, const char *original, size_t max)
{
	char *buf = malloc(max + 1);
	if (!buf)
		return NULL;
	size_t n = snprintf(buf, max + 1, "%s", prefix);
	if (n > max)
		n = max;
	bool space = false;
	/* Emoji sequences first: not cut in the middle */
	char *composed = ui_emoji_compose(original);
	const char *text = composed ? composed : "";
	while (*text) {
		if (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r') {
			space = n > 0;
			text++;
			continue;
		}
		/* Whole characters only */
		unsigned char c = *text;
		size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		if (n + space + len > max || strnlen(text, len) < len)
			break;
		if (space)
			buf[n++] = ' ';
		space = false;
		memcpy(buf + n, text, len);
		n += len;
		text += len;
	}
	buf[n] = '\0';
	free(composed);
	return buf;
}

/* Conversations */

void ui_chat_scroll_end(struct ui_chat *chat, lv_anim_enable_t anim)
{
	lv_obj_update_layout(chat->list);
	lv_obj_scroll_to_y(chat->list, lv_obj_get_scroll_y(chat->list) + lv_obj_get_scroll_bottom(chat->list), anim);
}

static void chat_keyboard(struct ui_chat *chat, bool show)
{
	ui_keyboard_show(chat->kb, show);
	ui_chat_scroll_end(chat, LV_ANIM_OFF);
}

static void chat_send(struct ui_chat *chat)
{
	const char *text = lv_textarea_get_text(chat->ta);
	while (*text == ' ' || *text == '\n')
		text++;
	size_t n = strlen(text);
	while (n && (text[n - 1] == ' ' || text[n - 1] == '\n'))
		n--;
	/* Copied first: hiding the keyboard unfocuses the text area, whose label then
	 * sets its text again (reallocated), with Enter while it has the focus */
	char *trimmed = n ? strndup(text, n) : NULL;
	chat_keyboard(chat, false);
	if (!trimmed)
		return;
	lv_textarea_set_text(chat->ta, "");
	ui_vibrate(UI_VIBRATE_TAP);
	chat->send(chat, trimmed);
	free(trimmed);
	ui_chat_reply(chat, 0, NULL, NULL);
}

static void chat_send_cb(lv_event_t *e)
{
	chat_send(lv_event_get_user_data(e));
}

static void chat_ta_cb(lv_event_t *e)
{
	struct ui_chat *chat = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		chat_keyboard(chat, true);
		break;
	case LV_EVENT_READY: /* the keyboard's Enter key */
		chat_send(chat);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		chat_keyboard(chat, false);
		break;
	default:
		break;
	}
}

/* The list shrinking (the field grew a line): what was at its bottom stays there */
static void chat_list_size_cb(lv_event_t *e)
{
	lv_obj_t *list = lv_event_get_current_target_obj(e);
	const lv_area_t *was = lv_event_get_param(e);
	int32_t shrunk = lv_area_get_height(was) - lv_obj_get_height(list);
	int32_t below = lv_obj_get_scroll_bottom(list);
	if (shrunk > 0 && below > 0)
		lv_obj_scroll_by(list, 0, -LV_MIN(shrunk, below), LV_ANIM_OFF);
}

static void chat_click_cb(lv_event_t *e)
{
	struct ui_chat *chat = lv_event_get_user_data(e);
	if (!lv_obj_is_hidden(chat->kb))
		chat_keyboard(chat, false);
}

/* Replies */

#define REPLY_SWIPE 120    /* px a message is swiped left to reply */
#define REPLY_TEXT_MAX 200 /* bytes of the message answered, on one line */

/* Who wrote it in `color`, its text on one line under it, after a bar of that color */
static lv_obj_t *quote_create(lv_obj_t *parent, lv_color_t color, lv_color_t text_color, int32_t width,
                              lv_obj_t **name, lv_obj_t **text)
{
	lv_obj_t *quote = ui_box(parent);
	lv_obj_set_size(quote, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(quote, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_border_side(quote, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_border_width(quote, 4, 0);
	lv_obj_set_style_border_color(quote, color, 0);
	lv_obj_set_style_border_opa(quote, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_left(quote, 16, 0);

	lv_obj_t **labels[] = { name, text };
	for (unsigned i = 0; i < 2; i++) {
		lv_obj_t *label = *labels[i] = lv_label_create(quote);
		lv_label_set_text_static(label, "");
		lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
		lv_obj_set_style_text_color(label, i ? text_color : color, 0);
		/* One line, dots where it is cut */
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_max_width(label, width, 0);
		lv_obj_set_height(label, lv_font_get_line_height(UI_FONT_CAPTION));
	}
	return quote;
}

static void quote_set(lv_obj_t *name, lv_obj_t *text, const char *who, const char *what)
{
	lv_label_set_text(name, who ? who : "");
	char *line = ui_one_line("", what ? what : "", REPLY_TEXT_MAX);
	lv_label_set_text(text, line ? line : "");
	free(line);
}

static void reply_cancel_cb(lv_event_t *e)
{
	ui_chat_reply(lv_event_get_user_data(e), 0, NULL, NULL);
}

/* Above the compose bar, hidden until replying */
static lv_obj_t *reply_bar_create(struct ui_chat *chat, lv_obj_t *body)
{
	lv_obj_t *bar = ui_bar_create(body);
	lv_obj_set_height(bar, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(bar, 12, 0);
	/* The screen's width but the bar's padding, its gap and the button */
	int32_t width = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 16 - 12 - 80;
	lv_obj_t *quote = quote_create(bar, lv_color_hex(UI_COLOR_ACCENT), lv_color_hex(UI_COLOR_TEXT), width,
	                               &chat->reply_name, &chat->reply_text);
	lv_obj_set_flex_grow(quote, 1);
	lv_obj_t *cancel = ui_button(bar, 80, 80, UI_COLOR_BAR, UI_COLOR_PRESSED, LV_SYMBOL_CLOSE,
	                             reply_cancel_cb, chat);
	lv_obj_set_style_text_color(lv_obj_get_child(cancel, 0), lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_hidden(bar, true);
	return bar;
}

void ui_chat_reply(struct ui_chat *chat, long long id, const char *name, const char *text)
{
	chat->reply_to = id;
	lv_obj_set_hidden(chat->reply_bar, !id);
	if (!id)
		return;
	quote_set(chat->reply_name, chat->reply_text, name, text);
	chat_keyboard(chat, true);
}

lv_obj_t *ui_chat_quote_create(lv_obj_t *label, bool outgoing, const char *name, const char *text)
{
	lv_obj_t *bubble = lv_obj_get_parent(label);
	lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(bubble, 8, 0);
	/* White on an outgoing bubble (the accent color) */
	lv_color_t color = outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_ACCENT);
	lv_color_t text_color = outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT);
	lv_obj_t *who, *what;
	lv_obj_t *quote = quote_create(bubble, color, text_color, lv_obj_get_style_max_width(label, 0) - 20,
	                               &who, &what);
	lv_obj_move_to_index(quote, 0);
	quote_set(who, what, name, text);
	return quote;
}

/* A message swiped left follows the finger, the list held still meanwhile; let go
 * far enough, it is replied to */
struct replyable {
	struct ui_chat *chat;
	long long id;
	lv_point_t start;
	bool swiping;
};

static void replyable_cb(lv_event_t *e)
{
	struct replyable *r = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p = { 0, 0 };
	if (indev)
		lv_indev_get_point(indev, &p);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		r->start = p;
		r->swiping = false;
		break;
	case LV_EVENT_PRESSING: {
		int32_t dx = p.x - r->start.x, dy = p.y - r->start.y;
		if (!r->swiping) {
			/* Scrolling the list, or not clearly sideways yet */
			if (!indev || lv_indev_get_scroll_obj(indev) || dx > -16 || -dx < 2 * LV_ABS(dy))
				break;
			r->swiping = true;
			lv_obj_set_scroll_chain_ver(obj, false);
		}
		lv_obj_set_style_translate_x(obj, LV_CLAMP(-REPLY_SWIPE * 3 / 2, dx, 0), 0);
		break;
	}
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: {
		if (!r->swiping)
			break;
		bool reply = lv_event_get_code(e) == LV_EVENT_RELEASED &&
		             lv_obj_get_style_translate_x(obj, 0) <= -REPLY_SWIPE;
		r->swiping = false;
		lv_obj_set_scroll_chain_ver(obj, true);
		lv_obj_set_style_translate_x(obj, 0, 0);
		/* Not a click as well (a photo opening, the keyboard hidden) */
		if (indev && lv_event_get_code(e) == LV_EVENT_RELEASED)
			lv_indev_reset(indev, obj);
		if (reply && r->chat->reply) {
			ui_vibrate(UI_VIBRATE_TAP);
			r->chat->reply(r->chat, r->id);
		}
		break;
	}
	case LV_EVENT_CLICKED:
		/* As a tap on the list: the keyboard hidden */
		lv_obj_send_event(r->chat->list, LV_EVENT_CLICKED, NULL);
		break;
	case LV_EVENT_DELETE:
		free(r);
		break;
	default:
		break;
	}
}

void ui_chat_replyable(struct ui_chat *chat, lv_obj_t *obj, long long id)
{
	struct replyable *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->chat = chat;
	r->id = id;
	lv_obj_set_clickable(obj, true);
	lv_obj_add_event_cb(obj, replyable_cb, LV_EVENT_ALL, r);
}

void ui_chat_create(struct ui_chat *chat, lv_obj_t *body, uint32_t max_length)
{
	lv_obj_set_scrollable(body, false);
	chat->list = lv_obj_create(body);
	lv_obj_remove_style_all(chat->list);
	lv_obj_set_width(chat->list, LV_PCT(100));
	lv_obj_set_flex_grow(chat->list, 1);
	lv_obj_set_flex_flow(chat->list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(chat->list, 16, 0);
	lv_obj_set_style_pad_ver(chat->list, 16, 0);
	lv_obj_set_style_pad_row(chat->list, 24, 0);
	lv_obj_set_scroll_dir(chat->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(chat->list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(chat->list, chat_click_cb, LV_EVENT_CLICKED, chat);
	lv_obj_add_event_cb(chat->list, chat_list_size_cb, LV_EVENT_SIZE_CHANGED, NULL);

	chat->reply_bar = reply_bar_create(chat, body);
	chat->ta = ui_compose_bar(body, max_length, chat_ta_cb, chat_send_cb, chat);
	/* Under the bar, shown while typing; the messages shrink above it */
	ui_textarea_set_prose(chat->ta);
	chat->kb = ui_keyboard_create(body, chat->ta);
}

lv_obj_t *ui_chat_group_create(struct ui_chat *chat, bool outgoing, lv_obj_t **time)
{
	lv_obj_t *group = ui_box(chat->list);
	lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START,
	                      outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
	                      outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(group, 6, 0);

	lv_obj_t *label = lv_label_create(group);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_hor(label, 12, 0);
	lv_obj_set_style_margin_top(label, 2, 0);
	if (time)
		*time = label;
	return group;
}

lv_obj_t *ui_chat_bubble_create(lv_obj_t *group, bool outgoing)
{
	lv_obj_t *bubble = ui_box(group);
	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bubble, 24, 0);
	lv_obj_set_style_pad_ver(bubble, 16, 0);
	lv_obj_set_style_radius(bubble, 24, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bubble, lv_color_hex(outgoing ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
	/* Before the time */
	lv_obj_move_to_index(bubble, (int32_t)lv_obj_get_child_count(group) - 2);

	/* Wraps at 75% of the screen (bubble padding included) */
	lv_obj_t *label = lv_label_create(bubble);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_max_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) * 3 / 4 - 48, 0);
	lv_obj_set_style_text_color(label, outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
	return label;
}

lv_obj_t *ui_confirm_bar(lv_obj_t *parent, const char *action, lv_event_cb_t confirm_cb,
                         lv_event_cb_t cancel_cb, void *user_data)
{
	lv_obj_t *bar = ui_bar_create(parent);
	lv_obj_t *cancel = ui_button(bar, LV_SIZE_CONTENT, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                             _("Cancel"), cancel_cb, user_data);
	lv_obj_set_flex_grow(cancel, 1);
	lv_obj_t *confirm = ui_button(bar, LV_SIZE_CONTENT, 80, UI_COLOR_RED, UI_COLOR_RED_PRESSED,
	                              action, confirm_cb, user_data);
	lv_obj_set_flex_grow(confirm, 1);
	lv_obj_set_hidden(bar, true);
	return bar;
}

lv_obj_t *ui_avatar_create(lv_obj_t *parent, int32_t size)
{
	lv_obj_t *avatar = ui_box(parent);
	lv_obj_set_size(avatar, size, size);
	lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
	/* On the avatar, so that it can be changed there */
	lv_obj_set_style_text_font(avatar, size >= 160 ? &ui_font_48 :
	                           size >= 96 ? &ui_font_32 :
	                           size >= 72 ? &ui_font_26 : &ui_font_20, 0);
	lv_obj_set_style_text_color(avatar, lv_color_white(), 0);

	lv_obj_t *text = lv_label_create(avatar);
	lv_label_set_text_static(text, "");
	lv_obj_center(text);
#if LV_USE_3DTEXTURE
	lv_obj_t *photo = lv_3dtexture_create(avatar);
	lv_obj_set_size(photo, size, size);
	lv_obj_set_clickable(photo, false);
	lv_obj_set_hidden(photo, true);
#endif
	return avatar;
}

void ui_avatar_set(lv_obj_t *avatar, uint32_t color, const char *text)
{
	lv_obj_set_style_bg_color(avatar, lv_color_hex(color), 0);
	ui_set_text(lv_obj_get_child(avatar, 0), text);
}

void ui_avatar_set_photo(lv_obj_t *avatar, unsigned tex)
{
#if LV_USE_3DTEXTURE
	lv_obj_t *photo = lv_obj_get_child(avatar, 1);
	lv_obj_set_hidden(photo, !tex);
	if (tex)
		lv_3dtexture_set_src(photo, tex);
	/* Behind the photo's soft edge: no colored fringe */
	lv_obj_set_hidden(lv_obj_get_child(avatar, 0), tex);
	lv_obj_set_style_bg_opa(avatar, tex ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
#else
	LV_UNUSED(avatar);
	LV_UNUSED(tex);
#endif
}

void ui_size_format(char *buf, size_t size, unsigned long long bytes)
{
	static const char *const units[] = { "kB", "MB", "GB", "TB" };
	if (bytes < 1000) {
		snprintf(buf, size, "%llu B", bytes);
		return;
	}
	double v = bytes / 1000.0;
	unsigned u = 0;
	/* As rounded: 999.6 kB is 1.0 MB, not 1000 kB */
	while (v >= 999.5 && u + 1 < sizeof(units) / sizeof(units[0])) {
		v /= 1000;
		u++;
	}
	snprintf(buf, size, v < 9.95 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

void ui_when_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
		strftime(buf, size, "%H:%M", &tm);
	else if (now - t < 6 * 24 * 3600)
		snprintf(buf, size, "%s", ui_day_name(tm.tm_wday, true));
	else if (tm.tm_year == today.tm_year)
		snprintf(buf, size, "%d %s", tm.tm_mday, ui_month_name(tm.tm_mon, true));
	else
		snprintf(buf, size, "%d %s %d", tm.tm_mday, ui_month_name(tm.tm_mon, true), tm.tm_year + 1900);
}

void ui_offset_format(char *buf, size_t size, long seconds)
{
	int off = (int)(seconds / 60 % (24 * 60)); /* minutes */
	char sign = off < 0 ? '-' : '+';
	if (off < 0)
		off = -off;
	if (off == 0)
		snprintf(buf, size, "UTC");
	else if (off % 60)
		snprintf(buf, size, "UTC%c%d:%02d", sign, off / 60, off % 60);
	else
		snprintf(buf, size, "UTC%c%d", sign, off / 60);
}

void ui_tz_localtime(const char *tz, time_t t, struct tm *tm)
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

/*
 * A virtual list (ui.h): a pool of rows, each showing the item whose index is its
 * number modulo the pool's size, so scrolling the window by a row binds one row again,
 * not all of them. The pool holds two screens of rows: the window is the rows in view
 * with half a screen before and after. Computed from the card's and the list's
 * coordinates as they are when the list scrolls (LVGL moves the children as it
 * scrolls) and when the count changes; before the first layout they are 0, the window
 * starts at 0 then, as the list does.
 */

struct ui_list {
	int32_t row_h;
	unsigned count, pool, start;
	lv_obj_t **rows;  /* the pool, NULL until needed */
	unsigned *shown;  /* the item each shows, UINT_MAX none */
	void (*make)(lv_obj_t *row, void *user_data);
	void (*bind)(lv_obj_t *row, unsigned index, void *user_data);
	void (*click)(unsigned index, void *user_data);
	void *user_data;
};

static void list_click_cb(lv_event_t *e)
{
	struct ui_list *l = lv_event_get_user_data(e);
	lv_obj_t *row = lv_event_get_current_target(e);
	for (unsigned i = 0; i < l->pool; i++)
		if (l->rows[i] == row && l->shown[i] != UINT_MAX && l->click) {
			l->click(l->shown[i], l->user_data);
			return;
		}
}

/* The rows for the items in and around the view bound, the others hidden */
static void list_window(lv_obj_t *card)
{
	struct ui_list *l = lv_obj_get_user_data(card);
	lv_obj_t *list = lv_obj_get_parent(card);
	lv_area_t lc, cc;
	lv_obj_get_coords(list, &lc);
	lv_obj_get_coords(card, &cc);
	int32_t pad = lv_obj_get_style_pad_top(card, 0);
	int32_t top = lc.y1 - cc.y1 - pad, bottom = lc.y2 - cc.y1 - pad;
	unsigned first = top > 0 ? (unsigned)(top / l->row_h) : 0;
	unsigned last = bottom > 0 ? (unsigned)(bottom / l->row_h) : 0;
	unsigned visible = last >= first ? last - first + 1 : 1;
	unsigned margin = visible < l->pool ? (l->pool - visible) / 2 : 0;
	unsigned start = first > margin ? first - margin : 0;
	if (start + l->pool > l->count)
		start = l->count > l->pool ? l->count - l->pool : 0;
	l->start = start;
	for (unsigned index = start; index < start + l->pool; index++) {
		unsigned slot = index % l->pool;
		lv_obj_t *row = l->rows[slot];
		if (index >= l->count) {
			if (row)
				lv_obj_set_hidden(row, true);
			continue;
		}
		if (!row) {
			row = l->rows[slot] = ui_row_create(card, l->row_h, true, list_click_cb, l);
			l->make(row, l->user_data);
		}
		if (l->shown[slot] != index) {
			l->shown[slot] = index;
			lv_obj_set_pos(row, 0, (int32_t)index * l->row_h);
			ui_row_set_divider(row, index + 1 < l->count);
			l->bind(row, index, l->user_data);
		}
		lv_obj_set_hidden(row, false);
	}
}

static void list_scroll_cb(lv_event_t *e)
{
	list_window(lv_event_get_user_data(e));
}

static void list_delete_cb(lv_event_t *e)
{
	struct ui_list *l = lv_event_get_user_data(e);
	free(l->rows);
	free(l->shown);
	free(l);
}

lv_obj_t *ui_list_create(lv_obj_t *list, int32_t row_h, void (*make)(lv_obj_t *row, void *user_data),
                         void (*bind)(lv_obj_t *row, unsigned index, void *user_data),
                         void (*click)(unsigned index, void *user_data), void *user_data)
{
	lv_obj_t *card = ui_card_create(list);
	lv_obj_set_layout(card, LV_LAYOUT_NONE);
	struct ui_list *l = calloc(1, sizeof(*l));
	l->row_h = row_h;
	unsigned screen = (unsigned)((lv_display_get_vertical_resolution(lv_display_get_default()) + row_h - 1) / row_h);
	l->pool = 2 * screen;
	l->rows = calloc(l->pool, sizeof(*l->rows));
	l->shown = malloc(l->pool * sizeof(*l->shown));
	for (unsigned i = 0; i < l->pool; i++)
		l->shown[i] = UINT_MAX;
	l->make = make;
	l->bind = bind;
	l->click = click;
	l->user_data = user_data;
	lv_obj_set_user_data(card, l);
	lv_obj_add_event_cb(card, list_delete_cb, LV_EVENT_DELETE, l);
	lv_obj_add_event_cb(list, list_scroll_cb, LV_EVENT_SCROLL, card);
	lv_obj_set_height(card, 0);
	return card;
}

void ui_list_set_count(lv_obj_t *card, unsigned count)
{
	struct ui_list *l = lv_obj_get_user_data(card);
	l->count = count;
	lv_obj_set_height(card, (int32_t)count * l->row_h + lv_obj_get_style_pad_top(card, 0) +
	                            lv_obj_get_style_pad_bottom(card, 0));
	/* Every row again: the items may have moved */
	for (unsigned i = 0; i < l->pool; i++)
		l->shown[i] = UINT_MAX;
	list_window(card);
}

void ui_list_refresh(lv_obj_t *card, unsigned index)
{
	struct ui_list *l = lv_obj_get_user_data(card);
	unsigned slot = index % l->pool;
	if (index < l->count && l->rows[slot] && l->shown[slot] == index)
		l->bind(l->rows[slot], index, l->user_data);
}
