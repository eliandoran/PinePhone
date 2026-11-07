/*
 * A bar at the bottom of a list going from page to page: first, previous,
 * PAGER_NUMBERS page numbers around the one shown (selected), next, last; those
 * leading nowhere dimmed, the bar hidden for one page. Its owner keeps the struct,
 * sets page and pages, and goes to the page go() is called with.
 */

#include "github.h"
#include "ui_symbols.h"

#define BUTTON_W 68

static void enable(lv_obj_t *button, bool on)
{
	lv_obj_set_state(button, LV_STATE_DISABLED, !on);
}

void pager_update(struct pager *p)
{
	lv_obj_set_hidden(p->bar, p->pages <= 1);
	if (p->pages <= 1)
		return;
	int count = p->pages < PAGER_NUMBERS ? p->pages : PAGER_NUMBERS;
	int start = p->page - PAGER_NUMBERS / 2;
	if (start > p->pages - count + 1)
		start = p->pages - count + 1;
	if (start < 1)
		start = 1;
	for (int i = 0; i < PAGER_NUMBERS; i++) {
		lv_obj_t *b = p->numbers[i];
		lv_obj_set_hidden(b, i >= count);
		lv_obj_set_user_data(b, (void *)(intptr_t)(start + i));
		lv_label_set_text_fmt(lv_obj_get_child(b, 0), "%d", start + i);
		lv_obj_set_state(b, LV_STATE_CHECKED, start + i == p->page);
	}
	enable(p->first, p->page > 1);
	enable(p->prev, p->page > 1);
	enable(p->next, p->page < p->pages);
	enable(p->last, p->page < p->pages);
}

/* A button's user data: the page, or -1 first, -2 previous, -3 next, -4 last */
static void page_cb(lv_event_t *e)
{
	struct pager *p = lv_event_get_user_data(e);
	int to = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
	if (to == -1)
		to = 1;
	else if (to == -2)
		to = p->page - 1;
	else if (to == -3)
		to = p->page + 1;
	else if (to == -4)
		to = p->pages;
	if (to >= 1 && to <= p->pages)
		p->go(to);
}

static lv_obj_t *page_button(struct pager *p, const char *text, intptr_t to)
{
	lv_obj_t *b = ui_button(p->bar, BUTTON_W, 72, UI_COLOR_CARD, UI_COLOR_PRESSED, text, page_cb, p);
	lv_obj_set_user_data(b, (void *)to);
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), UI_FONT_BODY, 0);
	/* The page shown */
	lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
	/* Leading nowhere */
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	return b;
}

void pager_create(struct pager *p, lv_obj_t *body, void (*go)(int page))
{
	p->go = go;
	p->bar = ui_box(body);
	lv_obj_set_size(p->bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(p->bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p->bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_bg_opa(p->bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->bar, lv_color_hex(UI_COLOR_BAR), 0);
	p->first = page_button(p, UI_SYMBOL_ANGLES_LEFT, -1);
	p->prev = page_button(p, LV_SYMBOL_LEFT, -2);
	for (int i = 0; i < PAGER_NUMBERS; i++)
		p->numbers[i] = page_button(p, "", 0);
	p->next = page_button(p, LV_SYMBOL_RIGHT, -3);
	p->last = page_button(p, UI_SYMBOL_ANGLES_RIGHT, -4);
	pager_update(p);
}
