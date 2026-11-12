/*
 * Status bar, on the top layer: it stays put while screens slide under it. The time,
 * the notifications on the left (the network's name while the shade is out), the
 * connections and the battery on the right.
 */

#include <string.h>
#include <time.h>

#include "net/bluetooth.h"
#include "data/data.h"
#include "modem/modem.h"
#include "ui_internal.h"
#include "ui_symbols.h"

static lv_obj_t *status_bar, *status_time, *status_charging, *status_usb, *status_wifi, *status_bt;
static lv_obj_t *status_signal, *status_tech; /* mobile signal bars, "4G" while data is up */
static lv_obj_t *status_roaming; /* "R" after the bars while roaming */
static lv_obj_t *status_modem;   /* a symbol in the bars' place when something is wrong */
/* Notifications: messages unread, missed calls, apps' */
static lv_obj_t *status_messages, *status_missed, *status_apps;
static lv_obj_t *status_battery_icon; /* drawn, left of status_charging (the bolt with the percentage) */
static lv_obj_t *status_network; /* the operator, on the left while the shade is out */
static bool shade_out, time_hidden;

static int wifi_bars = -1; /* wifi_level() at the last update */
static int bt_shown = -1; /* bt_status() at the last update */

int ui_wifi_bars(void)
{
	return wifi_bars;
}

/* Wi-Fi: a dot and three arcs fanning up from it, in the text color as far as the
 * signal's bars (user data, 0 to 4) reach, faint beyond */
static void wifi_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	int bars = (int)(intptr_t)lv_obj_get_user_data(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t w = LV_MAX(lv_area_get_height(&a) / 8, 2); /* stroke */
	int32_t dot = w + 1;

	/* The dot's bottom on the box's, the outer arc's top on its top: the box's height
	 * and place are the mobile signal's, so both icons span the same rows */
	lv_draw_arc_dsc_t arc;
	lv_draw_arc_dsc_init(&arc);
	arc.color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	arc.center = (lv_point_t){ (a.x1 + a.x2) / 2, a.y2 - w }; /* the dot's rows: center - dot to center + w */
	arc.rounded = 1;
	int32_t r = arc.center.y - a.y1; /* the outer arc's outer edge */
	for (int i = 0; i < 4; i++) {
		arc.opa = i < bars ? LV_OPA_COVER : LV_OPA_30;
		arc.radius = dot + i * (r - dot) / 3; /* evenly apart */
		arc.width = i == 0 ? arc.radius : w;
		arc.start_angle = i == 0 ? 0 : 225; /* clockwise from the right: the top quarter */
		arc.end_angle = i == 0 ? 360 : 315;
		lv_draw_arc(layer, &arc);
	}
}

/* While the modem starts or looks for a network (no bars yet, nothing wrong), the bars
 * light up one after another, then all go dim and again: something is going on. A step
 * each SEARCH_STEP_MS, 0 to 4 lit; the timer only while it shows */
#define SEARCH_STEP_MS 250
static lv_timer_t *search_timer;
static int search_step;

static void search_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	search_step = (search_step + 1) % 5;
	lv_obj_invalidate(status_signal);
}

static void search_show(bool on)
{
	if (on == (search_timer != NULL))
		return;
	if (on) {
		search_step = 0;
		search_timer = lv_timer_create(search_cb, SEARCH_STEP_MS, NULL);
	} else {
		lv_timer_delete(search_timer);
		search_timer = NULL;
	}
	lv_obj_invalidate(status_signal);
}

/* Mobile signal: four bars rising left to right, in the text color as far as the
 * bars (user data, -1 to 4: none lit while not registered) reach, faint beyond. None
 * while the modem's state shows in their place (status_layout()) */
