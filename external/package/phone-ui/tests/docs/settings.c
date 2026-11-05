/* docs/apps/settings.md's screenshots (included by test_docs.c) */

#define SETTINGS_ROUTE "/tmp/route" /* PROC_NET_ROUTE of wifi.o, the Makefile's */
#define SETTINGS_HISTORY "/var/lib/phone-ui/battery-history"

/* The switch of the open page's row `title`, NULL if none */
static lv_obj_t *settings_row_switch(const char *title)
{
	lv_obj_t *label = ui_test_find(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++)
		if (lv_obj_check_type(lv_obj_get_child(row, (int32_t)i), &lv_switch_class))
			return lv_obj_get_child(row, (int32_t)i);
	return NULL;
}

static bool settings_switch_on(const char *title)
{
	lv_obj_t *sw = settings_row_switch(title);
	return sw && lv_obj_has_state(sw, LV_STATE_CHECKED);
}

/* A few more networks around "Home", and its router */
static void settings_networks(void)
{
	ui_test_wifi_ap("Cafe Corner", -62, WPA2, "cornercafe");
	ui_test_wifi_ap("Library", -70, "[ESS]", NULL);
	ui_test_wifi_ap("Office", -78, "[WPA2-EAP-CCMP][ESS]", NULL);
	ui_test_write(SETTINGS_ROUTE,
	              "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
	              "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
}

/* settings.md: the list of sections, each with its value */
TEST(settings_list)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_shows("Display"));
	CHECK(ui_test_shows("50%"));     /* brightness */
	CHECK(ui_test_shows("Sound & vibration"));
	CHECK(ui_test_shows("Vibrate"));
	CHECK(ui_test_shows("Date & time"));
	CHECK(ui_test_shows("UTC"));
	CHECK(ui_test_shows("Battery"));
	CHECK(ui_test_shows("76%"));
	CHECK(ui_test_shows("Wi-Fi"));
	CHECK(ui_test_shows("Home"));
	CHECK(ui_test_shows("Network"));
	CHECK(ui_test_shows("Storage"));
	CHECK(ui_test_shows("About phone"));
	CHECK(ui_test_shows("PinePhone"));
	CHECK(ui_test_shows("Diagnostics"));
	shot("settings-list");
}

/* settings.md, Wi-Fi: connected to Home, its details, the networks around */
TEST(settings_wifi)
{
	settings_networks();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_wait_for("Connected to Home", 2000));
	CHECK(ui_test_wait_for("Cafe Corner", 3000));
	lv_obj_t *wifi_switch = ui_test_find_widget(&lv_switch_class); /* the only one */
	CHECK(wifi_switch && lv_obj_has_state(wifi_switch, LV_STATE_CHECKED));
	CHECK(ui_test_shows("192.168.1.42"));
	CHECK(ui_test_shows("192.168.1.1"));
	CHECK(ui_test_shows("2.4 GHz, channel 6"));
	CHECK(ui_test_shows("Excellent (-45 dBm)"));
	CHECK(ui_test_shows("WPA2"));
	CHECK(ui_test_shows("Forget this network"));
	CHECK(ui_test_shows("Networks"));
	CHECK(ui_test_shows("Secured \xc2\xb7 Good"));  /* Cafe Corner */
	CHECK(ui_test_shows("Open \xc2\xb7 Fair"));     /* Library */
	CHECK(ui_test_shows("Not supported \xc2\xb7 Weak")); /* Office: WPA2 Enterprise */
	ui_test_settle();
	shot("settings-wifi");
}

/* settings.md, Wi-Fi: the password of a new network, then connected and saved */
TEST(settings_wifi_password)
{
	settings_networks();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Wi-Fi"));
	CHECK(ui_test_wait_for("Cafe Corner", 3000));
	CHECK(ui_test_tap_text("Cafe Corner"));
	ui_test_settle();
	CHECK(ui_test_shows("Password"));
	/* Too short: refused, the page stays */
	CHECK(ui_test_type("short\n"));
	CHECK(ui_test_shows("At least 8 characters"));
	CHECK(ui_test_back());
	ui_test_settle();
	CHECK(ui_test_tap_text("Cafe Corner"));
	ui_test_settle();
	CHECK(!ui_test_shows("At least 8 characters"));
	CHECK(ui_test_type("cornercafe"));
	ui_test_settle();
	shot("settings-wifi-password");
	CHECK(ui_test_type("\n"));
	CHECK(ui_test_wait_for("Connected to Cafe Corner", 3000));
	CHECK(ui_test_wait_for("Saved", 3000)); /* Home, kept */
}

/* settings.md, Network: connections, Wi-Fi, the mobile network, data off by default, the APN */
TEST(settings_network)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Network"));
	ui_test_settle();
	CHECK(ui_test_shows("Connections"));
	CHECK(ui_test_shows("Bluetooth"));
	CHECK(ui_test_shows("Fake Mobile"));
	CHECK(ui_test_find("Mobile data") && !settings_switch_on("Mobile data"));
	CHECK(ui_test_find("Data roaming") && !settings_switch_on("Data roaming"));
	CHECK(ui_test_shows(UI_TEST_NUMBER));
	CHECK(ui_test_shows(UI_TEST_IMEI));
	CHECK(ui_test_shows("Access point name (APN)"));
	lv_obj_t *apn = ui_test_find_widget(&lv_textarea_class);
	CHECK(apn && strcmp(lv_textarea_get_placeholder_text(apn), "Automatic") == 0);
	CHECK(apn && lv_textarea_get_text(apn)[0] == '\0'); /* the network's own */
	shot("settings-network");
}

/* settings.md, Display: brightness at half, the dark theme */
TEST(settings_display)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	ui_test_settle();
	CHECK(ui_test_shows("Screen brightness"));
	CHECK(ui_test_shows("50%"));
	CHECK(ui_test_find_widget(&lv_slider_class));
	CHECK(ui_test_shows("Dark"));
	CHECK(ui_test_shows("Light"));
	CHECK(!ui_light);
	CHECK(ui_test_shows("720 x 1440"));
	shot("settings-display");
}

/* settings.md, Battery: the charge, the last day (charged last evening), details */
TEST(settings_battery)
{
	time_t now = time(NULL);
	char history[512];
	snprintf(history, sizeof(history),
	         "%lld 58 D\n%lld 22 D\n%lld 22 C\n%lld 100 C\n%lld 100 D\n%lld 92 D\n",
	         (long long)(now - 86400), (long long)(now - 14 * 3600), (long long)(now - 14 * 3600 + 60),
	         (long long)(now - 11 * 3600), (long long)(now - 9 * 3600), (long long)(now - 2 * 3600));
	ui_test_write(SETTINGS_HISTORY, history);
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Battery"));
	ui_test_settle();
	CHECK(ui_test_shows("76%"));
	CHECK(ui_test_shows("Discharging"));
	CHECK(ui_test_find("Show percentage in status bar") && !settings_switch_on("Show percentage in status bar"));
	CHECK(ui_test_shows("Last 24 hours"));
	CHECK(ui_test_shows("Good"));
	CHECK(ui_test_shows("3.85 V"));
	CHECK(ui_test_shows("-310 mA"));
	CHECK(ui_test_shows("Not connected")); /* the charger */
	CHECK(ui_test_shows("2750 mAh"));
	shot("settings-battery");
}
