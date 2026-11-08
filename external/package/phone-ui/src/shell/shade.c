/*
 * Quick settings, as Android's notification shade: a panel pulled down from the
 * status bar (or opened by tapping it), which shows the mobile network's name meanwhile
 * (ui_status_shade()). At its top the time and the date, Power (the power menu) and
 * Settings; then the Wi-Fi and Bluetooth (a long press opens their settings), mobile data,
 * flashlight and rotation lock toggles, the brightness and volume sliders (cards filled
 * to their level, their icon inside), the dark theme's toggle beside the brightness and
 * vibration's beside the volume, then the state of the USB network (while a computer is
 * plugged in, its arrow unfolds what the USB is used for: Network only or File transfer,
 * usb_mode.c) and the battery in words.
 * Let go past half way or flicked down, it opens; else it goes back. Dragged up, a tap
 * below it or a toggle closes it; the dark theme's leaves it open, made again in the other.
 *
 * Below them, the notifications (notify.c), which follow changes while it is open:
 * one tapped closes it and opens; "Clear all" marks them all seen.
 *
 * It is made at each opening, in the theme's colors of the moment, and deleted
 * once closed. It goes on the top layer under the status bar, over the back
 * and home gestures' strips.
 */

#include <dirent.h>
#include <string.h>

#include "net/bluetooth.h"
#include "data/data.h"
#include "i18n.h"
#include "modem/modem.h"
#include "ui.h"
#include "ui_symbols.h"
#include "net/usb_mode.h"

#define SLOP 12     /* px moved before the panel follows */
#define FLICK 400   /* px/s */
/* Between two readings, not a finger moving: a quick flick goes up to ~300 px
 * down or up, hardly sideways */
#define JUMP_X 160
#define JUMP_Y 480
#define OVER 48     /* px of the panel above the screen: its top corners stay hidden */
#define DIM LV_OPA_40 /* the screen below the panel, open (less while it moves) */
#define TICK_MS 1000
#define TILE 120     /* px: a tile's height */
#define SLIDER 112   /* px: a slider's height, and its tile's beside it, square */
#define BUTTON 80    /* px across: Power and Settings, at the top */
#define RADIUS 28    /* of the tiles and sliders */
#define ICON_W 96    /* px at a slider's start where its icon is centered */

static struct {
	lv_obj_t *bar;               /* the status bar, which takes the pull */
	lv_obj_t *scrim, *panel;     /* while it is shown */
	lv_obj_t *clock;             /* the time over the date (ui_clock_update()) */
	lv_obj_t *volume, *volume_icon, *usb, *battery;
	lv_obj_t *battery_icon;      /* drawn, in battery's row */
	lv_obj_t *wifi, *wifi_name;  /* the tile, its label: the network's name */
	lv_obj_t *bt, *bt_name;      /* the tile, its label: the device connected */
	lv_obj_t *data;              /* the mobile data tile, NULL without */
	lv_obj_t *torch;             /* starts the toggles' second row upright */
	lv_obj_t *gap;               /* an empty column after it: the rows' tiles line up (with mobile data) */
	lv_obj_t *sliders;           /* their rows: one above the other, side by side sideways */
	lv_obj_t *usb_arrow;         /* at the end of usb's row, NULL without the board's USB modes */
	lv_obj_t *usb_modes;         /* under it, unfolded: Network only, File transfer */
	lv_obj_t *usb_network, *usb_files; /* their rows */
	bool usb_unfolded;
	lv_obj_t *notifications;     /* their section, hidden without any */
	unsigned version;            /* data_version it shows */
	lv_timer_t *timer;
	int32_t h;                   /* panel height on screen (without OVER) */
	int32_t pos;                 /* how far it is down, 0 to h */
	int32_t start;               /* pos when the finger went down */
	int32_t y0, y, v;            /* where it started following, last position, speed (px/s) */
	uint32_t t;                  /* of the last position */
	lv_point_t last;             /* previous reading */
	bool dragging;
	bool jumped;                 /* ignored until the finger lifts: not a finger moving */
	bool declined;               /* from elsewhere (ui_shade_pull_from()), up first: not ours */
	bool opening;                /* where the last slide went: open, or closed */
} sh;