static void signal_draw_cb(lv_event_t *e)
{
	if (lv_label_get_text(status_modem)[0])
		return;
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	int bars = search_timer ? search_step : (int)(intptr_t)lv_obj_get_user_data(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t gap = 3, w = (lv_area_get_width(&a) - 3 * gap) / 4, h = lv_area_get_height(&a);

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	rect.radius = 2;
	for (int i = 0; i < 4; i++) {
		rect.bg_opa = i < bars ? LV_OPA_COVER : LV_OPA_30;
		lv_area_t bar = { a.x1 + i * (w + gap), a.y2 - h * (i + 1) / 4 + 1, 0, a.y2 };
		bar.x2 = bar.x1 + w - 1;
		lv_draw_rect(layer, &rect, &bar);
	}
}

static bool status_item_shown(lv_obj_t *o)
{
	return !lv_obj_is_hidden(o) && !(lv_obj_check_type(o, &lv_label_class) && !lv_label_get_text(o)[0]);
}

/* The space between two of the status bar's items side by side: 6 px from the bars or
 * the "R" to the "R" or the technology, 10 px between the connections, 16 px between
 * the notifications (three, room enough) */
static int32_t status_gap(lv_obj_t *left, lv_obj_t *right)
{
	if (left == status_messages || left == status_missed)
		return 16;
	return (left == status_signal || left == status_roaming) &&
	       (right == status_roaming || right == status_tech) ? 6 : 10;
}

/* The status bar as Android's: on the left what came (the notifications: messages,
 * missed calls, apps'), on the right, 16 px left of the battery, the connections: USB, Wi-Fi,
 * the mobile signal, roaming's "R", its technology; what is hidden or empty skipped.
 * The modem's state (no SIM, no service...) shows in the bars' place, all dim then:
 * the modem starting, searching or failing moves nothing */
static void status_layout(void)
{
	lv_obj_t *left[] = { status_messages, status_missed, status_apps };
	lv_obj_t *prev = NULL;
	int32_t x = 0;
	for (unsigned i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
		lv_obj_t *o = left[i];
		if (!status_item_shown(o))
			continue;
		if (prev)
			x += status_gap(prev, o);
		lv_obj_update_layout(o);
		lv_obj_align(o, LV_ALIGN_LEFT_MID, x, 0);
		x += lv_obj_get_width(o);
		prev = o;
	}

	/* From the battery leftwards */
	lv_obj_t *right[] = { status_usb, status_bt, status_wifi, status_signal, status_roaming, status_tech };
	lv_obj_update_layout(status_battery_icon);
	x = lv_obj_get_width(status_battery_icon) + 16;
	/* The bolt after the battery, in the bar's padding: plugging in moves nothing */
	if (status_item_shown(status_charging)) {
		lv_obj_update_layout(status_charging);
		lv_obj_align(status_charging, LV_ALIGN_RIGHT_MID, 4 + lv_obj_get_width(status_charging), 0);
	}
	lv_obj_t *next = NULL;
	for (unsigned i = sizeof(right) / sizeof(right[0]); i-- > 0;) {
		lv_obj_t *o = right[i];
		if (!status_item_shown(o))
			continue;
		if (next)
			x += status_gap(o, next);
		lv_obj_update_layout(o);
		lv_obj_align(o, LV_ALIGN_RIGHT_MID, -x, 0);
		x += lv_obj_get_width(o);
		next = o;
	}
	if (status_item_shown(status_modem)) {
		lv_obj_update_layout(status_signal);
		lv_obj_align_to(status_modem, status_signal, LV_ALIGN_CENTER, 0, 0);
	}
	lv_obj_invalidate(status_signal); /* drawn or not, as the state shows */
}

void ui_status_update(void)
{
	char buf[64];
	bt_tick(); /* Bluetooth turned on at startup, once its adapter is there */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	ui_set_text(status_time, buf);

	/* What the left side shows; laid out again when that changes */
	bool relayout = false;

	/* The mobile signal while there is a modem: bars lit while registered */
	int signal = modem_signal_bars();
	bool signal_hidden = modem_state() == MODEM_NONE;
	if (signal_hidden != lv_obj_is_hidden(status_signal) ||
	    signal != (int)(intptr_t)lv_obj_get_user_data(status_signal)) {
		lv_obj_set_hidden(status_signal, signal_hidden);
		lv_obj_set_user_data(status_signal, (void *)(intptr_t)signal);
		lv_obj_invalidate(status_signal);
		relayout = true;
	}
	/* Roaming: an "R", as Android; information, in the text color */
	if (modem_roaming() == lv_obj_is_hidden(status_roaming)) {
		lv_obj_set_hidden(status_roaming, !modem_roaming());
		relayout = true;
	}
	/* Mobile data while its session is up: the technology ("4G"), as Android; not
	 * while Wi-Fi is connected, whose route comes first. Otherwise only while the shade
	 * is out */
	bool data = modem_data_state() == MODEM_DATA_CONNECTED && wifi_level() < 0;
	const char *tech = data || shade_out ? modem_technology_text() : "";
	if (strcmp(lv_label_get_text(status_tech), tech) != 0) {
		lv_label_set_text_static(status_tech, tech);
		relayout = true;
	}

	/* The modem's state when something is wrong: a symbol (no SIM, no service), red
	 * when waiting will not help (no modem, SIM blocked) */
	const char *state = modem_status_icon();
	if (strcmp(lv_label_get_text(status_modem), state) != 0) {
		lv_label_set_text_static(status_modem, state);
		relayout = true;
	}
	lv_color_t state_color = lv_color_hex(modem_status_alarm() ? UI_COLOR_RED : UI_COLOR_TEXT);
	if (!lv_color_eq(lv_obj_get_style_text_color(status_modem, 0), state_color))
		lv_obj_set_style_text_color(status_modem, state_color, 0);
	/* Starting or searching: no bars yet, nothing wrong */
	search_show(!signal_hidden && signal < 0 && !state[0]);

	/* Wi-Fi while connected, its bars the signal (all dim until it has an address) */
	int bars = wifi_level();
	if (bars != wifi_bars) {
		wifi_bars = bars;
		lv_obj_set_hidden(status_wifi, bars < 0);
		lv_obj_set_user_data(status_wifi, (void *)(intptr_t)bars);
		lv_obj_invalidate(status_wifi);
		relayout = true;
	}

	/* USB symbol while plugged into a computer, dim until it has its address on the USB network
	 * (as Wi-Fi's bars until the phone has one); whether it shares its internet is the shade's */
	usb_net_update(now);
	int usb = ui_usb_net();
	/* Compared with the theme's: setting a style redraws, even the same */
	lv_color_t usb_color = lv_color_hex(usb_net_lit() ? UI_COLOR_TEXT : UI_COLOR_DIM);
	if (!lv_color_eq(lv_obj_get_style_text_color(status_usb, 0), usb_color))
		lv_obj_set_style_text_color(status_usb, usb_color, 0);
	if ((usb == 0) != lv_obj_is_hidden(status_usb)) {
		lv_obj_set_hidden(status_usb, usb == 0);
		relayout = true;
	}
	/* Bluetooth while on, dim until a device is connected */
	int bt = bt_status();
	lv_color_t bt_color = lv_color_hex(bt > 0 ? UI_COLOR_TEXT : UI_COLOR_DIM);
	if (bt >= 0 && !lv_color_eq(lv_obj_get_style_text_color(status_bt, 0), bt_color))
		lv_obj_set_style_text_color(status_bt, bt_color, 0); /* as USB's */
	if (bt != bt_shown) {
		if ((bt < 0) != (bt_shown < 0))
			relayout = true;
		lv_obj_set_hidden(status_bt, bt < 0);
		bt_shown = bt;
	}
	/* Notifications not seen yet; the shade lists them, the network's name in their place
	 * while it is out (the operator, or what is wrong) */
	bool messages = notification_messages() > 0 && !shade_out,
	     missed = notification_missed_calls() > 0 && !shade_out, apps = notification_apps() > 0 && !shade_out;
	const char *network = shade_out && modem_state() != MODEM_NONE ? modem_summary() : "";
	if (strcmp(lv_label_get_text(status_network), network) != 0)
		ui_set_text(status_network, network);
	if (messages == lv_obj_is_hidden(status_messages) || missed == lv_obj_is_hidden(status_missed) ||
	    apps == lv_obj_is_hidden(status_apps)) {
		lv_obj_set_hidden(status_messages, !messages);
		lv_obj_set_hidden(status_missed, !missed);
		lv_obj_set_hidden(status_apps, !apps);
		relayout = true;
	}

	long capacity;
	char status[32];
	if (!ui_read_long(UI_BATTERY "/capacity", &capacity))
		capacity = -1;
	ui_battery_status(status, sizeof(status));
	ui_battery_update((int)capacity, status);

	/* Only the icon without a driver (a "?") or a usable battery (crossed out). The
	 * percentage only if asked (Settings, Battery): the fill shows the level. With it
	 * the number is in the icon, wider for "100", and the bolt after it */
	bool percent = ui_setting_get_int("battery_percent", 0), charging = false;
	if (percent != battery_icon_percent(status_battery_icon, percent)) {
		lv_obj_set_size(status_battery_icon, percent ? 46 : 40, percent ? 22 : 20);
		relayout = true;
	}
	if (capacity < 0 || !ui_battery_present()) {
		ui_battery_icon_set(status_battery_icon, capacity < 0 ? UI_BATTERY_UNKNOWN : UI_BATTERY_ABSENT, false);
	} else {
		charging = ui_battery_plugged(status);
		ui_battery_icon_set(status_battery_icon, (int)capacity, charging);
	}
	const char *bolt = percent && charging ? LV_SYMBOL_CHARGE : "";
	if (strcmp(lv_label_get_text(status_charging), bolt) != 0) {
		lv_label_set_text_static(status_charging, bolt);
		relayout = true;
	}
	/* The connections stand left of the battery */
	if (relayout)
		status_layout();
}

void ui_status_bar_foreground(void)
{
	lv_obj_move_to_index(status_bar, -1);
}

/* Recolored with the theme */
void status_recolor(void)
{
	if (!status_bar)
		return;
	lv_obj_set_style_text_color(status_bar, lv_color_hex(UI_COLOR_TEXT), 0);
	ui_status_update();
}

void status_chrome(bool hidden, bool time)
{
	if (!status_bar)
		return;
	time_hidden = time;
	lv_obj_set_hidden(status_bar, hidden);
	lv_obj_set_hidden(status_time, time_hidden || shade_out);
}

void ui_status_shade(bool out)
{
	if (!status_bar || shade_out == out)
		return;
	shade_out = out;
	lv_obj_set_hidden(status_time, time_hidden || shade_out);
	ui_status_update();
}

lv_obj_t *status_init(void)
{
	lv_obj_t *bar = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), UI_STATUS_H);
	/* No background: the wallpapers are shaded at the top (wallpaper.c) */
	lv_obj_set_style_pad_hor(bar, 20, 0);
	lv_obj_set_style_text_color(bar, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(bar, UI_FONT_BODY, 0);

	/* The notifications and the connections, placed by status_layout() */
	modem_init();
	status_signal = ui_box(bar);
	lv_obj_set_size(status_signal, 30, 24);
	lv_obj_set_user_data(status_signal, (void *)(intptr_t)modem_signal_bars());
	lv_obj_add_event_cb(status_signal, signal_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(status_signal, modem_state() == MODEM_NONE);

	status_roaming = lv_label_create(bar);
	lv_obj_set_style_text_font(status_roaming, UI_FONT_CAPTION, 0);
	lv_label_set_text_static(status_roaming, "R");
	lv_obj_set_hidden(status_roaming, !modem_roaming());

	status_tech = lv_label_create(bar);
	lv_obj_set_style_text_font(status_tech, UI_FONT_CAPTION, 0);
	lv_label_set_text_static(status_tech, "");

	status_modem = lv_label_create(bar);
	lv_obj_set_style_text_font(status_modem, &ui_font_20, 0);
	lv_label_set_text_static(status_modem, modem_status_icon());

	status_wifi = ui_box(bar);
	lv_obj_set_size(status_wifi, 36, 24); /* as tall as status_signal */
	lv_obj_add_event_cb(status_wifi, wifi_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(status_wifi, true);

	/* Upright as the bars and Wi-Fi, about as tall in the smaller font */
	status_usb = lv_label_create(bar);
	lv_obj_set_style_text_font(status_usb, &ui_font_20, 0);
	lv_label_set_text_static(status_usb, UI_SYMBOL_USB_270);
	lv_obj_set_hidden(status_usb, true);

	/* Font Awesome 7's, 24 px tall in this font as Wi-Fi (LVGL's is 21 in the smaller one) */
	status_bt = lv_label_create(bar);
	lv_obj_set_style_text_font(status_bt, &ui_font_24, 0);
	lv_label_set_text_static(status_bt, UI_SYMBOL_BLUETOOTH_B);
	lv_obj_set_hidden(status_bt, true);

	status_messages = lv_label_create(bar);
	lv_obj_set_style_text_font(status_messages, &ui_font_20, 0);
	lv_label_set_text_static(status_messages, LV_SYMBOL_ENVELOPE);
	lv_obj_set_hidden(status_messages, true);
	status_missed = lv_label_create(bar);
	lv_obj_set_style_text_font(status_missed, &ui_font_20, 0);
	lv_label_set_text_static(status_missed, UI_SYMBOL_PHONE_SLASH);
	lv_obj_set_hidden(status_missed, true);
	status_apps = lv_label_create(bar);
	lv_obj_set_style_text_font(status_apps, &ui_font_20, 0);
	lv_label_set_text_static(status_apps, LV_SYMBOL_BELL);
	lv_obj_set_hidden(status_apps, true);

	status_network = lv_label_create(bar);
	ui_set_text(status_network, "");
	lv_obj_set_style_max_width(status_network, 360, 0);
	lv_label_set_long_mode(status_network, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(status_network, LV_ALIGN_LEFT_MID, 0, 0);

	status_time = lv_label_create(bar);
	ui_set_text(status_time, "");
	lv_obj_align(status_time, LV_ALIGN_CENTER, 0, 0);

	/* The bolt placed by status_layout(), right of the battery */
	status_battery_icon = ui_battery_icon_create(bar, 40, 20);
	lv_obj_align(status_battery_icon, LV_ALIGN_RIGHT_MID, 0, 0);
	status_charging = lv_label_create(bar);
	lv_obj_set_style_text_font(status_charging, &ui_font_20, 0);
	lv_label_set_text_static(status_charging, "");
	status_layout();

	status_bar = bar; /* recolored by status_recolor() from now on */
	return bar;
}
