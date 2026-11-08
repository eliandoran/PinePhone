/*
 * Clock: the app's screen, its pages switched by a navigation bar. See clock.h.
 */

#include <string.h>
#include <time.h>

#include "clock.h"
#include "ui_symbols.h"
#include "src/indev/lv_indev_private.h" /* the pointer's scroll throw, for the wheels */

static struct {
	lv_obj_t *nav, *pages[CLOCK_PAGES];
} clk;

uint64_t clock_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
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

lv_obj_t *clock_setup_create(lv_obj_t *page)
{
	lv_obj_t *col = ui_box(page);
	lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 40, 0);
	return col;
}

/* Wheels: LVGL's roller, its options "00" to the last, repeated so that it turns round
 * (LV_ROLLER_MODE_INFINITE); restyled: the values dim, the chosen one on a band */

#define WHEEL_W 160
#define WHEEL_ROWS 5
#define WHEEL_LINE_SPACE 16
/* Let go while moving, a wheel turns on as far as the finger's last movement takes it with
 * this friction (% of the speed lost per frame), WHEEL_THROW_ROWS at most: LVGL's 10 for
 * scrolling sent a flick of 3 values through 13, a long swipe through half the hours */
#define WHEEL_THROW 45
#define WHEEL_THROW_ROWS 8
#define WHEEL_TURN_MS 300 /* to the value let go on, or tapped */
#define COLON_W 40

static int32_t wheel_row(lv_obj_t *w)
{
	return lv_font_get_line_height(lv_obj_get_style_text_font(w, LV_PART_MAIN)) + WHEEL_LINE_SPACE;
}

static uint8_t throw_was; /* the pointer's, while a wheel is let go */

static void throw_set_cb(lv_event_t *e)
{
	lv_indev_t *indev = lv_indev_active();
	if (!indev || throw_was)
		return;
	throw_was = indev->scroll_throw;
	indev->scroll_throw = WHEEL_THROW;
	/* The speed whose run-out (speed * 100 / friction) is that many rows */
	int32_t max = WHEEL_THROW_ROWS * wheel_row(lv_event_get_target_obj(e)) * WHEEL_THROW / 100;
	lv_point_t *v = &indev->pointer.scroll_throw_vect_ori;
	v->y = LV_CLAMP(-max, v->y, max);
}

static void throw_reset_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_indev_t *indev = lv_indev_active();
	if (indev && throw_was) {
		indev->scroll_throw = throw_was;
		throw_was = 0;
	}
}

/* While dragged, its values (three copies, LV_ROLLER_MODE_INFINITE) moved by a copy's
 * height (the same values: nothing changes on screen) to keep its middle off the first copy
 * and off the end, by more than a throw (WHEEL_THROW_ROWS) and a row of rounding. The roller
 * moves back to the middle copy itself only once it stops turning: swipes one after the
 * other ran off the end. And let go on the first copy, lv_roller_set_selected() takes the
 * row for a value of the copy nearest the one chosen before: after a drag of more than half
 * the values, the wrong one, a whole turn away the other way. */
static void wrap_cb(lv_event_t *e)
{
	lv_obj_t *w = lv_event_get_target_obj(e);
	lv_obj_t *label = lv_obj_get_child(w, 0);
	int32_t row = wheel_row(w), copy = (int32_t)lv_roller_get_option_count(w) * row;
	int32_t margin = (WHEEL_THROW_ROWS + 1) * row;
	int32_t y = lv_obj_get_y_aligned(label);
	int32_t middle = lv_obj_get_height(w) / 2 - y; /* in the label */
	if (middle < copy + margin)
		lv_obj_set_y(label, y - copy);
	else if (middle > lv_obj_get_height(label) - margin)
		lv_obj_set_y(label, y + copy);
}

static lv_obj_t *wheel_create(lv_obj_t *parent, unsigned count, lv_event_cb_t cb)
{
	char options[100 * 3];
	size_t n = 0;
	for (unsigned i = 0; i < count && i < 100; i++)
		n += (size_t)snprintf(options + n, sizeof(options) - n, i ? "\n%02u" : "%02u", i);

	lv_obj_t *w = lv_roller_create(parent);
	lv_obj_remove_style_all(w);
	lv_roller_set_options(w, options, LV_ROLLER_MODE_INFINITE);
	lv_obj_set_width(w, WHEEL_W);
	lv_obj_set_style_text_font(w, UI_FONT_DISPLAY, 0);
	lv_obj_set_style_text_line_space(w, WHEEL_LINE_SPACE, 0);
	lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(w, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_anim_duration(w, WHEEL_TURN_MS, 0);
	lv_obj_set_style_bg_opa(w, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_bg_color(w, lv_color_hex(UI_COLOR_CARD), LV_PART_SELECTED);
	lv_obj_set_style_radius(w, 24, LV_PART_SELECTED);
	lv_obj_set_style_text_color(w, lv_color_hex(UI_COLOR_TEXT), LV_PART_SELECTED);
	lv_roller_set_visible_row_count(w, WHEEL_ROWS);
	lv_obj_add_event_cb(w, cb, LV_EVENT_VALUE_CHANGED, NULL);
	/* The roller works out the throw as it is released: more friction for it, the
	 * pointer's own back after */
	lv_obj_add_event_cb(w, throw_set_cb, LV_EVENT_RELEASED | LV_EVENT_PREPROCESS, NULL);
	lv_obj_add_event_cb(w, throw_set_cb, LV_EVENT_PRESS_LOST | LV_EVENT_PREPROCESS, NULL);
	lv_obj_add_event_cb(w, throw_reset_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(w, throw_reset_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(w, wrap_cb, LV_EVENT_PRESSING, NULL);
	return w;
}

lv_obj_t *clock_wheels_create(lv_obj_t *parent, const char *title0, unsigned count0, const char *title1,
                              unsigned count1, lv_event_cb_t cb, lv_obj_t *wheels[2])
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 12, 0);

	/* Each wheel's title over it */
	lv_obj_t *titles = ui_box(col);
	lv_obj_set_size(titles, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(titles, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(titles, COLON_W, 0);
	const char *title[2] = { title0, title1 };
	for (int i = 0; i < 2; i++) {
		lv_obj_t *t = ui_dim_label(titles, title[i], UI_FONT_CAPTION);
		lv_obj_set_width(t, WHEEL_W);
		lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	}

	lv_obj_t *row = ui_box(col);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	wheels[0] = wheel_create(row, count0, cb);
	lv_obj_t *colon = lv_label_create(row);
	lv_label_set_text_static(colon, ":");
	lv_obj_set_width(colon, COLON_W);
	lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(colon, UI_FONT_DISPLAY, 0);
	wheels[1] = wheel_create(row, count1, cb);
	return col;
}

void clock_wheel_set(lv_obj_t *wheel, unsigned value)
{
	if (lv_roller_get_selected(wheel) != value)
		lv_roller_set_selected(wheel, value, LV_ANIM_OFF);
}

unsigned clock_wheel_get(lv_obj_t *wheel)
{
	return (unsigned)lv_roller_get_selected(wheel);
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
