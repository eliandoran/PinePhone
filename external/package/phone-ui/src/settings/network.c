/* Settings, Network: interfaces and their addresses, Wi-Fi, Bluetooth, the mobile network */

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"

/* IPv4 address of the first interface whose name starts with `prefix` */
static bool if_address(const char *prefix, char *buf, size_t size)
{
	struct ifaddrs *ifs, *i;
	bool found = false;
	if (getifaddrs(&ifs) < 0)
		return false;
	for (i = ifs; i && !found; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
		    strncmp(i->ifa_name, prefix, strlen(prefix)) == 0)
			found = inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
			                  buf, (socklen_t)size) != NULL;
	freeifaddrs(ifs);
	return found;
}

void network_summary(lv_obj_t *value)
{
	char buf[INET_ADDRSTRLEN];
	ui_set_text(value, if_address("wlan", buf, sizeof(buf)) ? _("Wi-Fi")
	                   : if_address("usb", buf, sizeof(buf)) ? "USB" : _("Offline"));
}

static void wifi_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	wifi_open();
}

static void bluetooth_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	bluetooth_open();
}

static struct settings_mobile mobile;

static void network_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	settings_mobile_update(&mobile);
}

void network_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Network"), &page);

	ui_section_label(page, _("Connections"));
	lv_obj_t *card = ui_card_create(page);
	struct ifaddrs *ifs, *i;
	unsigned count = 0;
	if (getifaddrs(&ifs) == 0) {
		for (i = ifs; i; i = i->ifa_next) {
			char addr[INET_ADDRSTRLEN], title[64];
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK))
				continue;
			if (!inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof(addr)))
				continue;
			snprintf(title, sizeof(title), "%s (%s)",
			         strncmp(i->ifa_name, "usb", 3) == 0 ? "USB" :
			         strncmp(i->ifa_name, "wlan", 4) == 0 ? _("Wi-Fi") : _("Network"),
			         i->ifa_name);
			ui_set_text(settings_info_row(card, ui_intern(title)), addr);
			count++;
		}
		freeifaddrs(ifs);
	}
	if (!count)
		ui_set_text(settings_info_row(card, "USB"), ui_usb_powered() ? _("Charger only") : _("Not connected"));

	ui_section_label(page, _("Wireless"));
	card = ui_card_create(page);
	lv_obj_t *wifi = settings_info_row(card, _("Wi-Fi"));
	wifi_summary(wifi);
	settings_row_tappable(lv_obj_get_parent(wifi), wifi_cb, NULL);
	lv_obj_t *bluetooth = settings_info_row(card, "Bluetooth"); /* NON-NLS */
	bluetooth_summary(bluetooth);
	settings_row_tappable(lv_obj_get_parent(bluetooth), bluetooth_cb, NULL);

	settings_mobile_create(page, &mobile, true);
	settings_timer(scr, network_update, 2000);
	ui_push(scr);
}
