/*
 * Quick settings, as Android's notification shade: a panel pulled down from the
 * status bar (or opened by tapping it) with brightness and volume sliders, the
 * Wi-Fi network (a card opening its settings), the flashlight, vibration and theme
 * toggles, and the state of the mobile network (the operator), the USB network and
 * the battery in words. Let go past half way or flicked down, it opens; else it goes
 * back. Dragged up, a tap below it or a toggle closes it.
 *
 * It is made at each opening, in the theme's colors of the moment, and deleted
 * once closed. It goes on the top layer under the status bar, over the back
 * and home gestures' strips.
 */

#include <dirent.h>
#include <string.h>

#include "modem.h"
#include "ui.h"
#include "ui_symbols.h"

#define SLOP 12     /* px moved before the panel follows */
#define FLICK 400   /* px/s */
/* Between two readings, not a finger moving: a quick flick goes up to ~300 px
 * down or up, hardly sideways */
#define JUMP_X 160
#define JUMP_Y 480
#define OVER 48     /* px of the panel above the screen: its top corners stay hidden */
#define DIM LV_OPA_40 /* the screen below the panel, open (less while it moves) */
#define TICK_MS 1000
/* Tiles: three to a row upright (the panel's 720 px less its padding and the gaps) */
#define TILE_W ((720 - 2 * 32 - 2 * 24) / 3)

static struct {
	lv_obj_t *bar;               /* the status bar, which takes the pull */
	lv_obj_t *scrim, *panel;     /* while it is shown */
	lv_obj_t *volume, *volume_icon, *mobile, *usb, *battery; /* mobile: NULL without a modem */
	lv_obj_t *mobile_tech;       /* at the end of mobile's row: "4G" while registered */
	lv_obj_t *battery_icon;      /* drawn, in battery's row */
	lv_obj_t *wifi, *wifi_name;  /* the tile, its label: the network's name */
	lv_obj_t *data;              /* the mobile data tile, NULL without */
	lv_obj_t *torch;             /* the first toggle: a row of its own upright */
	lv_timer_t *timer;
	int32_t h;                   /* panel height on screen (without OVER) */
	int32_t pos;                 /* how far it is down, 0 to h */
	int32_t start;               /* pos when the finger went down */
	int32_t y0, y, v;            /* where it started following, last position, speed (px/s) */
	uint32_t t;                  /* of the last position */
	lv_point_t last;             /* previous reading */
	bool dragging;
	bool jumped;                 /* ignored until the finger lifts: not a finger moving */
	bool opening;                /* where the last slide went: open, or closed */
} sh;

static void shade_close(bool anim);
static void wifi_update(void);
static void mobile_update(void);

/* Position */

static void pos_set(int32_t pos)
{
	sh.pos = LV_CLAMP(0, pos, sh.h);
	lv_obj_set_y(sh.panel, sh.pos - sh.h - OVER);
	lv_obj_set_style_bg_opa(sh.scrim, sh.h ? (lv_opa_t)(DIM * sh.pos / sh.h) : 0, 0);
}

static void pos_exec_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	pos_set(v);
}

/* Asynchronously: it can be closed from one of its own events (the theme tile) */
static void destroy(void)
{
	lv_timer_delete(sh.timer);
	lv_obj_delete_async(sh.panel);
	lv_obj_delete_async(sh.scrim);
	sh.timer = NULL;
	sh.panel = sh.scrim = sh.volume = sh.volume_icon = sh.mobile = sh.usb = sh.battery = NULL;
	sh.mobile_tech = NULL;
	sh.battery_icon = NULL;
	sh.wifi = sh.wifi_name = sh.torch = sh.data = NULL;
	sh.dragging = false;
}

static void delete_now(void)
{
	lv_anim_delete(&sh, NULL);
	destroy();
}

static void closed_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	destroy();
}

