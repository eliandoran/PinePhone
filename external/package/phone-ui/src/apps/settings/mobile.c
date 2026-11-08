/* Settings, the Mobile section of About phone and Network: the phone number and the
 * IMEI; on Network also the network's name, the mobile data switch (the shade's
 * tile's setting), data roaming's and the APN */

#include "modem/modem.h"
#include "settings.h"

static void data_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	modem_data_set_enabled(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

static void roaming_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	modem_data_set_roaming(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

/* The APN field: saved once done (Enter, the keyboard put away, the page left), not
 * as each key is typed: each change attaches the modem again */
static void apn_cb(lv_event_t *e)
{
	struct settings_mobile *m = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(m->kb, true);
		lv_obj_scroll_to_view_recursive(m->apn, LV_ANIM_ON);
		break;
	case LV_EVENT_READY:
	case LV_EVENT_CANCEL:
		ui_keyboard_show(m->kb, false);
		modem_data_set_apn(lv_textarea_get_text(m->apn));
		break;
	case LV_EVENT_DELETE:
		modem_data_set_apn(lv_textarea_get_text(m->apn));
		break;
	default:
		break;
	}
}

/* Below the Mobile card: the field, empty for the network's own APN, and the keyboard
 * under the page (which shrinks above it) */
static void apn_create(lv_obj_t *page, struct settings_mobile *m)
{
	ui_section_label(page, _("Access point name (APN)"));
	m->apn = ui_text_field(page, 88, 63, apn_cb, m);
	lv_obj_set_width(m->apn, LV_PCT(100));
	lv_textarea_set_placeholder_text(m->apn, _("Automatic"));
	lv_textarea_set_accepted_chars(m->apn, MODEM_APN_CHARS);
	lv_textarea_set_text(m->apn, modem_data_apn());
	lv_obj_add_event_cb(m->apn, apn_cb, LV_EVENT_DELETE, m);
	m->kb = ui_keyboard_create(lv_obj_get_parent(page), m->apn);
}

void settings_mobile_create(lv_obj_t *page, struct settings_mobile *m, bool network)
{
	*m = (struct settings_mobile){ 0 };
	if (!network && modem_state() == MODEM_NONE)
		return;
	ui_section_label(page, _("Mobile"));
	lv_obj_t *card = ui_card_create(page);
	if (network)
		m->network = settings_info_row(card, _("Network"));
	if (network && modem_data_available()) {
		lv_obj_t *row = settings_row(card);
		lv_obj_t *title = lv_label_create(row);
		lv_label_set_text_static(title, _("Mobile data"));
		lv_obj_set_flex_grow(title, 1);
		m->data = ui_switch_create(row, modem_data_enabled());
		lv_obj_add_event_cb(m->data, data_cb, LV_EVENT_VALUE_CHANGED, NULL);
		row = settings_row(card);
		title = lv_label_create(row);
		lv_label_set_text_static(title, _("Data roaming"));
		lv_obj_set_flex_grow(title, 1);
		lv_obj_t *roaming = ui_switch_create(row, modem_data_roaming());
		lv_obj_add_event_cb(roaming, roaming_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}
	if (modem_state() != MODEM_NONE) {
		m->number = settings_info_row(card, _("Phone number"));
		m->imei = settings_info_row(card, "IMEI");
	}
	if (network && modem_data_available())
		apn_create(page, m);
	settings_mobile_update(m);
}

void settings_mobile_update(const struct settings_mobile *m)
{
	if (m->network)
		ui_set_text(m->network, modem_summary());
	/* Changed from the shade meanwhile */
	if (m->data && lv_obj_has_state(m->data, LV_STATE_CHECKED) != modem_data_enabled())
		lv_obj_set_state(m->data, LV_STATE_CHECKED, modem_data_enabled());
	if (m->number) {
		const char *number = modem_number();
		ui_set_text(m->number, number[0] ? number : modem_state() == MODEM_NO_SIM ? _("No SIM card") : _("Unknown"));
	}
	if (m->imei)
		ui_set_text(m->imei, modem_imei()[0] ? modem_imei() : _("Unknown"));
}
