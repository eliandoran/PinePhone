/* Settings, Wi-Fi (settings/wifi.c), through the UI (ui_test.h), on the fake
 * wpa_supplicant of fake_wpa.c */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"
#include "modem/modem.h"
#include "ui/qr.h"
#include "ui/qrcodegen.h"

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

/* Two access points of one network (a mesh, a router's two bands): listed once, as the
 * stronger */
TEST(one_network_of_two_access_points)
{
	ui_test_wifi_ap("Home", -80, WPA2, "secret123");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_ap("Home", -70, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("Home", found, 4), 1);
	CHECK(ui_test_shows("Secured \xc2\xb7 Excellent"));
	CHECK(!ui_test_shows("Secured \xc2\xb7 Weak"));
}

/* The password confirmed with the header's button; the keyboard put away and back */
TEST(password_with_the_ok_button)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123"));
	CHECK(ui_test_key(UI_TEST_KEY_HIDE)); /* hidden */
	CHECK(ui_test_textarea() == NULL);
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_textarea() != NULL);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

/* A saved network whose password changed on the router */
TEST(saved_network_wrong_password)
{
	ui_test_wifi_ap("Home", -50, WPA2, "newsecret");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Saved", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_wait_for("Wrong password for Home", 2000));
	ui_test_wait(1100);
	CHECK(ui_test_shows("Saved")); /* kept: it was not just added */
}

/* Connecting takes a while: said so until connected */
TEST(connecting)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_slow("Home", true);
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connecting to Home...", 2000));
	ui_test_wifi_slow("Home", false);
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

/* wpa_supplicant stops answering: not available, then back once it answers again */
TEST(supplicant_hung)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	ui_test_wifi_hung(true);
	CHECK(ui_test_wait_for("Not available", 5000));
	CHECK(!ui_test_shows("Networks"));
	ui_test_wifi_hung(false);
	CHECK(ui_test_wait_for("Connected to Home", 5000));
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
	CHECK(ui_test_font("At least 8 characters") == UI_FONT_BODY);
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

/* The rows of the status bar where columns x1 to x2 draw anything (faint too) */
static void drawn_rows(int x1, int x2, int *top, int *bottom)
{
	*top = *bottom = -1;
	for (int y = 0; y < UI_STATUS_H; y++)
		for (int x = x1; x <= x2; x++) {
			lv_color_t c = ui_test_pixel(x, y);
			if ((c.red + c.green + c.blue) / 3 > 40) {
				if (*top < 0)
					*top = y;
				*bottom = y;
			}
		}
}

/* Wi-Fi beside the mobile signal: as tall, on the same bottom line */
TEST(status_bar_aligned_with_mobile_signal)
{
	ui_test_wifi_ap("Home", -65, WPA2, "secret123"); /* 3 bars of 4 */
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_modem_signal(-60, UI_TEST_TECH_LTE);
	bool ready = false;
	for (int t = 0; t < 5000 && !ready; t += 48) {
		ready = modem_signal_bars() == 4 && ui_wifi_bars() == 3;
		ui_test_wait(48);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	CHECK(ready);
	ui_test_wait(1100); /* the status bar updated, nothing between them */

	int signal_top, signal_bottom, wifi_top, wifi_bottom;
	/* The signal's 30 px 16 px left of the battery (40 px, the bar's padding), Wi-Fi left of it */
	drawn_rows(720 - 20 - 40 - 16 - 30, 720 - 20 - 40 - 16 - 1, &signal_top, &signal_bottom);
	drawn_rows(400, 720 - 20 - 40 - 16 - 30 - 1, &wifi_top, &wifi_bottom);
	CHECK(signal_top > 0);
	CHECK_INT(wifi_top, signal_top);
	CHECK_INT(wifi_bottom, signal_bottom);
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

/* Wi-Fi 6E's band, and a frequency of none of the three (60 GHz): in MHz */
TEST(connection_details_6ghz_and_others)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_frequency("Home", 5975);
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_wait_for("6 GHz, channel 5", 2000));
	ui_test_wifi_frequency("Home", 5935);
	CHECK(ui_test_wait_for("6 GHz, channel 2", 2000));
	ui_test_wifi_frequency("Home", 58320);
	CHECK(ui_test_wait_for("58320 MHz", 2000));
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
	CHECK(ui_test_shows("Flashlight")); /* open */
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
	CHECK(!ui_test_shows("Flashlight")); /* closed */
	CHECK(ui_test_wait_for("Connected to Home", 2000));
}