/* Slides to `target` (0 or h) from where it is, slowing down */
static void slide_to(int32_t target)
{
	printf("phone-ui: shade: slide from %d to %d\n", (int)sh.pos, (int)target);
	lv_anim_delete(&sh, NULL);
	sh.opening = target != 0;
	if (!ui_anim_ms || sh.pos == target) {
		pos_set(target);
		if (!target)
			delete_now();
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &sh);
	lv_anim_set_exec_cb(&a, pos_exec_cb);
	lv_anim_set_values(&a, sh.pos, target);
	lv_anim_set_duration(&a, LV_MAX(ui_anim_ms * LV_ABS(target - sh.pos) / sh.h, 60));
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (!target)
		lv_anim_set_completed_cb(&a, closed_cb);
	lv_anim_start(&a);
}

/* Content */

static void volume_icon_update(int level)
{
	lv_label_set_text_static(sh.volume_icon, level == 0 ? LV_SYMBOL_MUTE :
	                         level <= UI_VOLUME_LEVELS / 2 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
}

static void status_update(void)
{
	int usb = ui_usb_net();
	wifi_update();
	mobile_update();
	if (sh.mobile) {
		ui_set_text(sh.mobile, modem_summary());
		const char *tech = modem_technology_text();
		ui_set_text(sh.mobile_tech, tech);
		lv_obj_set_hidden(sh.mobile_tech, !tech[0]);
	}

	ui_set_text(sh.usb, usb == 2 ? "Internet from the host" :
	            usb == 1 ? "Connected, no internet (just share-internet)" : "Not connected");

	long capacity, uv;
	char status[32], full[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		capacity = -1;
	if (!ui_read_line(UI_BATTERY "/status", status, sizeof(status)))
		status[0] = '\0';
	if (!ui_read_long(UI_BATTERY "/voltage_now", &uv))
		uv = -1;
	ui_battery_icon_update(sh.battery_icon);
	if (capacity < 0)
		ui_set_text(sh.battery, "No battery driver");
	else if (!ui_battery_present() && uv >= 0)
		ui_set_text_fmt(sh.battery, "No usable battery (%.1f V)", uv / 1e6);
	else if (!ui_battery_present())
		ui_set_text(sh.battery, "No usable battery");
	else if (strcmp(status, "Charging") == 0 && ui_battery_time_to_full(full, sizeof(full)))
		ui_set_text_fmt(sh.battery, "%ld%%, %s \xc2\xb7 full in about %s", capacity, status, full);
	else
		ui_set_text_fmt(sh.battery, "%ld%%, %s", capacity, status[0] ? status : "unknown");

	/* The volume keys move it too */
	int level = ui_volume_get();
	if (level >= 0 && lv_slider_get_value(sh.volume) != level &&
	    !lv_obj_has_state(sh.volume, LV_STATE_PRESSED)) {
		lv_slider_set_value(sh.volume, level, LV_ANIM_OFF);
		volume_icon_update(level);
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	status_update();
}

/* The sun: solid from half the brightness, an outline below */
static const char *sun_symbol(int brightness)
{
	return brightness >= 50 ? UI_SYMBOL_SUN : UI_SYMBOL_SUN_REGULAR;
}

static void brightness_cb(lv_event_t *e)
{
	int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
	ui_brightness_set(v);
	lv_label_set_text_static(lv_event_get_user_data(e), sun_symbol(v));
	/* Saving writes and syncs the file: once the finger is up */
	if (lv_event_get_code(e) == LV_EVENT_RELEASED)
		ui_setting_set_int("brightness", v);
}

static void volume_cb(lv_event_t *e)
{
	int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
	ui_volume_set(v);
	volume_icon_update(v);
}

/*
 * Flashlight: the camera's flash LED (SGM3140, "white:flash") in torch mode, a
 * brightness of 1. It stays on once the panel is closed. NULL path: no flash LED.
 */
static const char *torch_path(void)
{
	static char path[300];
	static bool searched;
	if (!searched) {
		searched = true;
		DIR *dir = opendir("/sys/class/leds");
		struct dirent *ent;
		while (dir && !path[0] && (ent = readdir(dir))) {
			size_t n = strlen(ent->d_name);
			if (n > 6 && strcmp(ent->d_name + n - 6, ":flash") == 0)
				snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", ent->d_name);
		}
		if (dir)
			closedir(dir);
	}
	return path[0] ? path : NULL;
}

static bool torch_on(void)
{
	long v;
	return torch_path() && ui_read_long(torch_path(), &v) && v > 0;
}

static void torch_cb(lv_event_t *e)
{
	lv_obj_t *tile = lv_event_get_target_obj(e);
	ui_write_long(torch_path(), !torch_on());
	lv_obj_set_state(tile, LV_STATE_CHECKED, torch_on());
}

static void haptics_cb(lv_event_t *e)
{
	lv_obj_t *tile = lv_event_get_target_obj(e);
	ui_haptics = !ui_haptics;
	ui_setting_set_int("haptics", ui_haptics);
	lv_obj_set_state(tile, LV_STATE_CHECKED, ui_haptics);
	ui_vibrate(UI_VIBRATE_TAP);
}

/* Screens that turn stay as the display is now */
static void rotation_lock_cb(lv_event_t *e)
{
	lv_obj_t *tile = lv_event_get_target_obj(e);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_rotation_lock_set(ui_rotation_lock() < 0);
	lv_obj_set_state(tile, LV_STATE_CHECKED, ui_rotation_lock() >= 0);
}

static void theme_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* The screens are rebuilt in the other theme, this panel goes with them */
	shade_close(false);
	ui_set_light(!ui_light, NULL);
}

static void scrim_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	printf("phone-ui: shade: tap beside the panel\n");
	shade_close(true);
}

/* A row with a symbol and a slider */
static lv_obj_t *slider_row(lv_obj_t *parent, const char *symbol, lv_obj_t **icon)
{
	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_PCT(100), 88);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 32, 0);
	/* The knob is drawn half past the slider's ends, the row would clip it */
	lv_obj_set_style_pad_right(row, 24, 0);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, symbol);
	lv_obj_set_style_text_font(label, &ui_font_32, 0);
	lv_obj_set_width(label, 48);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	if (icon)
		*icon = label;

	return ui_slider_create(row);
}

