/*
 * A monitor's own screens (prototype): main.c makes a display of the monitor's size
 * once one is connected over USB-C and shows it there while the phone is at home,
 * mirroring the phone otherwise. Its home has the clock and a dock (apps, and Apps for
 * the menu), its app menu every app in the phone menu's sections. It follows the phone,
 * its menu open while the phone's is; a mouse points there (mouse.c), an app picked
 * opening on the phone.
 */

#include "i18n.h"
#include "ui/ui_internal.h"

#define TILE_W 200
#define ICON 112 /* a size the icons are rendered at (gen-icons.sh) */

static struct {
	lv_display_t *disp;
	lv_obj_t *home, *menu, *clock;
	lv_timer_t *follow, *tick;
} dt;

/* The dock's apps, before Apps */
static const char *const dock_apps[] = { "Phone", "Messages", "Camera", "Notes", "Gallery", "Music" }; /* NON-NLS: names */

static void open_app_cb(lv_event_t *e)
{
	const struct ui_app *app = lv_event_get_user_data(e);
	ui_app_open(app);
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_open_menu();
}

/* An icon over its name, clickable */
static lv_obj_t *tile_create(lv_obj_t *parent)
{
	lv_obj_t *tile = lv_obj_create(parent);
	lv_obj_remove_style_all(tile);
	lv_obj_set_size(tile, TILE_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_ver(tile, 16, 0);
	lv_obj_set_style_pad_row(tile, 12, 0);
	lv_obj_set_style_radius(tile, 24, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_TEXT), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_clickable(tile, true);
	lv_obj_set_scrollable(tile, false);
	return tile;
}

static void tile_label(lv_obj_t *tile, const char *title)
{
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text(label, title); /* dots need a copy */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(label, TILE_W - 16);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void app_tile(lv_obj_t *parent, const struct ui_app *app)
{
	lv_obj_t *tile = tile_create(parent);
	lv_obj_add_event_cb(tile, open_app_cb, LV_EVENT_CLICKED, (void *)app);
	ui_app_icon_create(tile, app, ICON);
	tile_label(tile, ui_app_title(app));
}

/* The screen's background: dark, the accent rising from the bottom */
static lv_obj_t *screen_create(void)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0e14), 0);
	lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x123a6e), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);
	lv_obj_set_scrollable(scr, false);
	return scr;
}

static lv_obj_t *desktop_home_create(void)
{
	lv_obj_t *scr = screen_create();
	dt.clock = ui_clock_create(scr);

	lv_obj_t *dock = lv_obj_create(scr);
	lv_obj_remove_style_all(dock);
	lv_obj_set_size(dock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_all(dock, 16, 0);
	lv_obj_set_style_pad_column(dock, 8, 0);
	lv_obj_set_style_radius(dock, 40, 0);
	lv_obj_set_style_bg_color(dock, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(dock, LV_OPA_10, 0);
	lv_obj_set_scrollable(dock, false);
	lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -48);

	for (unsigned i = 0; i < sizeof(dock_apps) / sizeof(dock_apps[0]); i++) {
		const struct ui_app *app = ui_app_find(dock_apps[i]);
		if (app)
			app_tile(dock, app);
	}
	lv_obj_t *apps = tile_create(dock);
	lv_obj_add_event_cb(apps, open_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = ui_box(apps);
	lv_obj_set_size(icon, ICON, ICON);
	lv_obj_set_style_radius(icon, 28, 0);
	lv_obj_set_style_bg_color(icon, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
	lv_obj_t *symbol = lv_label_create(icon);
	lv_label_set_text_static(symbol, LV_SYMBOL_LIST);
	lv_obj_set_style_text_font(symbol, &ui_font_48, 0);
	lv_obj_center(symbol);
	tile_label(apps, _("Apps"));
	return scr;
}

static lv_obj_t *desktop_menu_create(void)
{
	lv_obj_t *scr = screen_create();
	lv_obj_t *grid = lv_obj_create(scr);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, 160, 0);
	lv_obj_set_style_pad_ver(grid, 64, 0);
	lv_obj_set_style_pad_row(grid, 16, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);

	static const char *const sections[UI_CATEGORIES] = {
		N_("Essentials"), N_("Tools"), N_("Internet & media"), N_("Games"),
	};
	for (unsigned c = 0; c < UI_CATEGORIES; c++)
	for (unsigned i = 0, n = 0; i < ui_app_count; i++) {
		if (ui_apps[i].category != c)
			continue;
		if (n++ == 0) {
			/* On a line of its own before the section's first app */
			lv_obj_t *heading = ui_dim_label(grid, _(sections[c]), UI_FONT_TITLE);
			lv_obj_set_width(heading, LV_PCT(100));
			lv_obj_set_style_pad_top(heading, c ? 24 : 0, 0);
		}
		app_tile(grid, &ui_apps[i]);
	}
	return scr;
}

/* The menu while the phone's is open, else home */
static void follow_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	lv_obj_t *want = nav_menu_on_top() ? dt.menu : dt.home;
	if (lv_display_get_screen_active(dt.disp) != want)
		lv_screen_load(want);
}

static void tick_cb(lv_timer_t *timer)
{
	ui_clock_align(timer);
	ui_clock_update(dt.clock);
}

void desktop_start(lv_display_t *disp)
{
	desktop_stop();
	lv_display_t *phone = lv_display_get_default();
	lv_display_set_theme(disp, lv_display_get_theme(phone));
	/* Its screens made on it */
	lv_display_set_default(disp);
	dt.disp = disp;
	dt.home = desktop_home_create();
	dt.menu = desktop_menu_create();
	lv_screen_load(dt.home);
	lv_display_set_default(phone);
	dt.follow = lv_timer_create(follow_cb, 50, NULL);
	dt.tick = lv_timer_create(tick_cb, 1000, NULL);
	ui_clock_align(dt.tick);
	mouse_start(disp);
	follow_cb(NULL);
}

void desktop_stop(void)
{
	if (!dt.disp)
		return;
	mouse_stop();
	lv_timer_delete(dt.follow);
	lv_timer_delete(dt.tick);
	lv_obj_delete(dt.menu);
	lv_obj_delete(dt.home);
	dt = (typeof(dt)){ 0 };
}

bool desktop_shown(void)
{
	return dt.disp && ui_at_home();
}