TEST(quick_settings_not_connected)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_start();
	ui_test_wait(1100);
	shade_pull();
	CHECK(ui_test_shows("Flashlight"));
	lv_obj_t *wifi = tile("Wi-Fi");
	CHECK(wifi && lv_obj_has_state(wifi, LV_STATE_CHECKED)); /* on, not connected */
	CHECK(ui_test_long_press_text("Wi-Fi"));
	CHECK(ui_test_wait_for("Networks", 2000));
}

TEST(quick_settings_not_running)
{
	shade_pull();
	CHECK(ui_test_shows("Flashlight"));
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
	CHECK(ui_test_shows("Flashlight")); /* still open */
	lv_obj_t *wifi = tile("Wi-Fi");
	CHECK(wifi && !lv_obj_has_state(wifi, LV_STATE_CHECKED | LV_STATE_DISABLED));
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_wifi_up());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(lv_obj_has_state(tile("Home"), LV_STATE_CHECKED));
}

/* Asleep, Wi-Fi cannot stay connected (the kernel disconnects it): wlan0 goes down before
 * the phone sleeps and up again when it wakes, the chip powered off and on as Wi-Fi off and
 * on do, rather than in the kernel's suspend and resume (on the PinePhone, rtw88's there
 * froze the phone on waking) */
TEST(down_while_asleep)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	CHECK(ui_test_wifi_up());
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300); /* the fade out, the lock screen, the sleep */
	CHECK(*file("/sys/power/state")); /* it slept */
	CHECK_INT(ui_test_wifi_downs(), 1);
	/* Awake: up again, after a moment, so that bringing it up (the chip's firmware, a
	 * few hundred ms on the phone) does not hold back what woke the phone (a call) */
	ui_test_wait(500);
	CHECK(!ui_test_wifi_up());
	ui_test_wait(1800);
	CHECK(ui_test_wifi_up());
}

/* Asleep again before it came up: it stays down, and comes up after the next waking */
TEST(asleep_again_before_it_is_up)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(1300); /* awake, past the second in which presses are ignored */
	CHECK(!ui_test_wifi_up());
	ui_test_write("/sys/power/state", "");
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(300);
	CHECK(*file("/sys/power/state")); /* slept again */
	CHECK_INT(ui_test_wifi_downs(), 1); /* never up in between */
	ui_test_wait(500);
	CHECK(!ui_test_wifi_up());
	ui_test_wait(1800);
	CHECK(ui_test_wifi_up());
}

/* Switched off, it stays off */
TEST(off_stays_off_after_sleep)
{
	ui_test_reset();
	ui_test_write("/sys/power/state", "");
	ui_test_write(SETTINGS, "wifi=0\n");
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_start();
	CHECK(!ui_test_wifi_up());
	int downs = ui_test_wifi_downs();
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(800);
	CHECK(*file("/sys/power/state")); /* it slept */
	CHECK_INT(ui_test_wifi_downs(), downs);
	CHECK(!ui_test_wifi_up());
}

/* Sharing: a QR code as phones' cameras read it (WIFI:T:...;S:...;P:...;;, ZXing's format) */

/* The QR code shown (the white square of ui_qr_create()), NULL if none */
static lv_obj_t *qr_find(lv_obj_t *obj)
{
	if (lv_obj_get_width(obj) > 300 && lv_obj_get_width(obj) == lv_obj_get_height(obj) &&
	    lv_obj_get_style_bg_opa(obj, 0) == LV_OPA_COVER &&
	    lv_color_to_u32(lv_obj_get_style_bg_color(obj, 0)) == 0xffffffff)
		return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = qr_find(lv_obj_get_child(obj, i));
		if (found)
			return found;
	}
	return NULL;
}

