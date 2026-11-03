/*
 * Home screen (clock, date, a dock with apps and the menu) and the menu with the apps.
 *
 * Both turn sideways with the phone (turn.c); the menu has 8 tiles to a row then.
 * Apps open upright.
 */

#include <time.h>

#include "ui.h"

LV_FONT_DECLARE(ui_font_clock) /* ui_font_clock.c, gen-fonts.sh: digits and colon only */

static const struct ui_app builtin_apps[] = {
	{ "Phone",      LV_SYMBOL_CALL,      0x34c759, phone_open },
	{ "Messages",   LV_SYMBOL_ENVELOPE,  0x0a84ff, messages_open },
	{ "Contacts",   LV_SYMBOL_BARS,      0xff9f0a, contacts_open },
	{ "Camera",     LV_SYMBOL_EYE_OPEN,  0x8e8e93, camera_open },
	{ "Gallery",    LV_SYMBOL_IMAGE,     0xff375f, gallery_open },
	{ "Music",      LV_SYMBOL_AUDIO,     0xbf5af2, music_open },
	{ "Clock",      LV_SYMBOL_BELL,      0xff453a, clock_open },
	{ "Calendar",   LV_SYMBOL_LIST,      0xe0592a, calendar_open },
	{ "Calculator", LV_SYMBOL_PLUS,      0x5e5ce6, calculator_open },
	{ "Notes",      LV_SYMBOL_EDIT,      0xffd60a, notes_open },
	{ "Recorder",   LV_SYMBOL_AUDIO,     0xff453a, recorder_open },
	{ "Files",      LV_SYMBOL_DIRECTORY, 0x64d2ff, files_open },
	{ "Maps",       LV_SYMBOL_GPS,       0x30d158, maps_open },
	{ "Weather",    LV_SYMBOL_CHARGE,    0x409cff, weather_open },
	{ "Compass",    LV_SYMBOL_UP,        0xac8e68, compass_open },
	{ "Browser",    LV_SYMBOL_WIFI,      0x5ac8fa, browser_open },
	{ "Assistant",  LV_SYMBOL_KEYBOARD,  0x4d6bfe, assistant_open },
	{ "Snake",      LV_SYMBOL_PLAY,      0x66d4cf, snake_open },
	{ "Settings",   LV_SYMBOL_SETTINGS,  0x636366, settings_open }, /* last: external.c */
};
const struct ui_app *ui_apps = builtin_apps;
unsigned ui_app_count = sizeof(builtin_apps) / sizeof(builtin_apps[0]);

static lv_obj_t *home_clock;

lv_obj_t *ui_clock_create(lv_obj_t *parent)
{
	/* A percentage down so that it sits right upright and sideways */
	lv_obj_t *clock = ui_box(parent);
	lv_obj_set_size(clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(clock, 20, 0);
	lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, LV_PCT(12));

	lv_obj_t *time = lv_label_create(clock);
	ui_set_text(time, "");
	lv_obj_set_style_text_font(time, &ui_font_clock, 0);

	lv_obj_t *date = lv_label_create(clock);
	ui_set_text(date, "");
	lv_obj_set_style_text_font(date, &ui_font_32, 0);
	lv_obj_set_style_text_opa(date, LV_OPA_80, 0);

	ui_clock_update(clock);
	return clock;
}

void ui_clock_update(lv_obj_t *clock)
{
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(lv_obj_get_child(clock, 0), buf);
	strftime(buf, sizeof(buf), "%A %-d %B", &tm);
	ui_set_text(lv_obj_get_child(clock, 1), buf);
}

static void home_update(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	ui_clock_update(home_clock);
	ui_status_update();
}

/* Apps that are not done yet */
static void placeholder_open(const struct ui_app *app)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(app->name, NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, 32, 0);
	lv_obj_set_style_pad_bottom(body, 96, 0);

	ui_app_icon_create(body, app, 180);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "Coming soon");
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	ui_push(scr);
}

void ui_app_open(const struct ui_app *app)
{
	if (app->open)
		app->open();
	else if (app->dir)
		external_open(app);
	else
		placeholder_open(app);
}

static void open_app_cb(lv_event_t *e)
{
	ui_upright(); /* the app's screens are made for the display's size */
	ui_app_open(lv_event_get_user_data(e));
}

#define MENU_PAD 12 /* on each side of the grid */

static lv_obj_t *menu_screen, *menu_grid;

/* The menu's tiles: 4 to a row upright, 8 sideways */
static void menu_tiles_fit(void)
{
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int32_t cols = w > lv_display_get_vertical_resolution(lv_display_get_default()) ? 8 : 4;
	for (uint32_t i = 0; menu_grid && i < lv_obj_get_child_count(menu_grid); i++)
		lv_obj_set_width(lv_obj_get_child(menu_grid, i), (w - 2 * MENU_PAD) / cols);
}

static void resolution_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	menu_tiles_fit();
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