/* A toggle: a rounded tile, in the accent color while on; cb (if any) gets clicks */
static lv_obj_t *tile_create(lv_obj_t *parent, const char *symbol, const char *text, bool on,
                             lv_event_cb_t cb)
{
	lv_obj_t *tile = lv_button_create(parent);
	lv_obj_set_height(tile, 120);
	lv_obj_set_width(tile, TILE_W);
	lv_obj_set_style_radius(tile, 28, 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_BUTTON), 0);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_ACCENT_PRESSED), LV_STATE_CHECKED | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(tile, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_color(tile, lv_color_white(), LV_STATE_CHECKED);
	/* Dim rather than transparent: opacity would render it into a layer */
	lv_obj_set_style_text_color(tile, lv_color_hex(UI_COLOR_DIM), LV_STATE_DISABLED);
	lv_obj_set_style_text_color(tile, lv_color_hex(UI_COLOR_DIM), LV_STATE_CHECKED | LV_STATE_DISABLED);
	lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_BUTTON), LV_STATE_CHECKED | LV_STATE_DISABLED);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(tile, 8, 0);
	lv_obj_set_state(tile, LV_STATE_CHECKED, on);
	if (cb)
		lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *icon = lv_label_create(tile);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	return tile;
}

/* A tap turns Wi-Fi on or off, a long press opens its settings */
static void wifi_toggle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	wifi_set_enabled(!wifi_enabled());
	wifi_update();
}

static void wifi_settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* Settings are not opened over the lock screen */
	if (ui_locked())
		return;
	shade_close(true);
	wifi_open();
}

