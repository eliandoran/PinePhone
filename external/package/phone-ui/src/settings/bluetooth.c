/*
 * Settings, Bluetooth: on and off, the paired devices (a tap connects or disconnects
 * one), the devices nearby (a tap pairs with one, its code confirmed here when it shows
 * one). Through BlueZ (bluetooth.c); while the page is open the phone discovers and is
 * visible to others, and their pairing is asked here too.
 */

#include <stdio.h>
#include <string.h>

#include "../bluetooth.h"
#include "settings.h"

#define ROW_H 112
#define TICK_MS 500

static struct {
	lv_obj_t *toggle, *status;
	lv_obj_t *prompt, *prompt_title, *prompt_code, *prompt_buttons;
	lv_obj_t *paired_label, *paired, *nearby_label, *nearby;
	char note[96];  /* what happened to the last pairing, shown as the status */
	char shown[2048]; /* what the lists show: rebuilt when it changes */
	struct bt_device devices[BT_MAX_DEVICES]; /* the rows' */
	unsigned count;
} st;

static lv_timer_t *timer;

static void refresh(void)
{
	if (timer)
		lv_timer_ready(timer);
}

static const char *signal_text(int dbm)
{
	return dbm >= -55 ? C_("signal", "Excellent") : dbm >= -67 ? C_("signal", "Good") :
	       dbm >= -75 ? C_("signal", "Fair") : C_("signal", "Weak");
}

static const char *device_name(const struct bt_device *d)
{
	return d->name[0] ? d->name : d->address;
}

/* The name of the device at `path`, its path's address if it is not listed */
static const char *name_of(const char *path)
{
	const struct bt_device *d = bt_device(path);
	const char *dev = strstr(path, "/dev_");
	return d ? device_name(d) : dev ? dev + 5 : path;
}

static const char *device_state(const struct bt_device *d)
{
	const struct bt_pairing_info *p = bt_pairing();
	if ((p->state == BT_PAIR_BUSY || p->state == BT_PAIR_CONFIRM) && strcmp(p->path, d->path) == 0)
		return _("Pairing...");
	if (strcmp(bt_connecting(), d->path) == 0)
		return _("Connecting...");
	if (d->paired)
		return d->connected ? _("Connected") : _("Not connected");
	return signal_text(d->rssi);
}

static void device_cb(lv_event_t *e)
{
	const struct bt_device *d = lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	st.note[0] = '\0';
	if (d->paired)
		bt_connect(d->path, !d->connected);
	else
		bt_pair(d->path);
	refresh();
}

static void row_add(lv_obj_t *card, const struct bt_device *d, bool divider)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, device_cb, (void *)d);
	lv_obj_t *text = ui_row_text(row);
	lv_obj_set_width(ui_one_line_label(text, ui_intern(device_name(d)), UI_FONT_TITLE), LV_PCT(100));
	lv_obj_set_width(ui_dim_label(text, device_state(d), UI_FONT_CAPTION), LV_PCT(100));
}

static void lists_update(bool on)
{
	struct bt_device devices[BT_MAX_DEVICES];
	unsigned count = on ? bt_devices(devices, BT_MAX_DEVICES) : 0, paired = 0;
	char shown[sizeof(st.shown)];
	size_t len = (size_t)snprintf(shown, sizeof(shown), "%d\n", on);
	for (unsigned i = 0; i < count && len < sizeof(shown); i++) {
		paired += devices[i].paired;
		len += (size_t)snprintf(shown + len, sizeof(shown) - len, "%s|%s|%s\n", devices[i].path,
		                        device_name(&devices[i]), device_state(&devices[i]));
	}
	if (strcmp(shown, st.shown) == 0)
		return;
	snprintf(st.shown, sizeof(st.shown), "%s", shown);
	memcpy(st.devices, devices, count * sizeof(devices[0]));
	st.count = count;

	lv_obj_clean(st.paired);
	lv_obj_clean(st.nearby);
	lv_obj_set_hidden(st.paired_label, !paired);
	lv_obj_set_hidden(st.paired, !paired);
	lv_obj_set_hidden(st.nearby_label, !on);
	lv_obj_set_hidden(st.nearby, !on);
	for (unsigned i = 0; i < paired; i++)
		row_add(st.paired, &st.devices[i], i + 1 < paired);
	for (unsigned i = paired; i < count; i++)
		row_add(st.nearby, &st.devices[i], i + 1 < count);
	if (on && paired == count) {
		lv_obj_t *row = ui_row_create(st.nearby, ROW_H, false, NULL, NULL);
		ui_dim_label(row, _("Searching..."), UI_FONT_BODY);
	}
}