static void shade_close(bool anim);
static void wifi_update(void);
static void bt_update(void);
static void mobile_update(void);
static void usb_modes_update(void);
static void slider_icon_update(lv_obj_t *slider);

/* Position */

/* Only what changed: setting the scrim's opacity redraws the whole screen, even the
 * same one (each reading of a finger held still, or past the panel's end) */
static void pos_set(int32_t pos)
{
	sh.pos = LV_CLAMP(0, pos, sh.h);
	lv_obj_set_y(sh.panel, sh.pos - sh.h - OVER);
	lv_opa_t opa = sh.h ? (lv_opa_t)(DIM * sh.pos / sh.h) : 0;
	if (lv_obj_get_style_bg_opa(sh.scrim, 0) != opa)
		lv_obj_set_style_bg_opa(sh.scrim, opa, 0);
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
	sh.panel = sh.scrim = sh.clock = sh.volume = sh.volume_icon = sh.usb = sh.battery = NULL;
	sh.battery_icon = NULL;
	sh.wifi = sh.wifi_name = sh.bt = sh.bt_name = sh.torch = sh.gap = sh.sliders = sh.data = sh.notifications = NULL;
	sh.usb_arrow = sh.usb_modes = sh.usb_network = sh.usb_files = NULL;
	sh.usb_unfolded = false;
	sh.dragging = false;
	ui_status_shade(false);
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
	bt_update();
	mobile_update();
	usb_modes_update();
	ui_clock_update(sh.clock);

	/* The cable's: Wi-Fi or mobile data may well be online without it */
	ui_set_text(sh.usb, usb_file_transfer() ? _("Connected to a computer, file transfer on") :
	            usb == 2 ? _("Connected to a computer and its internet") :
	            usb == 1 ? _("Connected to a computer") :
	            ui_usb_powered() ? _("Charger only") : _("Not connected"));

	long capacity, uv;
	char status[32], full[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		capacity = -1;
	ui_battery_status(status, sizeof(status));
	if (!ui_read_long(UI_BATTERY "/voltage_now", &uv))
		uv = -1;
	ui_battery_icon_update(sh.battery_icon);
	if (capacity < 0)
		ui_set_text(sh.battery, _("No battery driver"));
	else if (!ui_battery_present() && uv >= 0)
		ui_set_text_fmt(sh.battery, _("No usable battery (%.1f V)"), uv / 1e6);
	else if (!ui_battery_present())
		ui_set_text(sh.battery, _("No usable battery"));
	else if (strcmp(status, "Charging") == 0 && ui_battery_time_to_full(full, sizeof(full)))
		ui_set_text_fmt(sh.battery, _("%ld%% \xc2\xb7 Full in ~%s"), capacity, full);
	else
		ui_set_text_fmt(sh.battery, "%ld%% \xc2\xb7 %s", capacity,
		                status[0] ? ui_battery_word(status) : _("unknown"));

	/* The volume keys move it too */
	int level = ui_volume_get();
	if (level >= 0 && lv_slider_get_value(sh.volume) != level &&
	    !lv_obj_has_state(sh.volume, LV_STATE_PRESSED)) {
		lv_slider_set_value(sh.volume, level, LV_ANIM_OFF);
		volume_icon_update(level);
		slider_icon_update(sh.volume);
	}
}

/* Notifications */

#define NOTIFICATIONS 3 /* shown, then how many more */

static void notifications_fill(void);

/* One opened: closed on the way */
static void opening(void)
{
	shade_close(true);
}

/* The panel measured again after its content changed: open, it stays so */
static void measure(void)
{
	bool open = sh.pos == sh.h;
	lv_obj_update_layout(sh.panel);
	sh.h = lv_obj_get_height(sh.panel) - OVER;
	pos_set(open ? sh.h : sh.pos);
}

static void notifications_refresh(void)
{
	notifications_fill();
	measure();
}

/* After the tap's event: the button tapped is deleted */
static void refresh_async(void *data)
{
	LV_UNUSED(data);
	if (sh.panel && !sh.dragging && !lv_anim_get(&sh, NULL))
		notifications_refresh();
}

static void clear_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	notifications_clear();
	lv_async_call(refresh_async, NULL);
}