/* Wi-Fi: a toggle, labelled with the network's name while connected */
static void wifi_tile(lv_obj_t *parent)
{
	sh.wifi = tile_create(parent, LV_SYMBOL_WIFI, "Wi-Fi", false, NULL);
	lv_obj_add_event_cb(sh.wifi, wifi_toggle_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(sh.wifi, wifi_settings_cb, LV_EVENT_LONG_PRESSED, NULL);
	sh.wifi_name = lv_obj_get_child(sh.wifi, 1);
	ui_set_text(sh.wifi_name, "Wi-Fi");
	lv_obj_set_width(sh.wifi_name, LV_PCT(100));
	lv_label_set_long_mode(sh.wifi_name, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_style_text_align(sh.wifi_name, LV_TEXT_ALIGN_CENTER, 0);
}

static void wifi_update(void)
{
	char ssid[33];
	int state = wifi_network(ssid, sizeof(ssid));
	ui_set_text(sh.wifi_name, state > 0 && ssid[0] ? ssid : "Wi-Fi");
	lv_obj_set_state(sh.wifi, LV_STATE_CHECKED, state >= 0 && wifi_enabled());
	lv_obj_set_state(sh.wifi, LV_STATE_DISABLED, state < 0);
}

/* Upright, the Wi-Fi tile has a row of its own above the toggles; sideways, all go on one */
static void tiles_layout(void)
{
	lv_display_t *d = lv_display_get_default();
	bool wide = lv_display_get_horizontal_resolution(d) > lv_display_get_vertical_resolution(d);
	lv_obj_set_flag(sh.torch, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK, !wide);
}

/* A line of state: a dim symbol (NULL: the battery icon, in sh.battery_icon), then the
 * text (returned, for ui_set_text()) */
static lv_obj_t *state_row(lv_obj_t *parent, const char *symbol)
{
	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 32, 0);

	lv_obj_t *icon;
	if (symbol) {
		icon = lv_label_create(row);
		lv_label_set_text_static(icon, symbol);
		lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
	} else {
		icon = ui_box(row); /* the battery centered in the symbols' width */
		lv_obj_set_height(icon, 18);
		sh.battery_icon = ui_battery_icon_create(icon, 36, 18);
		lv_obj_center(sh.battery_icon);
	}
	lv_obj_set_width(icon, 48);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *text = lv_label_create(row);
	ui_set_text(text, "");
	lv_obj_set_flex_grow(text, 1);
	lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
	return text;
}

/* Mobile data: a toggle beside Wi-Fi */
static void mobile_update(void)
{
	if (sh.data)
		lv_obj_set_state(sh.data, LV_STATE_CHECKED, modem_data_enabled());
}

static void mobile_toggle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	modem_data_set_enabled(!modem_data_enabled());
	mobile_update();
}

static void mobile_tile(lv_obj_t *parent)
{
	sh.data = tile_create(parent, UI_SYMBOL_TOWER_BROADCAST, "Mobile data", modem_data_enabled(),
	                      mobile_toggle_cb);
}

/* Room around the tiles' row for a pressed tile, which grows (the theme's style) */
static void tiles_ext_cb(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, 8);
}

static void drag_cb(lv_event_t *e);

