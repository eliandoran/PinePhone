/* The user documentation's screenshots (docs/images/generated/, `just docs-shots`): one
 * test per image, which gets to the screen as a user would and saves it. Not one of the
 * tests `just test` runs (the Makefile's docs-shots target), but a test all the same: it
 * fails when the screen no longer shows what the documentation describes.
 *
 * Every screen is the same moment of the same phone: the wall clock set (Saturday
 * 14 March 2026, 09:41), registered on "Fake Mobile" with LTE, on the Wi-Fi network
 * "Home", the battery at 76% (ui_test_reset()'s sysfs). Images go to $DOCS_SHOTS as
 * <name>.png.
 *
 * The apps' pages (docs/apps/<app>.md) have theirs in docs/<app>.c, included at the end:
 * tests and images named <app>_... and <app>-..., static helpers too. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "modem.h"
#include "test.h"
#include "ui_test.h"

#define WPA2 "[WPA2-PSK-CCMP][ESS]"
#define NOW 1773481260 /* Saturday 14 March 2026, 09:41:00 UTC */

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

TEST_SETUP
{
	ui_test_reset();
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
	CHECK(ui_test_shows("Vibration"));
	CHECK(ui_test_shows("Home"));        /* the Wi-Fi network */
	CHECK(ui_test_shows("Fake Mobile")); /* the operator */
	shot("quick-settings");
}

/* mobile.md: roaming with mobile data and data roaming on: "R" and "4G" in the status bar,
 * the operator in the quick settings */
TEST(mobile_roaming)
{
	modem_data_set_roaming(true);
	modem_data_set_enabled(true);
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Fake Mobile");
	for (int i = 0; i < 100 && (!modem_roaming() || modem_data_state() != MODEM_DATA_CONNECTED); i++) {
		ui_test_wait(100);
		nanosleep(&(struct timespec){ 0, 10000000 }, NULL); /* the fake modem answers in real time */
	}
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
#include "docs/recorder.c"
#include "docs/files.c"
#include "docs/maps.c"
#include "docs/weather.c"
#include "docs/compass.c"
#include "docs/browser.c"
#include "docs/assistant.c"
#include "docs/snake.c"
#include "docs/settings.c"