/* Their heading and "Clear all" above the card */
static void notifications_fill(void)
{
	lv_obj_t *s = sh.notifications;
	lv_obj_clean(s);
	sh.version = data_version;
	lv_obj_set_hidden(s, !notification_count());
	if (!notification_count())
		return;
	lv_obj_t *head = ui_box(s);
	lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	ui_dim_label(head, _("Notifications"), UI_FONT_CAPTION);
	lv_obj_t *clear = lv_label_create(head);
	lv_label_set_text_static(clear, _("Clear all"));
	lv_obj_set_style_text_font(clear, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(clear, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_text_opa(clear, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_set_clickable(clear, true);
	lv_obj_set_ext_click_area(clear, 20);
	lv_obj_add_event_cb(clear, clear_cb, LV_EVENT_CLICKED, NULL);
	/* On the panel's color the card would not show: the colors of a tile off, its
	 * dividers gaps in the panel's color */
	lv_obj_t *card = lv_obj_get_child(notifications_card_create(s, NOTIFICATIONS, NULL, opening), 0);
	lv_obj_set_style_radius(card, 28, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_BUTTON), 0);
	for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
		lv_obj_t *row = lv_obj_get_child(card, i);
		lv_obj_set_style_radius(row, 28, LV_STATE_PRESSED);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	status_update();
	/* Not while it moves: it is measured again */
	if (sh.version != data_version && !sh.dragging && !lv_anim_get(&sh, NULL))
		notifications_refresh();
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

static void create(void);

/* Made again in the theme of the screens rebuilt, open */
static void theme_reopen(void)
{
	create();
	pos_set(sh.h);
}

static void theme_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* The screens are rebuilt in the other theme, this panel in their colors after them */
	shade_close(false);
	ui_set_light(!ui_light, theme_reopen);
}

static void scrim_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	shade_close(true);
}

/* Room around a row of tiles for a pressed tile, which grows (the theme's style) */
static void tiles_ext_cb(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, 8);
}

/* A row of tiles (wrapping), a pressed one not cut at its edges */
static lv_obj_t *row_create(lv_obj_t *parent)
{
	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(row, 24, 0);
	lv_obj_set_style_pad_column(row, 24, 0);
	lv_obj_set_overflow_visible(row, true);
	lv_obj_add_event_cb(row, tiles_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(row);
	return row;
}

/* How far the fill reaches from the slider's start: empty at the minimum, whole at the
 * maximum, where the finger is in between */
static int32_t slider_fill(lv_obj_t *slider)
{
	int32_t min = lv_slider_get_min_value(slider), max = lv_slider_get_max_value(slider);
	return lv_obj_get_width(slider) * (lv_slider_get_value(slider) - min) / LV_MAX(max - min, 1);
}

/* The icon white on the fill once it reaches its middle, in the text color before */
static void slider_icon_update(lv_obj_t *slider)
{
	lv_obj_set_style_text_color(lv_obj_get_child(slider, 0), slider_fill(slider) >= ICON_W / 2 ?
	                            lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
}

static void slider_changed_cb(lv_event_t *e)
{
	slider_icon_update(lv_event_get_current_target_obj(e));
}

/* The level: the card filled in the accent color from its start to the value */
static void slider_draw_cb(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_current_target_obj(e);
	lv_area_t a;
	lv_obj_get_coords(slider, &a);
	if (!slider_fill(slider))
		return;
	a.x2 = a.x1 + slider_fill(slider) - 1;
	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_color_hex(UI_COLOR_ACCENT);
	rect.radius = RADIUS;
	lv_draw_rect(lv_event_get_layer(e), &rect, &a);
}

/* A slider as a card in a row of its own, its symbol inside at its start (in *icon),
 * without a knob */
static lv_obj_t *slider_row(lv_obj_t *parent, const char *symbol, lv_obj_t **icon)
{
	lv_obj_t *slider = lv_slider_create(row_create(parent));
	lv_obj_set_height(slider, SLIDER);
	lv_obj_set_flex_grow(slider, 1);
	lv_obj_set_style_radius(slider, RADIUS, 0);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_BUTTON), 0);
	lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, 0);
	/* The value from the card's edges, where the fill reaches */
	lv_obj_set_style_pad_hor(slider, 0, 0);
	/* Its own fill rather than the indicator, which would be cut short of a full radius */
	lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);

	lv_obj_t *label = lv_label_create(slider);
	lv_label_set_text_static(label, symbol);
	lv_obj_set_style_text_font(label, UI_FONT_HEADLINE, 0);
	lv_obj_set_width(label, ICON_W);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_add_event_cb(slider, slider_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_SIZE_CHANGED, NULL); /* laid out */
	if (icon)
		*icon = label;
	return slider;
}