static void drag_events(lv_obj_t *obj)
{
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void create(void)
{
	lv_obj_t *top = lv_layer_top();

	/* Taps beside the panel close it, and do not reach the screen */
	sh.scrim = ui_box(top);
	lv_obj_set_size(sh.scrim, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(sh.scrim, lv_color_black(), 0); /* dims the screen: pos_set() */
	lv_obj_set_clickable(sh.scrim, true);
	lv_obj_add_event_cb(sh.scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *p = sh.panel = ui_box(top);
	lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(p, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(p, OVER, 0);
	lv_obj_set_style_pad_top(p, OVER + UI_STATUS_H + 16, 0);
	lv_obj_set_style_pad_hor(p, 32, 0);
	lv_obj_set_style_pad_bottom(p, 20, 0);
	lv_obj_set_style_pad_row(p, 24, 0);
	lv_obj_set_style_text_color(p, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(p, &ui_font_24, 0);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	/* Dragged up from anywhere but its controls */
	lv_obj_set_clickable(p, true);
	drag_events(p);

	int b = ui_brightness_get();
	if (b >= 0) {
		lv_obj_t *sun;
		lv_obj_t *slider = slider_row(p, sun_symbol(b), &sun);
		lv_slider_set_range(slider, UI_BRIGHTNESS_MIN, 100);
		lv_slider_set_value(slider, b, LV_ANIM_OFF);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, sun);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, sun);
	}

	sh.volume = slider_row(p, LV_SYMBOL_VOLUME_MAX, &sh.volume_icon);
	lv_slider_set_range(sh.volume, 0, UI_VOLUME_LEVELS);
	int level = LV_MAX(ui_volume_get(), 0);
	lv_slider_set_value(sh.volume, level, LV_ANIM_OFF);
	volume_icon_update(level);
	lv_obj_add_event_cb(sh.volume, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *tiles = ui_box(p);
	lv_obj_set_size(tiles, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(tiles, 24, 0);
	lv_obj_set_style_pad_column(tiles, 24, 0);
	lv_obj_set_style_pad_ver(tiles, 8, 0);
	/* A pressed tile grows (the theme's style): not cut at the row's edges */
	lv_obj_set_overflow_visible(tiles, true);
	lv_obj_add_event_cb(tiles, tiles_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(tiles);
	wifi_tile(tiles);
	if (modem_data_available())
		mobile_tile(tiles);
	lv_obj_t *torch = sh.torch = tile_create(tiles, LV_SYMBOL_CHARGE, "Flashlight", torch_on(), torch_cb);
	tiles_layout();
	lv_obj_set_state(torch, LV_STATE_DISABLED, !torch_path());
	tile_create(tiles, LV_SYMBOL_BELL, "Vibration", ui_haptics, haptics_cb);
	/* Switching rebuilds the screens: an open app would be closed */
	lv_obj_t *theme = tile_create(tiles, LV_SYMBOL_EYE_OPEN, "Light theme", ui_light, theme_cb);
	lv_obj_set_state(theme, LV_STATE_DISABLED, !ui_at_home() || ui_locked());
	tile_create(tiles, UI_SYMBOL_LOCK, "Rotation lock", ui_rotation_lock() >= 0, rotation_lock_cb);

	/* The mobile network: the operator, or what is wrong (the status bar's in full);
	 * the technology at the end, dim (the status bar has it only while data is up) */
	if (modem_state() != MODEM_NONE) {
		sh.mobile = state_row(p, UI_SYMBOL_TOWER_CELL);
		sh.mobile_tech = lv_label_create(lv_obj_get_parent(sh.mobile));
		ui_set_text(sh.mobile_tech, "");
		lv_obj_set_style_text_color(sh.mobile_tech, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_hidden(sh.mobile_tech, true);
	}
	sh.usb = state_row(p, LV_SYMBOL_USB);
	sh.battery = state_row(p, NULL);

	/* The handle */
	lv_obj_t *handle = ui_box(p);
	lv_obj_set_size(handle, 72, 8);
	lv_obj_set_style_radius(handle, 4, 0);
	lv_obj_set_style_bg_color(handle, lv_color_hex(UI_COLOR_TRACK), 0);
	lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
	lv_obj_set_style_margin_top(handle, 8, 0);

	status_update();
	sh.timer = lv_timer_create(tick_cb, TICK_MS, NULL);

	lv_obj_update_layout(p);
	sh.h = lv_obj_get_height(p) - OVER;
	pos_set(0);
	/* Under the status bar, over everything else */
	lv_obj_move_foreground(sh.bar);
}

static void shade_close(bool anim)
{
	if (!sh.panel)
		return;
	if (anim)
		slide_to(0);
	else
		delete_now();
}

/* The display turned (turn.c), also while it is open, e.g. upright again behind the
 * lock screen after waking: its width follows the display's, its height is measured
 * again; a slide on its way ends where it was going */
static void resolution_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!sh.panel)
		return;
	bool sliding = lv_anim_delete(&sh, NULL);
	if (sliding && !sh.opening) {
		delete_now();
		return;
	}
	bool open = sliding || (!sh.dragging && sh.pos == sh.h);
	tiles_layout();
	lv_obj_update_layout(sh.panel);
	sh.h = lv_obj_get_height(sh.panel) - OVER;
	pos_set(open ? sh.h : sh.pos);
}

/* The pull, on the status bar and on the panel */

static void drag_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	if (code == LV_EVENT_PRESSED) {
		sh.y0 = sh.y = p.y;
		sh.last = p;
		sh.v = 0;
		sh.t = lv_tick_get();
		sh.start = sh.pos;
		sh.dragging = sh.jumped = false;
		printf("phone-ui: shade: pressed at %d,%d on the %s, pos %d of %d\n", (int)p.x, (int)p.y,
		       lv_event_get_current_target_obj(e) == sh.bar ? "bar" : "panel", (int)sh.pos,
		       (int)sh.h);
		return;
	}
	if (sh.jumped) {
		if (code != LV_EVENT_PRESSING)
			printf("phone-ui: shade: %s after a jump, ignored\n",
			       code == LV_EVENT_RELEASED ? "released" : "press lost");
		return; /* until the finger lifts */
	}
	if (code == LV_EVENT_PRESSING) {
		/* The pointer moved to another finger (or a wrong reading) rather than a
		 * finger moving: the pull is called off, the panel goes back */
		if (LV_ABS(p.x - sh.last.x) > JUMP_X || LV_ABS(p.y - sh.last.y) > JUMP_Y) {
			printf("phone-ui: shade: the pointer jumped from %d,%d to %d,%d, ignored\n",
			       (int)sh.last.x, (int)sh.last.y, (int)p.x, (int)p.y);
			sh.jumped = true;
			if (sh.dragging) {
				sh.dragging = false;
				slide_to(sh.start ? sh.h : 0);
			}
			return;
		}
		sh.last = p;
		if (!sh.dragging) {
			if (LV_ABS(p.y - sh.y0) < SLOP)
				return;
			if (!sh.panel)
				create();
			lv_anim_delete(&sh, NULL);
			sh.start = sh.pos;
			sh.dragging = true;
			sh.y0 = p.y;
			printf("phone-ui: shade: following from y %d, pos %d of %d\n", (int)p.y,
			       (int)sh.pos, (int)sh.h);
		}
		uint32_t dt = lv_tick_elaps(sh.t);
		if (dt) {
			sh.v = (sh.v + (p.y - sh.y) * 1000 / (int32_t)dt) / 2;
			sh.y = p.y;
			sh.t = lv_tick_get();
		}
		pos_set(sh.start + p.y - sh.y0);
		printf("phone-ui: shade: at y %d, dt %u ms, v %d px/s, pos %d\n", (int)p.y, (unsigned)dt,
		       (int)sh.v, (int)sh.pos);
		return;
	}
	if (code == LV_EVENT_PRESS_LOST && !sh.dragging) {
		printf("phone-ui: shade: press lost at %d,%d before following\n", (int)p.x, (int)p.y);
		return;
	}
	if (!sh.dragging) {
		/* A tap: the status bar toggles it, the panel's background does nothing */
		if (lv_event_get_current_target_obj(e) != sh.bar)
			return;
		/* Lifted on the bar too: a press there that ends elsewhere is not a tap */
		if (p.y >= UI_STATUS_H) {
			printf("phone-ui: shade: pressed on the bar, released at %d,%d: not a tap\n",
			       (int)p.x, (int)p.y);
			return;
		}
		printf("phone-ui: shade: tap on the bar at %d,%d\n", (int)p.x, (int)p.y);
		if (!sh.panel)
			create();
		slide_to(sh.pos == sh.h ? 0 : sh.h);
		return;
	}
	sh.dragging = false;
	uint32_t still = lv_tick_elaps(sh.t);
	if (still > 100) /* held still before letting go */
		sh.v = 0;
	bool open = sh.v > FLICK || (sh.v > -FLICK && sh.pos > sh.h / 2);
	printf("phone-ui: shade: %s at %d,%d, %u ms after the last move, v %d px/s, pos %d of %d: %s\n",
	       code == LV_EVENT_RELEASED ? "released" : "press lost", (int)p.x, (int)p.y,
	       (unsigned)still, (int)sh.v, (int)sh.pos, (int)sh.h, open ? "open" : "close");
	slide_to(open ? sh.h : 0);
}

void ui_shade_init(lv_obj_t *status_bar)
{
	sh.bar = status_bar;
	lv_obj_set_clickable(status_bar, true);
	lv_obj_set_scrollable(status_bar, false);
	lv_obj_set_gesture_bubble(status_bar, false);
	drag_events(status_bar);
	lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED,
	                        NULL);
}
