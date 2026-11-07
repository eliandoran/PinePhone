/* The user documentation's screenshots (docs/images/generated/, `just docs-shots`): one
 * test per image, which gets to the screen as a user would and saves it. Not one of the
 * tests `just test` runs (the Makefile's docs-shots target), but a test all the same: it
 * fails when the screen no longer shows what the documentation describes.
 *
 * Every screen is the same moment of the same phone: the wall clock set (Saturday
 * 14 March 2026, 09:41), registered on "Fake Mobile" with LTE, on the Wi-Fi network
 * "Home", the battery at 76% (ui_test_reset()'s sysfs). Images go to $DOCS_SHOTS as
 * <name>.png, which the Makefile turns into docs/images/generated/<name>.webp.
 *
 * The apps' pages (docs/apps/<app>.md) have theirs in docs/<app>.c, included at the end:
 * tests and images named <app>_... and <app>-..., static helpers too. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modem.h"
#include "test.h"
#include "ui_test.h"
#include "ui_symbols.h"

#define WPA2 "[WPA2-PSK-CCMP][ESS]"
#define NOW 1773481260 /* Saturday 14 March 2026, 09:41:00 UTC */

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

static void seed(const char *test);

TEST_SETUP
{
	ui_test_reset();
	seed(test_name);
	/* Since unplugged (Settings, Battery), read at startup: 1 h 55 min on, 7 h 10 min asleep */
	ui_test_write("/var/lib/phone-ui/screen-time", "6900000 25800000 0 0 0\n");
	ui_test_wall_clock(NOW);
	ui_test_wifi_ap("Home", -45, WPA2, "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_modem_signal(-85, UI_TEST_TECH_LTE);
	ui_test_start();
	CHECK(ui_test_wait_until(registered, 10000));
	ui_test_wall_clock(NOW); /* the time set up took: 09:41 again */
	ui_test_settle();
}

/* The screen as $DOCS_SHOTS/<name>.png */
static void shot(const char *name)
{
	const char *dir = getenv("DOCS_SHOTS");
	char path[512];
	CHECK(dir != NULL);
	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	ui_test_wait(500); /* the status bar's last updates drawn */
	CHECK(ui_test_screenshot(path));
}

/* index.md: the home screen */
TEST(home)
{
	CHECK(ui_test_shows("09:41"));
	CHECK(ui_test_shows("Saturday 14 March"));
	shot("home");
}

/* index.md: the quick settings, pulled down from the status bar */
TEST(quick_settings)
{
	ui_test_swipe(360, 20, 360, 900, 600);
	ui_test_settle();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(ui_test_shows("Home"));        /* the Wi-Fi network */
	CHECK(ui_test_shows("Fake Mobile")); /* the operator */
	shot("quick-settings");
}

static bool roaming_connected(void)
{
	return modem_roaming() && modem_data_state() == MODEM_DATA_CONNECTED;
}

/* mobile.md: roaming with mobile data and data roaming on: "R" and "4G" in the status bar,
 * the operator in the quick settings */
TEST(mobile_roaming)
{
	modem_data_set_roaming(true);
	modem_data_set_enabled(true);
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Fake Mobile");
	ui_test_wait_until(roaming_connected, 10000);
	CHECK(modem_roaming());
	CHECK(modem_data_state() == MODEM_DATA_CONNECTED);
	ui_test_swipe(360, 20, 360, 900, 600);
	ui_test_settle();
	CHECK(ui_test_shows("R"));
	CHECK(ui_test_shows("Fake Mobile (roaming)"));
	CHECK(ui_test_shows("4G"));
	shot("mobile-roaming");
}

/* docs/apps/<app>.md */
#include "docs/phone.c"
#include "docs/messages.c"
#include "docs/contacts.c"
#include "docs/camera.c"
#include "docs/gallery.c"
#include "docs/music.c"
#include "docs/clock.c"
#include "docs/calendar.c"
#include "docs/calculator.c"
#include "docs/notes.c"
#include "docs/tasks.c"
#include "docs/recorder.c"
#include "docs/files.c"
#include "docs/maps.c"
#include "docs/weather.c"
#include "docs/compass.c"
#include "docs/web_lite.c"
#include "docs/books.c"
#include "docs/assistant.c"
#include "docs/snake.c"
#include "docs/settings.c"

/* index.md: the lock screen with notifications, Maya's messages and a missed call */
TEST(lock_notifications)
{
	ui_lock();
	ui_test_settle();
	CHECK(ui_test_shows("Maya Okafor"));
	CHECK(ui_test_shows("Save me a coffee \xe2\x98\x95"));
	CHECK(ui_test_shows("Daniel Reyes"));
	CHECK(ui_test_shows("Missed call"));
	shot("lock-notifications");
}

/* What phone-ui reads at startup (the notifications: conversations, the call log), written
 * before it starts: the data of the app whose images the test makes */
static void seed(const char *test)
{
	if (strcmp(test, "lock_notifications") == 0) {
		messages_contacts();
		messages_stored();
		char line[256];
		snprintf(line, sizeof(line), "{ \"number\": \"+447700900456\", \"time\": %lld, "
		         "\"kind\": \"missed\", \"duration\": 0, \"new\": true }\n", (long long)(NOW - 25 * 60));
		ui_test_write("/var/lib/phone/calls.jsonl", line);
	} else if (strncmp(test, "messages_", 9) == 0) {
		messages_contacts();
		messages_stored();
	} else if (strncmp(test, "phone_", 6) == 0) {
		phone_contacts();
		phone_calls();
	}
}
