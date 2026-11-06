/*
 * Home screen (clock, date, a dock with apps and the menu) and the menu with the apps.
 *
 * Both turn sideways with the phone (turn.c); the menu has 8 tiles to a row then.
 * Apps open upright.
 */

#include <time.h>

#include "data.h"
#include "i18n.h"
#include "ui.h"

LV_FONT_DECLARE(ui_font_clock) /* ui_font_clock.c, gen-fonts.sh: digits and colon only */

/* Their names: English, how apps are found (--open, ui_app_find()); shown translated */
static const struct ui_app builtin_apps[] = {
	{ N_("Phone"),      LV_SYMBOL_CALL,      0x34c759, phone_open,       UI_ESSENTIALS },
	{ N_("Messages"),   LV_SYMBOL_ENVELOPE,  0x0a84ff, messages_open,    UI_ESSENTIALS },
	{ N_("Contacts"),   LV_SYMBOL_BARS,      0xff9f0a, contacts_open,    UI_ESSENTIALS },
	{ N_("Camera"),     LV_SYMBOL_EYE_OPEN,  0x8e8e93, camera_open,      UI_ESSENTIALS },
	{ N_("Gallery"),    LV_SYMBOL_IMAGE,     0xff375f, gallery_open,     UI_ESSENTIALS },
	{ N_("Music"),      LV_SYMBOL_AUDIO,     0xbf5af2, music_open,       UI_MEDIA },
	{ N_("Clock"),      LV_SYMBOL_BELL,      0xff453a, clock_open,       UI_ESSENTIALS },
	{ N_("Calendar"),   LV_SYMBOL_LIST,      0xe0592a, calendar_open,    UI_ESSENTIALS },
	{ N_("Calculator"), LV_SYMBOL_PLUS,      0x5e5ce6, calculator_open,  UI_TOOLS },
	{ N_("Notes"),      LV_SYMBOL_EDIT,      0xffd60a, notes_open,       UI_TOOLS },
	{ N_("Recorder"),   LV_SYMBOL_AUDIO,     0xff453a, recorder_open,    UI_TOOLS },
	{ N_("Files"),      LV_SYMBOL_DIRECTORY, 0x64d2ff, files_open,       UI_TOOLS },
	{ N_("Maps"),       LV_SYMBOL_GPS,       0x30d158, maps_open,        UI_MEDIA },
	{ N_("Weather"),    LV_SYMBOL_CHARGE,    0x409cff, weather_open,     UI_MEDIA },
	{ N_("Compass"),    LV_SYMBOL_UP,        0xac8e68, compass_open,     UI_TOOLS },
	{ N_("Web Lite"),   LV_SYMBOL_FILE,      0x1aa6a6, browser_open,     UI_MEDIA },
	{ N_("Books"),      LV_SYMBOL_FILE,      0xff9500, books_open,       UI_MEDIA },
	{ N_("Assistant"),  LV_SYMBOL_KEYBOARD,  0x4d6bfe, assistant_open,   UI_MEDIA },
	{ N_("Snake"),      LV_SYMBOL_PLAY,      0x66d4cf, snake_open,       UI_GAMES },
	{ N_("Settings"), LV_SYMBOL_SETTINGS,  0x636366, settings_open,    UI_TOOLS }, /* last: external.c */
};
const struct ui_app *ui_apps = builtin_apps;
unsigned ui_app_count = sizeof(builtin_apps) / sizeof(builtin_apps[0]);

static lv_obj_t *home_clock;

/* Badges: a count in a red pill over the top right corner of an app's icon, the
 * notifications' (notify.c) of Phone (missed calls) and Messages (unread), in the dock
 * and the menu; hidden at 0 */
#define BADGES 4
#define BADGE_OUT 8 /* px past the icon's edges: within the dock's slot, the menu's tile */

static struct badge {
	lv_obj_t *label;
	unsigned (*count)(void);
	unsigned shown;
} badges[BADGES];
static unsigned badge_count;

static void badge_update(struct badge *b)
{
	unsigned n = b->count();
	if (n == b->shown)
		return;
	b->shown = n;
	lv_obj_set_hidden(b->label, !n);
	if (n > 99)
		ui_set_text(b->label, "99+"); /* NON-NLS */
	else if (n)
		ui_set_text_fmt(b->label, "%u", n);
}

/* Children of an object with overflow visible are drawn only as far as its extra draw
 * size reaches past it: the icon's, as far as its badge */
static void badge_ext_cb(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, BADGE_OUT);
}