/* A toggle: a rounded tile, in the accent color while on, square, its symbol over the text
 * if any; cb (if any) gets clicks */
static lv_obj_t *tile_create(lv_obj_t *parent, const char *symbol, const char *text, bool on,
                             lv_event_cb_t cb)
{
	lv_obj_t *tile = lv_button_create(parent);
	lv_obj_set_size(tile, TILE, TILE);
	lv_obj_set_style_radius(tile, RADIUS, 0);
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
	if (!text)
		return tile;
	lv_obj_t *label = lv_label_create(tile);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	return tile;
}

/* A tile's label for a name (a network's, a device's): centered, wrapped onto a second line when
 * too wide for one, the rest cut */
static void name_label(lv_obj_t *label)
{
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_max_height(label, 2 * lv_font_get_line_height(UI_FONT_CAPTION), 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
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
	sh.wifi = tile_create(parent, LV_SYMBOL_WIFI, _("Wi-Fi"), false, NULL);
	lv_obj_add_event_cb(sh.wifi, wifi_toggle_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(sh.wifi, wifi_settings_cb, LV_EVENT_LONG_PRESSED, NULL);
	sh.wifi_name = lv_obj_get_child(sh.wifi, 1);
	ui_set_text(sh.wifi_name, _("Wi-Fi"));
	name_label(sh.wifi_name);
}

static void wifi_update(void)
{
	char ssid[33];
	int state = wifi_network(ssid, sizeof(ssid));
	ui_set_text(sh.wifi_name, state > 0 && ssid[0] ? ssid : _("Wi-Fi"));
	lv_obj_set_state(sh.wifi, LV_STATE_CHECKED, state >= 0 && wifi_enabled());
	lv_obj_set_state(sh.wifi, LV_STATE_DISABLED, state < 0);
}

/* Bluetooth: as Wi-Fi, labelled with the device connected */
static void bt_toggle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	bt_set_powered(bt_state() != BT_ON);
	bt_update();
}

static void bt_settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (ui_locked())
		return;
	shade_close(true);
	bluetooth_open();
}

