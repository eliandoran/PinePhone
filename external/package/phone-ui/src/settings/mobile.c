/* Settings, the Mobile section of About phone and Network: the phone number and the
 * IMEI; on Network also the network's name and the mobile data switch (the shade's
 * tile's setting) */

#include "../modem.h"
#include "settings.h"

static void data_cb(lv_event_t *e)
{
	ui_vibrate(15);
	modem_data_set_enabled(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

void settings_mobile_create(lv_obj_t *page, struct settings_mobile *m, bool network)
{
	*m = (struct settings_mobile){ 0 };
	if (!network && modem_state() == MODEM_NONE)
		return;
	ui_section_label(page, "Mobile");
	lv_obj_t *card = ui_card_create(page);
	if (network)
		m->network = settings_info_row(card, "Network");
	if (network && modem_data_available()) {
		lv_obj_t *row = settings_row(card);
		lv_obj_t *title = lv_label_create(row);
		lv_label_set_text_static(title, "Mobile data");
		lv_obj_set_flex_grow(title, 1);
		m->data = ui_switch_create(row, modem_data_enabled());
		lv_obj_add_event_cb(m->data, data_cb, LV_EVENT_VALUE_CHANGED, NULL);
	}
	if (modem_state() != MODEM_NONE) {
		m->number = settings_info_row(card, "Phone number");
		m->imei = settings_info_row(card, "IMEI");
	}
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
		ui_set_text(m->number, number[0] ? number : modem_state() == MODEM_NO_SIM ? "No SIM card" : "Unknown");
	}
	if (m->imei)
		ui_set_text(m->imei, modem_imei()[0] ? modem_imei() : "Unknown");
}
