/*
 * Home screen (clock, date, Menu button) and the menu with the apps.
 */

#include <time.h>

#include "ui.h"

const struct ui_app ui_apps[] = {
	{ "Phone",      LV_SYMBOL_CALL,      0x34c759, phone_open },
	{ "Messages",   LV_SYMBOL_ENVELOPE,  0x0a84ff, messages_open },
	{ "Contacts",   LV_SYMBOL_BARS,      0xff9f0a, contacts_open },
	{ "Camera",     LV_SYMBOL_EYE_OPEN,  0x8e8e93, camera_open },
	{ "Gallery",    LV_SYMBOL_IMAGE,     0xff375f, NULL },
	{ "Music",      LV_SYMBOL_AUDIO,     0xbf5af2, NULL },
	{ "Clock",      LV_SYMBOL_BELL,      0xff453a, clock_open },
	{ "Calculator", LV_SYMBOL_PLUS,      0x5e5ce6, calculator_open },
	{ "Notes",      LV_SYMBOL_EDIT,      0xffd60a, notes_open },
	{ "Files",      LV_SYMBOL_DIRECTORY, 0x64d2ff, files_open },
	{ "Maps",       LV_SYMBOL_GPS,       0x30d158, NULL },
	{ "Settings",   LV_SYMBOL_SETTINGS,  0x636366, settings_open },
};
const unsigned ui_app_count = sizeof(ui_apps) / sizeof(ui_apps[0]);

static lv_obj_t *home_time, *home_date;

static void home_update(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);

	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(home_time, buf);
	strftime(buf, sizeof(buf), "%A %-d %B", &tm);
	ui_set_text(home_date, buf);
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
	else
		placeholder_open(app);
}

static void open_app_cb(lv_event_t *e)
{
	ui_app_open(lv_event_get_user_data(e));
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
	lv_obj_set_style_pad_hor(grid, 12, 0);
	lv_obj_set_style_pad_ver(grid, 16, 0);
	lv_obj_set_style_pad_row(grid, 24, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	for (unsigned i = 0; i < ui_app_count; i++) {
		lv_obj_t *tile = lv_obj_create(grid);
		lv_obj_remove_style_all(tile);
		lv_obj_set_size(tile, LV_PCT(33), LV_SIZE_CONTENT);
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

		ui_app_icon_create(tile, &ui_apps[i], 128);
		lv_obj_t *label = lv_label_create(tile);
		lv_label_set_text_static(label, ui_apps[i].name);
	}
	return scr;
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_open_menu();
}

void home_create(void)
{
	lv_obj_t *content;
	lv_obj_t *home = ui_screen_create(LV_OPA_TRANSP, &content);

	home_time = lv_label_create(content);
	ui_set_text(home_time, "");
	lv_obj_set_style_text_font(home_time, &ui_font_48, 0);
	lv_obj_align(home_time, LV_ALIGN_TOP_MID, 0, 160);

	home_date = lv_label_create(content);
	ui_set_text(home_date, "");
	lv_obj_set_style_text_font(home_date, &ui_font_26, 0);
	lv_obj_align(home_date, LV_ALIGN_TOP_MID, 0, 270);

	lv_obj_t *button = lv_button_create(content);
	lv_obj_set_size(button, 240, 88);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -80);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_20, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_opa(button, LV_OPA_40, 0);
	lv_obj_set_style_border_width(button, 2, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, open_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, LV_SYMBOL_LIST "  Menu");
	lv_obj_set_style_text_font(label, &ui_font_32, 0);
	lv_obj_center(label);

	ui_set_screens(home, menu_create());
	/* Created again when the theme changes: one timer for all */
	static lv_timer_t *timer;
	if (!timer)
		timer = lv_timer_create(home_update, 1000, NULL);
	lv_timer_ready(timer);
}