static void bt_tile(lv_obj_t *parent)
{
	bt_refresh(); /* off, the status bar's tick does not ask */
	sh.bt = tile_create(parent, LV_SYMBOL_BLUETOOTH, "Bluetooth", false, NULL); /* NON-NLS */
	lv_obj_add_event_cb(sh.bt, bt_toggle_cb, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(sh.bt, bt_settings_cb, LV_EVENT_LONG_PRESSED, NULL);
	sh.bt_name = lv_obj_get_child(sh.bt, 1);
	name_label(sh.bt_name);
}

static void bt_update(void)
{
	struct bt_device devices[BT_MAX_DEVICES];
	enum bt_state state = bt_state();
	unsigned count = state == BT_ON ? bt_devices(devices, BT_MAX_DEVICES) : 0;
	const char *name = "Bluetooth"; /* NON-NLS */
	for (unsigned i = 0; i < count; i++)
		if (devices[i].connected) {
			name = devices[i].name[0] ? devices[i].name : devices[i].address;
			break;
		}
	ui_set_text(sh.bt_name, name);
	lv_obj_set_state(sh.bt, LV_STATE_CHECKED, state == BT_ON);
	lv_obj_set_state(sh.bt, LV_STATE_DISABLED, state == BT_UNAVAILABLE);
}

/* Upright, Wi-Fi, Bluetooth and mobile data above the flashlight and the rotation lock and a
 * gap, in columns (two of two without mobile data), the sliders one above the other;
 * sideways, the toggles all on one row, the sliders side by side */
static void tiles_layout(void)
{
	lv_display_t *d = lv_display_get_default();
	bool wide = lv_display_get_horizontal_resolution(d) > lv_display_get_vertical_resolution(d);
	lv_obj_set_flex_in_new_track(sh.torch, !wide);
	lv_obj_set_hidden(sh.gap, wide || !sh.data);
	for (uint32_t i = 0; i < lv_obj_get_child_count(sh.sliders); i++) {
		lv_obj_t *row = lv_obj_get_child(sh.sliders, i);
		lv_obj_set_width(row, wide ? 1 : LV_PCT(100));
		lv_obj_set_flex_grow(row, wide);
	}
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
	sh.data = tile_create(parent, UI_SYMBOL_TOWER_BROADCAST, _("Mobile data"), modem_data_enabled(),
	                      mobile_toggle_cb);
}

/* What the USB is used for, under its row: unfolded by the row's arrow while a computer is
 * plugged in (or file transfer is on); File transfer is not chosen from the lock screen.
 * Only what changed is set: the panel stays still between frames */
static void set_hidden(lv_obj_t *obj, bool hidden)
{
	if (lv_obj_is_hidden(obj) != hidden)
		lv_obj_set_hidden(obj, hidden);
}

static void usb_modes_update(void)
{
	if (!sh.usb_arrow)
		return;
	bool on = usb_file_transfer(), busy = usb_mode_busy();
	bool shown = on || usb_computer();
	if (!shown)
		sh.usb_unfolded = false;
	bool resized = lv_obj_is_hidden(sh.usb_modes) == (shown && sh.usb_unfolded);
	set_hidden(sh.usb_arrow, !shown);
	set_hidden(sh.usb_modes, !(shown && sh.usb_unfolded));
	lv_obj_set_clickable(lv_obj_get_parent(sh.usb), shown);
	const char *arrow = sh.usb_unfolded ? LV_SYMBOL_UP : LV_SYMBOL_DOWN;
	if (lv_label_get_text(sh.usb_arrow) != arrow)
		lv_label_set_text_static(sh.usb_arrow, arrow);
	set_hidden(lv_obj_get_child(sh.usb_network, -1), on);
	set_hidden(lv_obj_get_child(sh.usb_files, -1), !on);
	lv_obj_set_state(sh.usb_network, LV_STATE_DISABLED, busy);
	lv_obj_set_state(sh.usb_files, LV_STATE_DISABLED, busy || (ui_locked() && !on));
	if (resized && sh.timer) /* not while it is made: measured then */
		measure();
}

static void usb_row_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	sh.usb_unfolded = !sh.usb_unfolded;
	usb_modes_update();
}

static void usb_mode_cb(lv_event_t *e)
{
	bool files = lv_event_get_target_obj(e) == sh.usb_files;
	if (files != usb_file_transfer()) {
		ui_vibrate(UI_VIBRATE_TAP);
		usb_file_transfer_set(files);
	}
	status_update();
}