static void badge_add(lv_obj_t *icon, const struct ui_app *app)
{
	unsigned (*count)(void) = app->open == phone_open ? notification_missed_calls :
	                          app->open == messages_open ? notification_messages : NULL;
	if (!count || badge_count == BADGES)
		return;
	lv_obj_set_overflow_visible(icon, true);
	lv_obj_add_event_cb(icon, badge_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(icon);
	lv_obj_t *label = lv_label_create(icon);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_set_style_bg_color(label, lv_color_hex(UI_COLOR_RED), 0);
	lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(label, 10, 0);
	lv_obj_set_style_pad_ver(label, 4, 0);
	lv_obj_set_style_min_width(label, 36, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, BADGE_OUT, -BADGE_OUT);
	lv_obj_set_hidden(label, true);
	struct badge *b = &badges[badge_count++];
	*b = (struct badge){ label, count, 0 };
	badge_update(b);
}

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
	ui_set_text_fmt(lv_obj_get_child(clock, 1), "%s %d %s", ui_day_name(tm.tm_wday, false), tm.tm_mday,
	                ui_month_name(tm.tm_mon, false));
}

static void home_update(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	ui_clock_update(home_clock);
	ui_status_update();
	for (unsigned i = 0; i < badge_count; i++)
		badge_update(&badges[i]);
}

/* Apps that are not done yet */
static void placeholder_open(const struct ui_app *app)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_(app->name), NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, 32, 0);
	lv_obj_set_style_pad_bottom(body, 96, 0);

	ui_app_icon_create(body, app, 180);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, _("Coming soon"));
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

static lv_obj_t *menu_screen, *menu_grid;

/*
 * The menu dragged up from anywhere on the home screen, the dock too, or down from
 * the menu (with its grid scrolled to the top): it follows the finger. Let go past a
 * third of the way or flicked, it goes, else it falls back.
 */

#define DRAG_SLOP 12   /* px moved before it follows */
#define DRAG_FLICK 400 /* px/s */

static struct {
	bool open;     /* up from the home screen, else down from the menu */
	bool can;      /* the menu's grid was at its top on the press (or opening) */
	int32_t y0;    /* where it started following */
	int32_t y, v;  /* last position, speed (px/s, the way it goes) */
	uint32_t t;    /* of the last position */
	bool dragging;
	bool dragged;  /* since the press: no click then */
} drag;

static void drag_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		drag.open = lv_obj_get_screen(lv_event_get_current_target(e)) != menu_screen;
		drag.y0 = drag.y = p.y;
		drag.v = 0;
		drag.t = lv_tick_get();
		drag.dragging = drag.dragged = false;
		/* The grid at its top: pulled down, the menu goes rather than overscroll. Else
		 * it scrolls, and the menu stays to the release: decided here, as LVGL only
		 * finds the scroll after PRESSING, which a quick flick can get past the slop */
		drag.can = drag.open || lv_obj_get_scroll_y(menu_grid) <= 0;
		if (!drag.open)
			lv_obj_set_scroll_dir(menu_grid, drag.can ? LV_DIR_BOTTOM : LV_DIR_VER);
		return;
	}
	int32_t dir = drag.open ? 1 : -1; /* up, down */
	if (code == LV_EVENT_PRESSING) {
		if (!drag.dragging) {
			if (!drag.can || drag.dragged || lv_indev_get_scroll_obj(indev) ||
			    dir * (drag.y0 - p.y) < DRAG_SLOP)
				return;
			/* Not pressed in the screen as it slides */
			lv_obj_remove_state(lv_event_get_target(e), LV_STATE_PRESSED);
			drag.dragged = true;
			if (!ui_menu_drag_begin(drag.open))
				return;
			drag.dragging = true;
			drag.y0 = p.y;
		}
		uint32_t dt = lv_tick_elaps(drag.t);
		if (dt) {
			drag.v = (drag.v + dir * (drag.y - p.y) * 1000 / (int32_t)dt) / 2;
			drag.y = p.y;
			drag.t = lv_tick_get();
		}
		ui_menu_drag(dir * (drag.y0 - p.y));
		return;
	}
	if (!drag.dragging)
		return;
	drag.dragging = false;
	if (lv_tick_elaps(drag.t) > 100) /* held still before letting go */
		drag.v = 0;
	int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
	ui_menu_drag_end(drag.v > DRAG_FLICK ||
	                 (drag.v > -DRAG_FLICK && dir * (drag.y0 - drag.y) > h / 3));
}

