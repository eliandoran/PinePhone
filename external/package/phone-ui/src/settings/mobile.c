/* Settings, the Mobile section of About phone and Network: the phone number and the
 * IMEI, with the network's name on Network */

#include "../modem.h"
#include "settings.h"

void settings_mobile_create(lv_obj_t *page, struct settings_mobile *m, bool network)
{
	*m = (struct settings_mobile){ 0 };
	if (!network && modem_state() == MODEM_NONE)
		return;
	ui_section_label(page, "Mobile");
	lv_obj_t *card = ui_card_create(page);
	if (network)
		m->network = settings_info_row(card, "Network");
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
	if (m->number) {
		const char *number = modem_number();
		ui_set_text(m->number, number[0] ? number : modem_state() == MODEM_NO_SIM ? "No SIM card" : "Unknown");
	}
	if (m->imei)
		ui_set_text(m->imei, modem_imei()[0] ? modem_imei() : "Unknown");
}
