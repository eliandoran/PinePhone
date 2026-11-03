/*
 * Clock: the app's screen, a segmented control over its three pages. See clock.h.
 */

#include <string.h>
#include <time.h>

#include "clock.h"

#define PAGES 3

static struct {
	lv_obj_t *tabs[PAGES], *pages[PAGES];
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

/* Segmented control */

static void tab_select(uintptr_t sel)
{
	for (uintptr_t i = 0; i < PAGES; i++) {
		lv_obj_set_state(clk.tabs[i], LV_STATE_CHECKED, i == sel);
		lv_obj_set_hidden(clk.pages[i], i != sel);
	}
}

static void tab_cb(lv_event_t *e)
{
	tab_select((uintptr_t)lv_event_get_user_data(e));
}

static void segments_create(lv_obj_t *parent)
{
	static const char *const names[PAGES] = { "Clock", "Stopwatch", "Timer" };

	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_pad_bottom(bar, 8, 0);

	lv_obj_t *seg = ui_box(bar);
	lv_obj_set_size(seg, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(seg, 6, 0);
	lv_obj_set_style_pad_column(seg, 6, 0);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);

	for (uintptr_t i = 0; i < PAGES; i++) {
		lv_obj_t *tab = ui_box(seg);
		lv_obj_set_height(tab, 80);
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_white(), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, (void *)i);

		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, names[i]);
		lv_obj_center(label);
		clk.tabs[i] = tab;
	}
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
	lv_obj_t *scr = ui_app_screen_create("Clock", NULL, &body);
	lv_obj_set_scrollable(body, false);

	segments_create(body);
	for (int i = 0; i < PAGES; i++)
		clk.pages[i] = clock_page_create(body);
	world_create(clk.pages[0]);
	stopwatch_create(clk.pages[1]);
	countdown_create(clk.pages[2]);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

	tab_select(0);
	ui_push(scr);
}
