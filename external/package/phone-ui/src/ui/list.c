/*
 * A long list of rows, made only for the items in view.
 */

#include <limits.h>
#include <stdlib.h>

#include "ui.h"

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

/* Rows of symbol, title, info and arrow */

struct list_row {
	lv_obj_t *symbol, *title, *info, *arrow;
};

static void list_row_free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

void ui_list_row_make(lv_obj_t *row, void *user_data)
{
	LV_UNUSED(user_data);
	struct list_row *r = malloc(sizeof(*r));
	if (!r)
		return;
	r->symbol = lv_label_create(row);
	lv_label_set_text_static(r->symbol, "");
	lv_obj_set_width(r->symbol, 48);
	lv_obj_set_style_text_align(r->symbol, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(r->symbol, &ui_font_32, 0);

	lv_obj_t *text = ui_row_text(row);
	r->title = ui_one_line_label(text, "", UI_FONT_TITLE);
	lv_obj_set_width(r->title, LV_PCT(100));
	r->info = ui_one_line_label(text, "", UI_FONT_CAPTION);
	lv_obj_set_width(r->info, LV_PCT(100));
	lv_obj_set_style_text_color(r->info, lv_color_hex(UI_COLOR_DIM), 0);

	r->arrow = lv_label_create(row);
	lv_label_set_text_static(r->arrow, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_color(r->arrow, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_set_user_data(row, r);
	lv_obj_add_event_cb(row, list_row_free_cb, LV_EVENT_DELETE, r);
}

void ui_list_row_set(lv_obj_t *row, const char *symbol, uint32_t color, const char *title, const char *info,
                     bool arrow)
{
	struct list_row *r = lv_obj_get_user_data(row);
	if (!r)
		return;
	lv_label_set_text(r->symbol, symbol);
	lv_obj_set_style_text_color(r->symbol, lv_color_hex(color), 0);
	lv_label_set_text(r->title, title);
	lv_label_set_text(r->info, info ? info : "");
	lv_obj_set_hidden(r->info, !info);
	lv_obj_set_hidden(r->arrow, !arrow);
}
