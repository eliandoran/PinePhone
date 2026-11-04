/* Settings, Wi-Fi (settings/wifi.c), through the UI (ui_test.h), on the fake
 * wpa_supplicant of fake_wpa.c */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define CONF "/var/lib/wpa_supplicant/wpa_supplicant.conf"
#define WPA2 "[WPA2-PSK-CCMP][ESS]"


/* A file's content (static), "" if it cannot be read */
static const char *file(const char *path)
{
	static char text[4096];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f)
		fclose(f);
	text[n] = '\0';
	return text;
}

static bool open_wifi(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Wi-Fi");
}

/* The vertical middle of the label showing `text`, -1 if none */
static int32_t y_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	lv_area_t a;
	if (!label)
		return -1;
	lv_obj_get_coords(label, &a);
	return (a.y1 + a.y2) / 2;
}

TEST(not_running)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("Not available"));
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_shows("Not available"));
	CHECK(!ui_test_shows("Networks"));
}

TEST(networks_strongest_first)
{
	ui_test_wifi_ap("Cafe", -80, "[ESS]", NULL);
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_ap("", -40, WPA2, "x"); /* hidden */
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_shows("Not connected"));
	CHECK(ui_test_shows("Cafe"));
	CHECK(y_of("Home") < y_of("Cafe"));
	CHECK(ui_test_shows_part("Secured"));
	CHECK(ui_test_shows_part("Open"));
}

TEST(ssid_in_utf8)
{
	ui_test_wifi_ap("Caf\xc3\xa9 \"Le Bon\"", -60, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Caf\xc3\xa9 \"Le Bon\"", 2000));
	CHECK(ui_test_tap_text("Caf\xc3\xa9 \"Le Bon\""));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Caf\xc3\xa9 \"Le Bon\"", 2000));
	CHECK(strstr(file(CONF), "ssid=436166c3a920224c6520426f6e22\n") != NULL);
}

TEST(connect_with_password)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_shows("192.168.1.42"));
	CHECK(strstr(file(CONF), "ssid=486f6d65\n") != NULL);
	CHECK(strstr(file(CONF), "psk=\"secret123\"\n") != NULL);
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Home")); /* the list's value */
}

TEST(wpa3_transition)
{
	ui_test_wifi_ap("Home", -50, "[WPA2-PSK+SAE-CCMP][SAE-H2E][ESS]", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(strstr(file(CONF), "key_mgmt=WPA-PSK SAE\n") != NULL);
	CHECK(strstr(file(CONF), "ieee80211w=1\n") != NULL);
}

TEST(wrong_password)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("wrongpass\n"));
	CHECK(ui_test_wait_for("Wrong password for Home", 2000));
	CHECK(strstr(file(CONF), "network={") == NULL); /* forgotten */
	CHECK(ui_test_tap_text("Home"));                 /* asks again */
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

TEST(password_too_short)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("abc\n"));
	CHECK(ui_test_shows("At least 8 characters"));
	CHECK(ui_test_textarea() != NULL); /* still asking */
	CHECK(strstr(file(CONF), "network={") == NULL);
}

TEST(open_network)
{
	ui_test_wifi_ap("Cafe", -70, "[ESS]", NULL);
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Cafe", 2000));
	CHECK(ui_test_tap_text("Cafe"));
	CHECK(ui_test_wait_for("Connected to Cafe", 2000));
	CHECK(ui_test_textarea() == NULL); /* no password asked */
	CHECK(strstr(file(CONF), "key_mgmt=NONE\n") != NULL);
}

TEST(saved_network_without_password)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_ap("Cafe", -70, "[ESS]", NULL);
	ui_test_wifi_saved("Cafe", NULL); /* connected */
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Cafe", 2000));
	CHECK(ui_test_shows("Saved")); /* Home */
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_textarea() == NULL);
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

TEST(forget)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_tap_text("Forget this network"));
	CHECK(ui_test_wait_for("Not connected", 2000));
	CHECK(!ui_test_shows("Forget this network"));
	CHECK(!ui_test_shows("192.168.1.42"));
	CHECK(strstr(file(CONF), "network={") == NULL);
}

TEST(enterprise_not_supported)
{
	ui_test_wifi_ap("Corp", -50, "[WPA2-EAP-CCMP][ESS]", "x");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Corp", 2000));
	CHECK(ui_test_shows_part("Not supported"));
	CHECK(ui_test_tap_text("Corp"));
	CHECK(ui_test_textarea() == NULL);
}

TEST(network_page_links_here)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Network"));
	CHECK(ui_test_shows("Home")); /* the Wi-Fi row */
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

/* The status bar: hidden without a connection, the signal in bars */

TEST(status_bar_without_wifi)
{
	ui_test_start();
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), -1);
	ui_test_wifi_ap("Home", -50, WPA2, "secret123"); /* running, not connected */
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), -1);
}

TEST(status_bar_signal)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), 4);
	ui_test_screenshot("build/wifi_status_bar.png");
}

TEST(status_bar_weak_signal)
{
	ui_test_wifi_ap("Far", -78, WPA2, "secret123");
	ui_test_wifi_saved("Far", "secret123");
	ui_test_start();
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), 2);
}

TEST(status_bar_after_forget)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_tap_text("Forget this network"));
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), -1);
}

/* Details of the connection */

#define ROUTE "/tmp/route" /* PROC_NET_ROUTE of wifi.o, the Makefile's */
#define ROUTE_HEADER "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"