/* A mode's row, under the USB row's text: its name, a check mark while it is the one */
static lv_obj_t *usb_mode_row(lv_obj_t *parent, const char *text)
{
	lv_obj_t *row = ui_box(parent);
	lv_obj_set_size(row, LV_PCT(100), 64);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(row, 80, 0); /* under the text: the symbols' 48 and the gap */
	lv_obj_set_style_radius(row, RADIUS, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(row, lv_color_hex(UI_COLOR_DIM), LV_STATE_DISABLED);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, usb_mode_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(row);
	ui_set_text(label, text);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *check = lv_label_create(row);
	lv_label_set_text_static(check, LV_SYMBOL_OK);
	lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_hidden(check, true);
	return row;
}

/* The USB row tappable, its arrow at the end, the modes under it (folded) */
static void usb_modes_create(lv_obj_t *panel)
{
	lv_obj_t *row = lv_obj_get_parent(sh.usb);
	lv_obj_set_style_radius(row, RADIUS, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_add_event_cb(row, usb_row_cb, LV_EVENT_CLICKED, NULL);
	sh.usb_arrow = lv_label_create(row);
	lv_label_set_text_static(sh.usb_arrow, LV_SYMBOL_DOWN);
	lv_obj_set_style_text_color(sh.usb_arrow, lv_color_hex(UI_COLOR_DIM), 0);

	sh.usb_modes = ui_box(panel);
	lv_obj_set_size(sh.usb_modes, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sh.usb_modes, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_margin_top(sh.usb_modes, -16, 0); /* closer to its row than the next one */
	sh.usb_network = usb_mode_row(sh.usb_modes, _("Network only"));
	sh.usb_files = usb_mode_row(sh.usb_modes, _("File transfer"));
	lv_obj_set_hidden(sh.usb_modes, true);
}

static void drag_cb(lv_event_t *e);

static void drag_events(lv_obj_t *obj)
{
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* Power: the menu of a held Power key, over the panel */
static void power_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_power_menu();
}

static void settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (ui_locked())
		return;
	shade_close(true);
	settings_open();
}

/* The time over the date on the left (the status bar's moved here), Power and Settings on
 * the right, round; Settings dim over the lock screen */
static void header_create(lv_obj_t *panel)
{
	lv_obj_t *head = ui_box(panel);
	lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	sh.clock = ui_box(head);
	lv_obj_set_size(sh.clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sh.clock, LV_FLEX_FLOW_COLUMN);
	lv_obj_t *time = lv_label_create(sh.clock);
	ui_set_text(time, "");
	lv_obj_set_style_text_font(time, UI_FONT_DISPLAY, 0);
	lv_obj_t *date = lv_label_create(sh.clock);
	ui_set_text(date, "");
	lv_obj_set_style_text_color(date, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *buttons = ui_box(head);
	lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(buttons, 24, 0);
	ui_round_button(buttons, BUTTON, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_POWER, power_cb, NULL);
	lv_obj_t *gear = ui_round_button(buttons, BUTTON, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                                 LV_SYMBOL_SETTINGS, settings_cb, NULL);
	lv_obj_set_style_text_color(gear, lv_color_hex(UI_COLOR_DIM), LV_STATE_DISABLED);
	lv_obj_set_state(gear, LV_STATE_DISABLED, ui_locked());
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
	lv_obj_set_style_text_font(p, UI_FONT_BODY, 0);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	/* Dragged up from anywhere but its controls */
	lv_obj_set_clickable(p, true);
	drag_events(p);

	header_create(p);

	/* The toggles, sharing their rows' width */
	lv_obj_t *tiles = row_create(p);
	wifi_tile(tiles);
	bt_tile(tiles);
	if (modem_data_available())
		mobile_tile(tiles);
	lv_obj_t *torch = sh.torch = tile_create(tiles, LV_SYMBOL_CHARGE, _("Flashlight"), torch_on(), torch_cb);
	lv_obj_set_state(torch, LV_STATE_DISABLED, !torch_path());
	tile_create(tiles, UI_SYMBOL_LOCK, _("Rotation lock"), ui_rotation_lock() >= 0,
	            rotation_lock_cb);
	sh.gap = ui_box(tiles);
	lv_obj_set_height(sh.gap, TILE);
	for (uint32_t i = 0; i < lv_obj_get_child_count(tiles); i++)
		lv_obj_set_flex_grow(lv_obj_get_child(tiles, i), 1);

	/* The sliders, each with a tile beside it */
	sh.sliders = ui_box(p);
	lv_obj_set_size(sh.sliders, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sh.sliders, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(sh.sliders, 24, 0);
	lv_obj_set_style_pad_column(sh.sliders, 24, 0);
	lv_obj_set_overflow_visible(sh.sliders, true); /* a pressed tile grows */
	lv_obj_add_event_cb(sh.sliders, tiles_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(sh.sliders);

	/* The dark theme beside the brightness (alone without a backlight) */
	int b = ui_brightness_get();
	lv_obj_t *row;
	if (b >= 0) {
		lv_obj_t *sun;
		lv_obj_t *slider = slider_row(sh.sliders, sun_symbol(b), &sun);
		lv_slider_set_range(slider, UI_BRIGHTNESS_MIN, 100);
		lv_slider_set_value(slider, b, LV_ANIM_OFF);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, sun);
		lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, sun);
		row = lv_obj_get_parent(slider);
	} else {
		row = row_create(sh.sliders);
	}
	/* Switching rebuilds the screens: an open app would be closed */
	lv_obj_t *theme = tile_create(row, UI_SYMBOL_MOON, NULL, !ui_light, theme_cb);
	lv_obj_set_size(theme, SLIDER, SLIDER);
	lv_obj_set_state(theme, LV_STATE_DISABLED, !ui_at_home() || ui_locked());

	sh.volume = slider_row(sh.sliders, LV_SYMBOL_VOLUME_MAX, &sh.volume_icon);
	lv_slider_set_range(sh.volume, 0, UI_VOLUME_LEVELS);
	int level = LV_MAX(ui_volume_get(), 0);
	lv_slider_set_value(sh.volume, level, LV_ANIM_OFF);
	volume_icon_update(level);
	lv_obj_add_event_cb(sh.volume, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_t *haptics = tile_create(lv_obj_get_parent(sh.volume), LV_SYMBOL_BELL, NULL, ui_haptics, haptics_cb);
	lv_obj_set_size(haptics, SLIDER, SLIDER);
	tiles_layout();

	/* The mobile network's name is the status bar's while the panel is out */
	sh.usb = state_row(p, LV_SYMBOL_USB);
	if (usb_mode_available())
		usb_modes_create(p);
	sh.battery = state_row(p, NULL);

	sh.notifications = ui_box(p);
	lv_obj_set_size(sh.notifications, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sh.notifications, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(sh.notifications, 12, 0);
	lv_obj_set_style_margin_top(sh.notifications, 8, 0);
	notifications_fill();

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
	lv_obj_move_to_index(sh.bar, -1);
	ui_status_shade(true);
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
	lv_obj_t *target = lv_event_get_current_target_obj(e);
	bool elsewhere = target != sh.bar && target != sh.panel;

	if (code == LV_EVENT_PRESSED) {
		sh.y0 = sh.y = p.y;
		sh.last = p;
		sh.v = 0;
		sh.t = lv_tick_get();
		sh.start = sh.pos;
		sh.dragging = sh.jumped = sh.declined = false;
		return;
	}
	if (sh.declined || sh.jumped)
		return; /* until the finger lifts */
	if (code == LV_EVENT_PRESSING) {
		/* The pointer moved to another finger (or a wrong reading) rather than a
		 * finger moving: the pull is called off, the panel goes back */
		if (LV_ABS(p.x - sh.last.x) > JUMP_X || LV_ABS(p.y - sh.last.y) > JUMP_Y) {
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
			if (elsewhere) {
				/* Pulled down only, the first way it went: up is another drag's */
				if (p.y < sh.y0) {
					sh.declined = true;
					return;
				}
				/* Not pressed in the screen under the panel */
				lv_obj_remove_state(lv_event_get_target_obj(e), LV_STATE_PRESSED);
			}
			if (!sh.panel)
				create();
			lv_anim_delete(&sh, NULL);
			sh.start = sh.pos;
			sh.dragging = true;
			sh.y0 = p.y;
		}
		uint32_t dt = lv_tick_elaps(sh.t);
		if (dt) {
			sh.v = (sh.v + (p.y - sh.y) * 1000 / (int32_t)dt) / 2;
			sh.y = p.y;
			sh.t = lv_tick_get();
		}
		pos_set(sh.start + p.y - sh.y0);
		return;
	}
	if (code == LV_EVENT_PRESS_LOST && !sh.dragging)
		return;
	if (!sh.dragging) {
		/* A tap: the status bar toggles it, the panel's background does nothing */
		if (lv_event_get_current_target_obj(e) != sh.bar)
			return;
		/* Lifted on the bar too: a press there that ends elsewhere is not a tap */
		if (p.y >= UI_STATUS_H)
			return;
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
	slide_to(open ? sh.h : 0);
}

void ui_shade_init(lv_obj_t *status_bar)
{
	sh.bar = status_bar;
	lv_obj_set_clickable(status_bar, true);
	lv_obj_set_scrollable(status_bar, false);
	lv_obj_set_gesture_bubble(status_bar, false);
	usb_mode_init();
	drag_events(status_bar);
	lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED,
	                        NULL);
}

void ui_shade_pull_from(lv_obj_t *obj)
{
	drag_events(obj);
}

bool ui_shade_shown(void)
{
	return sh.panel != NULL;
}

void ui_shade_close(void)
{
	shade_close(false);
}