static lv_obj_t *menu_create(void)
{
	lv_obj_t *content;
	lv_obj_t *scr = ui_screen_create(LV_OPA_60, &content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *header = lv_obj_create(content);
	lv_obj_remove_style_all(header);
	lv_obj_set_size(header, LV_PCT(100), UI_HEADER_H);
	lv_obj_t *back = lv_button_create(header);
	lv_obj_set_size(back, 80, 72);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_text_color(back, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text_static(icon, LV_SYMBOL_DOWN);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);
	lv_obj_t *title = lv_label_create(header);
	lv_label_set_text_static(title, "Menu");
	lv_obj_set_style_text_font(title, &ui_font_32, 0);
	lv_obj_center(title);

	lv_obj_t *grid = lv_obj_create(content);
	lv_obj_remove_style_all(grid);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, MENU_PAD, 0);
	lv_obj_set_style_pad_ver(grid, 16, 0);
	lv_obj_set_style_pad_row(grid, 24, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (unsigned i = 0; i < ui_app_count; i++) {
		lv_obj_t *tile = lv_obj_create(grid);
		lv_obj_remove_style_all(tile);
		lv_obj_set_height(tile, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(tile, 12, 0);
		lv_obj_set_style_pad_row(tile, 12, 0);
		lv_obj_set_style_radius(tile, 24, 0);
		lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_TEXT), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
		lv_obj_set_clickable(tile, true);
		lv_obj_set_scrollable(tile, false);
		lv_obj_add_event_cb(tile, open_app_cb, LV_EVENT_CLICKED, (void *)&ui_apps[i]);

		ui_app_icon_create(tile, &ui_apps[i], 112);
		lv_obj_t *label = lv_label_create(tile);
		lv_label_set_text_static(label, ui_apps[i].name);
	}
	menu_grid = grid;
	menu_tiles_fit();
	return scr;
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_open_menu();
}

/* The dock: the most used apps and the menu, on a bar along the bottom */
static const char *const dock_apps[] = { "Phone", "Messages", "Camera" };
#define DOCK_ICON 112 /* a size the icons are rendered at (gen-icons.sh) */

static lv_obj_t *dock_slot(lv_obj_t *dock)
{
	lv_obj_t *slot = lv_obj_create(dock);
	lv_obj_remove_style_all(slot);
	lv_obj_set_size(slot, DOCK_ICON + 16, DOCK_ICON + 16);
	lv_obj_set_style_radius(slot, 36, 0);
	lv_obj_set_style_bg_color(slot, lv_color_hex(UI_COLOR_TEXT), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(slot, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_clickable(slot, true);
	lv_obj_set_scrollable(slot, false);
	return slot;
}

static void dock_create(lv_obj_t *parent)
{
	lv_obj_t *dock = ui_box(parent);
	lv_obj_set_size(dock, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(dock, 20, 0);
	lv_obj_set_style_pad_bottom(dock, 28, 0);
	/* Tinted like the status bar (ui.c) */
	lv_obj_set_style_bg_color(dock, lv_color_hex(UI_COLOR_SHADE), 0);
	lv_obj_set_style_bg_opa(dock, LV_OPA_40, 0);

	for (unsigned i = 0; i < sizeof(dock_apps) / sizeof(dock_apps[0]); i++) {
		const struct ui_app *app = ui_app_find(dock_apps[i]);
		if (!app)
			continue;
		lv_obj_t *slot = dock_slot(dock);
		lv_obj_add_event_cb(slot, open_app_cb, LV_EVENT_CLICKED, (void *)app);
		lv_obj_center(ui_app_icon_create(slot, app, DOCK_ICON));
	}

	/* The menu: a 3 x 3 grid of dots on a translucent tile */
	lv_obj_t *slot = dock_slot(dock);
	lv_obj_add_event_cb(slot, open_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *tile = ui_box(slot);
	lv_obj_set_size(tile, DOCK_ICON, DOCK_ICON);
	lv_obj_center(tile);
	lv_obj_set_style_radius(tile, DOCK_ICON / 4, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(tile, LV_OPA_20, 0);
	for (int i = 0; i < 9; i++) {
		lv_obj_t *dot = ui_box(tile);
		lv_obj_set_size(dot, 18, 18);
		lv_obj_set_style_radius(dot, 6, 0);
		lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_align(dot, LV_ALIGN_CENTER, (i % 3 - 1) * 28, (i / 3 - 1) * 28);
	}
}

void home_create(void)
{
	lv_obj_t *content;
	lv_obj_t *home = ui_screen_create(LV_OPA_TRANSP, &content);

	home_clock = ui_clock_create(content);

	dock_create(content);

	menu_screen = menu_create();
	ui_set_screens(home, menu_screen);
	ui_screen_turns(home, false);
	ui_screen_turns(menu_screen, false);
	/* Created again when the theme changes: one timer and handler for all */
	static lv_timer_t *timer;
	if (!timer) {
		timer = lv_timer_create(home_update, 1000, NULL);
		lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);
	}
	lv_timer_ready(timer);
}