TEST(connection_details)
{
	/* The router 192.168.1.1, default route through wlan0 (before the USB one) */
	ui_test_write(ROUTE, ROUTE_HEADER
	              "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
	              "usb0\t00000000\t022A10AC\t0003\t0\t0\t10\t00000000\t0\t0\t0\n"
	              "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_shows("192.168.1.1"));
	CHECK(ui_test_shows("2.4 GHz, channel 6"));
	CHECK(ui_test_shows("Excellent (-50 dBm)"));
	CHECK(ui_test_shows("72 Mbit/s"));
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), 4);
	ui_test_screenshot("build/wifi_details.png");
	CHECK(ui_test_shows("WPA2"));
}

TEST(connection_details_5ghz)
{
	remove(ROUTE);
	ui_test_wifi_ap("Home", -72, WPA2, "secret123");
	ui_test_wifi_frequency("Home", 5180);
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_shows("5 GHz, channel 36"));
	CHECK(ui_test_shows("Fair (-72 dBm)"));
	CHECK(!ui_test_shows("Router")); /* no default route through wlan0 */
}

TEST(connection_details_wpa3_and_open)
{
	ui_test_wifi_ap("Home", -50, "[WPA2-PSK+SAE-CCMP][SAE-H2E][ESS]", "secret123");
	ui_test_wifi_ap("Cafe", -60, "[ESS]", NULL);
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_shows("WPA3"));
	CHECK(ui_test_tap_text("Cafe"));
	CHECK(ui_test_wait_for("Connected to Cafe", 2000));
	CHECK(ui_test_shows("Open"));
}

TEST(connection_details_hidden_when_not_connected)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(!ui_test_shows("Frequency"));
	CHECK(!ui_test_shows("Security"));
	CHECK(!ui_test_shows("Link speed"));
}

/* Quick settings: a card with the network, opening this page */

static void shade_pull(void)
{
	ui_test_start();
	ui_test_tap_at(360, UI_STATUS_H / 2); /* the status bar */
}

/* The tile showing `text`, NULL if none */
static lv_obj_t *tile(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	return label ? lv_obj_get_parent(label) : NULL;
}

TEST(quick_settings_connected)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_test_wait(1100);
	shade_pull();
	CHECK(ui_test_shows("Vibration")); /* open */
	lv_obj_t *wifi = tile("Home"), *torch = tile("Flashlight");
	CHECK(wifi && torch);
	if (!wifi || !torch)
		return;
	CHECK(lv_obj_has_state(wifi, LV_STATE_CHECKED));
	/* As wide as the toggles, on a row of its own above them */
	lv_area_t w, t;
	lv_obj_get_coords(wifi, &w);
	lv_obj_get_coords(torch, &t);
	CHECK_INT(lv_area_get_width(&w), lv_area_get_width(&t));
	CHECK_INT(w.x1, t.x1);
	CHECK(w.y2 < t.y1);
	CHECK(ui_test_long_press_text("Home")); /* a tap turns it off */
	CHECK(!ui_test_shows("Vibration")); /* closed */
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

TEST(quick_settings_not_connected)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_start();
	ui_test_wait(1100);
	shade_pull();
	CHECK(ui_test_shows("Vibration"));
	lv_obj_t *wifi = tile("Wi-Fi");
	CHECK(wifi && lv_obj_has_state(wifi, LV_STATE_CHECKED)); /* on, not connected */
	CHECK(ui_test_long_press_text("Wi-Fi"));
	CHECK(ui_test_wait_for("Networks", 2000));
}

TEST(quick_settings_not_running)
{
	shade_pull();
	CHECK(ui_test_shows("Vibration"));
	lv_obj_t *wifi = tile("Wi-Fi");
	CHECK(wifi && lv_obj_has_state(wifi, LV_STATE_DISABLED));
}

TEST(quick_settings_locked)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_lock();
	ui_test_wait(1100);
	shade_pull();
	CHECK(ui_test_shows("Home"));
	CHECK(ui_test_long_press_text("Home"));
	CHECK(!ui_test_shows("Networks")); /* not over the lock screen */
}

/* Off and on: wlan0 down (wpa_supplicant waits), remembered */

#define SETTINGS "/var/lib/phone-ui/settings"

TEST(switched_off_and_on)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(!ui_test_wifi_up());
	CHECK(ui_test_wait_for("Off", 2000));
	CHECK(!ui_test_shows("Networks"));
	CHECK(!ui_test_shows("192.168.1.42"));
	CHECK(!ui_test_shows("Forget this network"));
	CHECK(strstr(file(SETTINGS), "wifi=0") != NULL);
	ui_test_wait(1100);
	CHECK_INT(ui_wifi_bars(), -1); /* the status bar */
	ui_test_tap(sw);
	CHECK(ui_test_wifi_up());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(strstr(file(SETTINGS), "wifi=1") != NULL);
}

TEST(off_at_startup)
{
	ui_test_reset();
	ui_test_write(SETTINGS, "wifi=0\n");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	CHECK(!ui_test_wifi_up());
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("Off")); /* the list's value */
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_shows("Off"));
	lv_obj_t *sw = ui_test_find_widget(&lv_switch_class);
	CHECK(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED));
}

TEST(quick_settings_tap_toggles)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_test_wait(1100);
	shade_pull();
	CHECK(ui_test_tap_text("Home"));
	CHECK(!ui_test_wifi_up());
	CHECK(ui_test_shows("Vibration")); /* still open */
	lv_obj_t *wifi = tile("Wi-Fi");
	CHECK(wifi && !lv_obj_has_state(wifi, LV_STATE_CHECKED | LV_STATE_DISABLED));
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_wifi_up());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(lv_obj_has_state(tile("Home"), LV_STATE_CHECKED));
}
