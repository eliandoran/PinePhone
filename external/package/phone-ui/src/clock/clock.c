/*
 * Clock: the app's screen, its three pages switched by a navigation bar. See clock.h.
 */

#include <string.h>
#include <time.h>

#include "clock.h"
#include "../ui_symbols.h"

#define PAGES 3

static struct {
	lv_obj_t *nav, *pages[PAGES];
} clk;

uint64_t clock_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

lv_obj_t *clock_page_create(lv_obj_t *parent)
{
	lv_obj_t *page = ui_box(parent);
	lv_obj_set_width(page, LV_PCT(100));
	lv_obj_set_flex_grow(page, 1);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(page, 20, 0);
	lv_obj_set_style_pad_bottom(page, 40, 0);
	return page;
}

lv_obj_t *clock_spacer(lv_obj_t *parent, int32_t height)
{
	lv_obj_t *s = ui_box(parent);
	lv_obj_set_size(s, 1, height);
	return s;
}

lv_obj_t *clock_button_row(lv_obj_t *parent)
{
	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return row;
}

void clock_button_set(lv_obj_t *button, const char *text, uint32_t color, uint32_t pressed)
{
	lv_obj_t *label = lv_obj_get_child(button, 0);
	ui_button_set_color(button, color, pressed);
	if (lv_label_get_text(label) != text)
		lv_label_set_text_static(label, text);
}

/* Navigation bar at the bottom, as Phone's */

static void tab_select(unsigned sel)
{
	ui_nav_bar_select(clk.nav, sel);
	for (unsigned i = 0; i < PAGES; i++)
		lv_obj_set_hidden(clk.pages[i], i != sel);
}

static void tab_cb(lv_event_t *e)
{
	tab_select(ui_nav_bar_index(e));
}

static void nav_create(lv_obj_t *parent)
{
	const struct ui_nav_tab tabs[PAGES] = {
		{ _("Clock"), UI_SYMBOL_CLOCK, 0 },
		{ _("Stopwatch"), UI_SYMBOL_STOPWATCH, 0 },
		{ _("Timer"), UI_SYMBOL_HOURGLASS_HALF, 0 },
	};
	clk.nav = ui_nav_bar_create(parent, tabs, PAGES, 48, tab_cb, NULL);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	world_delete();
	stopwatch_delete();
	countdown_delete();
	memset(&clk, 0, sizeof(clk));
}

void clock_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Clock"), NULL, &body);
	lv_obj_set_scrollable(body, false);

	for (int i = 0; i < PAGES; i++)
		clk.pages[i] = clock_page_create(body);
	nav_create(body);
	world_create(clk.pages[0]);
	stopwatch_create(clk.pages[1]);
	countdown_create(clk.pages[2]);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

	tab_select(0);
	ui_push(scr);
}