static void drag_add(lv_obj_t *obj)
{
	lv_obj_set_clickable(obj, true);
	lv_obj_set_scrollable(obj, false);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* An app's tile or dock slot tapped, not dragged */
static void open_app_cb(lv_event_t *e)
{
	if (drag.dragged)
		return;
	ui_upright(); /* the app's screens are made for the display's size */
	ui_app_open(lv_event_get_user_data(e));
}

#define MENU_PAD 12  /* on each side of the grid */
#define MENU_ICON 112
#define MENU_HEADER_H 56

/* The menu's tiles: 4 to a row upright, 8 sideways; the sections' headings across,
 * from the left edge of the icons */
static void menu_tiles_fit(void)
{
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int32_t cols = w > lv_display_get_vertical_resolution(lv_display_get_default()) ? 8 : 4;
	int32_t tile_w = (w - 2 * MENU_PAD) / cols;
	for (uint32_t i = 0; menu_grid && i < lv_obj_get_child_count(menu_grid); i++) {
		lv_obj_t *child = lv_obj_get_child(menu_grid, i);
		if (lv_obj_check_type(child, &lv_label_class)) {
			lv_obj_set_width(child, LV_PCT(100));
			lv_obj_set_style_pad_left(child, (tile_w - MENU_ICON) / 2, 0);
		} else {
			lv_obj_set_width(child, tile_w);
		}
	}
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
	drag_add(content); /* the header, the grid and its tiles pass their presses on */

	/* No title: a handle to pull it down by, and the arrow that closes it */
	lv_obj_t *header = lv_obj_create(content);
	lv_obj_remove_style_all(header);
	lv_obj_set_size(header, LV_PCT(100), MENU_HEADER_H);
	lv_obj_set_clickable(header, false);
	lv_obj_t *handle = lv_obj_create(header);
	lv_obj_remove_style_all(handle);
	lv_obj_set_size(handle, 64, 6);
	lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(handle, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(handle, LV_OPA_40, 0);
	lv_obj_set_clickable(handle, false);
	lv_obj_center(handle);
	lv_obj_t *back = lv_button_create(header);
	lv_obj_set_size(back, 72, MENU_HEADER_H);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_text_color(back, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text_static(icon, LV_SYMBOL_DOWN);
	lv_obj_set_style_text_font(icon, &ui_font_24, 0);
	lv_obj_center(icon);

	lv_obj_t *grid = lv_obj_create(content);
	lv_obj_remove_style_all(grid);
	lv_obj_set_width(grid, LV_PCT(100));
	lv_obj_set_flex_grow(grid, 1);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_hor(grid, MENU_PAD, 0);
	lv_obj_set_style_pad_top(grid, 4, 0);
	lv_obj_set_style_pad_bottom(grid, 16, 0);
	lv_obj_set_style_pad_row(grid, 24, 0);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_event_bubble(grid, true);

	static const char *const sections[UI_CATEGORIES] = {
		N_("Essentials"), N_("Tools"), N_("Internet & media"), N_("Games"),
	};
	for (unsigned c = 0; c < UI_CATEGORIES; c++)
	for (unsigned i = 0, n = 0; i < ui_app_count; i++) {
		if (ui_apps[i].category != c)
			continue;
		if (n++ == 0) {
			/* Before the section's first app, on a line of its own (menu_tiles_fit()) */
			lv_obj_t *heading = ui_dim_label(grid, _(sections[c]), &ui_font_20);
			lv_obj_set_style_pad_top(heading, lv_obj_get_child_count(grid) > 1 ? 24 : 0, 0);
		}
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
		lv_obj_set_event_bubble(tile, true);
		lv_obj_add_event_cb(tile, open_app_cb, LV_EVENT_CLICKED, (void *)&ui_apps[i]);

		badge_add(ui_app_icon_create(tile, &ui_apps[i], MENU_ICON), &ui_apps[i]);
		lv_obj_t *label = lv_label_create(tile);
		lv_label_set_text_static(label, _(ui_apps[i].name));
	}
	menu_grid = grid;
	menu_tiles_fit();
	return scr;
}

static void open_menu_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!drag.dragged)
		ui_open_menu();
}

/* The dock: the most used apps and the menu, on a bar along the bottom */
static const char *const dock_apps[] = { "Phone", "Messages", "Camera" }; /* NON-NLS: names */
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
	lv_obj_set_event_bubble(slot, true); /* presses to the drag (drag_cb) */
	lv_obj_set_scrollable(slot, false);
	return slot;
}

static void dock_create(lv_obj_t *parent)
{
	lv_obj_t *dock = ui_box(parent);
	lv_obj_set_event_bubble(dock, true); /* its slots' presses on to the drag (drag_cb) */
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
		lv_obj_t *icon = ui_app_icon_create(slot, app, DOCK_ICON);
		lv_obj_center(icon);
		badge_add(icon, app);
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
	badge_count = 0; /* made again (the theme changed): the old ones go with the screens */

	drag_add(content);

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
