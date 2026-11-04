/* Settings, Wi-Fi (settings/wifi.c), through the UI (ui_test.h), on the fake
 * wpa_supplicant of fake_wpa.c */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define CONF "/var/lib/wpa_supplicant/wpa_supplicant.conf"
#define WPA2 "[WPA2-PSK-CCMP][ESS]"

TEST_SETUP
{
	ui_test_start();
}

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
