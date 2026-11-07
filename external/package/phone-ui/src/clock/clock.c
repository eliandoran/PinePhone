/*
 * Clock: the app's screen, its pages switched by a navigation bar. See clock.h.
 */

#include <string.h>
#include <time.h>

#include "clock.h"
#include "../ui_symbols.h"

static struct {
	lv_obj_t *nav, *pages[CLOCK_PAGES];
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

lv_obj_t *clock_round_button(lv_obj_t *row, const char *text, uint32_t color, uint32_t pressed, lv_event_cb_t cb)
{
	return ui_button(row, 168, 168, color, pressed, text, cb, NULL);
}

lv_obj_t *clock_display_create(lv_obj_t *page, struct digits *d, const char *pattern)
{
	clock_spacer(page, 72);
	digits_create(d, page, pattern);
	lv_obj_t *box = ui_box(page);
	lv_obj_set_size(box, LV_PCT(100), 88);
	return box;
}

lv_obj_t *clock_setup_create(lv_obj_t *page)
{
	lv_obj_t *col = ui_box(page);
	lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 40, 0);
	return col;
}

void clock_adjuster_create(lv_obj_t *parent, const char *title, lv_event_cb_t cb, int step)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	ui_dim_label(col, title, UI_FONT_CAPTION);

	lv_obj_t *row = ui_box(col);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 20, 0);

	for (int sign = -1; sign <= 1; sign += 2) {
		lv_obj_t *b = ui_button(row, 96, 96, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
		                        sign < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS, NULL, NULL);
		lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &ui_font_32, 0);
		/* Holding it repeats */
		lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, (void *)(intptr_t)(sign * step));
		lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(sign * step));
	}
}

/* Navigation bar at the bottom, as Phone's */

static void tab_select(unsigned sel)
{
	ui_nav_bar_select(clk.nav, sel);
	for (unsigned i = 0; i < CLOCK_PAGES; i++)
		lv_obj_set_hidden(clk.pages[i], i != sel);
}

static void tab_cb(lv_event_t *e)
{
	tab_select(ui_nav_bar_index(e));
}

static void nav_create(lv_obj_t *parent)
{
	const struct ui_nav_tab tabs[CLOCK_PAGES] = {
		[CLOCK_WORLD] = { _("Clock"), UI_SYMBOL_CLOCK },
		[CLOCK_ALARM] = { _("Alarm"), LV_SYMBOL_BELL },
		[CLOCK_STOPWATCH] = { _("Stopwatch"), UI_SYMBOL_STOPWATCH },
		[CLOCK_TIMER] = { _("Timer"), UI_SYMBOL_HOURGLASS_HALF },
	};
	clk.nav = ui_nav_bar_create(parent, tabs, CLOCK_PAGES, 48, tab_cb, NULL);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	world_delete();
	alarm_page_delete();
	stopwatch_delete();
	countdown_delete();
	memset(&clk, 0, sizeof(clk));
}

void clock_open_page(enum clock_page page)
{
	/* Already open (a notification tapped over it): that page */
	if (clk.nav) {
		tab_select(page);
		return;
	}
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Clock"), NULL, &body);
	lv_obj_set_scrollable(body, false);

	for (int i = 0; i < CLOCK_PAGES; i++)
		clk.pages[i] = clock_page_create(body);
	nav_create(body);
	world_create(clk.pages[CLOCK_WORLD]);
	alarm_page_create(clk.pages[CLOCK_ALARM]);
	stopwatch_create(clk.pages[CLOCK_STOPWATCH]);
	countdown_create(clk.pages[CLOCK_TIMER]);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

	tab_select(page);
	ui_push(scr);
}

void clock_open(void)
{
	clock_open_page(CLOCK_WORLD);
}