/* A code to confirm, or to type on the device */
static void prompt_update(void)
{
	const struct bt_pairing_info *p = bt_pairing();
	bool confirm = p->state == BT_PAIR_CONFIRM, show = p->state == BT_PAIR_SHOW;
	lv_obj_set_hidden(st.prompt, !confirm && !show);
	if (!confirm && !show)
		return;
	ui_set_text_fmt(st.prompt_title, confirm ? _("Pair with %s?") : _("Type this code on %s"), name_of(p->path));
	ui_set_text(st.prompt_code, p->code);
	lv_obj_set_hidden(st.prompt_code, !p->code[0]);
	lv_obj_set_hidden(st.prompt_buttons, !confirm);
}

static void tick_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	bt_poll();
	bt_refresh();
	bt_poll(); /* what the refresh found missing: discovery, the agent */

	const struct bt_pairing_info *p = bt_pairing();
	if (p->state == BT_PAIR_FAILED)
		snprintf(st.note, sizeof(st.note), _("Could not pair with %s"), name_of(p->path));
	if (p->state == BT_PAIR_DONE || p->state == BT_PAIR_FAILED)
		bt_pairing_clear();

	enum bt_state state = bt_state();
	lv_obj_set_state(st.toggle, LV_STATE_CHECKED, state == BT_ON);
	lv_obj_set_state(st.toggle, LV_STATE_DISABLED, state == BT_UNAVAILABLE);
	if (state == BT_UNAVAILABLE)
		ui_set_text(st.status, _("Not available"));
	else if (state == BT_OFF)
		ui_set_text(st.status, _("Off"));
	else if (st.note[0])
		ui_set_text(st.status, st.note);
	else
		ui_set_text_fmt(st.status, _("Visible as %s"), bt_name());
	prompt_update();
	lists_update(state == BT_ON);
}

static void toggle_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	st.note[0] = '\0';
	bt_set_powered(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
	refresh();
}

static void answer_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	bt_pair_confirm(lv_event_get_user_data(e) != NULL);
	refresh();
}

static void page_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(timer);
	timer = NULL;
	bt_active(false);
	memset(&st, 0, sizeof(st));
}

void bluetooth_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Bluetooth", &page); /* NON-NLS */
	lv_obj_add_event_cb(scr, page_delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *title = lv_label_create(row);
	lv_label_set_text_static(title, "Bluetooth"); /* NON-NLS */
	lv_obj_set_flex_grow(title, 1);
	st.toggle = ui_switch_create(row, false);
	lv_obj_add_event_cb(st.toggle, toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
	st.status = settings_info_row(card, _("Status"));

	st.prompt = settings_panel(page);
	st.prompt_title = lv_label_create(st.prompt);
	ui_set_text(st.prompt_title, "");
	lv_obj_set_width(st.prompt_title, LV_PCT(100));
	lv_label_set_long_mode(st.prompt_title, LV_LABEL_LONG_MODE_WRAP);
	st.prompt_code = lv_label_create(st.prompt);
	ui_set_text(st.prompt_code, "");
	lv_obj_set_style_text_font(st.prompt_code, UI_FONT_DISPLAY, 0);
	st.prompt_buttons = ui_box(st.prompt);
	lv_obj_set_size(st.prompt_buttons, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(st.prompt_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(st.prompt_buttons, 16, 0);
	lv_obj_set_style_pad_top(st.prompt_buttons, 8, 0);
	lv_obj_t *no = ui_button(st.prompt_buttons, 1, 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Cancel"),
	                         answer_cb, NULL);
	lv_obj_set_flex_grow(no, 1);
	lv_obj_t *yes = ui_button(st.prompt_buttons, 1, 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Pair"),
	                          answer_cb, (void *)1);
	lv_obj_set_flex_grow(yes, 1);
	lv_obj_set_hidden(st.prompt, true);

	st.paired_label = ui_section_label(page, _("Paired devices"));
	st.paired = ui_card_create(page);
	st.nearby_label = ui_section_label(page, _("Devices nearby"));
	st.nearby = ui_card_create(page);
	lv_obj_set_hidden(st.paired_label, true);
	lv_obj_set_hidden(st.paired, true);
	lv_obj_set_hidden(st.nearby_label, true);
	lv_obj_set_hidden(st.nearby, true);

	bt_active(true);
	timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	tick_cb(timer);
	ui_push(scr);
}

/* For the list of Settings and the Network page: on, and to what it is connected */
void bluetooth_summary(lv_obj_t *value)
{
	struct bt_device devices[BT_MAX_DEVICES];
	bt_refresh();
	enum bt_state state = bt_state();
	unsigned count = bt_devices(devices, BT_MAX_DEVICES);
	const char *connected = NULL;
	for (unsigned i = 0; i < count && !connected; i++)
		if (devices[i].connected)
			connected = device_name(&devices[i]);
	ui_set_text(value, state == BT_UNAVAILABLE ? _("Not available") : state == BT_OFF ? _("Off") :
	                   connected ? connected : _("On"));
}