/* Whether the screen shows `text`'s QR code: each module's middle dark or light as it
 * should be, the quiet zone light */
static bool shows_qr(const char *text)
{
	uint8_t temp[qrcodegen_BUFFER_LEN_MAX], code[qrcodegen_BUFFER_LEN_MAX];
	lv_obj_t *qr = qr_find(lv_screen_active());
	lv_area_t a;
	if (!qr) {
		printf("no QR code shown\n");
		return false;
	}
	if (!qrcodegen_encodeText(text, temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
	                          qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
		return false;
	ui_test_wait(100);
	lv_obj_get_coords(qr, &a);
	int n = qrcodegen_getSize(code);
	int32_t px = lv_area_get_width(&a) / (n + 2 * UI_QR_QUIET);
	int32_t x0 = a.x1 + (lv_area_get_width(&a) - px * n) / 2;
	int32_t y0 = a.y1 + (lv_area_get_height(&a) - px * n) / 2;
	for (int y = -UI_QR_QUIET; y < n + UI_QR_QUIET; y++)
		for (int x = -UI_QR_QUIET; x < n + UI_QR_QUIET; x++) {
			bool dark = qrcodegen_getModule(code, x, y); /* false outside */
			lv_color_t c = ui_test_pixel(x0 + x * px + px / 2, y0 + y * px + px / 2);
			if ((c.green < 128) != dark) {
				printf("QR module (%d, %d) is %s, not %s as \"%s\" has it\n", x, y,
				       c.green < 128 ? "dark" : "light", dark ? "dark" : "light", text);
				ui_test_screenshot("build/wifi_qr.png");
				return false;
			}
		}
	return true;
}

TEST(share_saved_network)
{
	ui_test_reset(); /* the saved file kept */
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_tap_text("Share this network"));
	CHECK(shows_qr("WIFI:T:WPA;S:Home;P:secret123;;"));
	CHECK(ui_test_shows("secret123")); /* to type it on a device without a camera */
	ui_test_screenshot("build/wifi_share.png");
}

/* Joined here: wpa_supplicant keeps its name in hex */
TEST(share_network_just_joined)
{
	ui_test_wifi_ap("Home", -50, WPA2, "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(!ui_test_shows("Share this network"));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_wait_for("Share this network", 2000));
	CHECK(ui_test_tap_text("Share this network"));
	CHECK(shows_qr("WIFI:T:WPA;S:Home;P:secret123;;"));
}

/* The format's special characters escaped with a backslash */
TEST(share_special_characters)
{
	ui_test_reset(); /* the saved file kept */
	ui_test_wifi_ap("Joe's;Net", -50, WPA2, "a\\b;c,d:e\"f");
	ui_test_wifi_saved("Joe's;Net", "a\\b;c,d:e\"f");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Joe's;Net", 2000));
	CHECK(ui_test_tap_text("Share this network"));
	CHECK(shows_qr("WIFI:T:WPA;S:Joe's\\;Net;P:a\\\\b\\;c\\,d\\:e\\\"f;;"));
}

TEST(share_open_network)
{
	ui_test_reset(); /* the saved file kept */
	ui_test_wifi_ap("Cafe", -50, "[ESS]", NULL);
	ui_test_wifi_saved("Cafe", NULL);
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Connected to Cafe", 2000));
	CHECK(ui_test_tap_text("Share this network"));
	CHECK(shows_qr("WIFI:T:nopass;S:Cafe;;"));
	CHECK(!ui_test_shows("Password"));
}

/* WPA3 only: SAE, which phones' cameras (Android 10 and later) take */
TEST(share_wpa3_network)
{
	ui_test_wifi_ap("Home", -50, "[SAE-CCMP][SAE-H2E][ESS]", "secret123");
	CHECK(open_wifi());
	CHECK(ui_test_wait_for("Home", 2000));
	CHECK(ui_test_tap_text("Home"));
	CHECK(ui_test_type("secret123\n"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_wait_for("Share this network", 2000));
	CHECK(ui_test_tap_text("Share this network"));
	CHECK(shows_qr("WIFI:T:SAE;S:Home;P:secret123;;"));
}
