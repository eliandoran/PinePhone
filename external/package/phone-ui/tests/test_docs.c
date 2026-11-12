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
 * The Makefile runs them twice: in the dark theme (the phone's default), then with
 * DOCS_THEME=light in the light one, whose images it names <name>-light.webp. The pages
 * show each in the documentation's own scheme (#only-light, #only-dark).
 *
 * The apps' pages (docs/apps/<app>.md) have theirs in docs/<app>.c, included at the end:
 * tests and images named <app>_... and <app>-..., static helpers too. */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fake_doom/fake_doom.h"
#include "fake_genesis/fake_genesis.h"
#include "fake_tdjson.h"
#include "modem/modem.h"
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

/* The light theme's run (DOCS_THEME=light) */
static bool light_theme(void)
{
	const char *theme = getenv("DOCS_THEME");
	return theme && strcmp(theme, "light") == 0;
}

TEST_SETUP
{
	ui_test_reset();
	seed(test_name);
	if (light_theme())
		CHECK(ui_test_write("/var/lib/phone-ui/settings", "light=1\n"));
	/* The PinePhone's ims=off, the modem's already: Settings › Network's switch */
	FILE *conf = fopen("/tmp/modem.conf", "a");
	fprintf(conf, "ims=off\n");
	fclose(conf);
	ui_test_modem_ims(2, false);
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
	ui_test_bt_running(false); /* off, but there */
	ui_test_swipe(360, 20, 360, 900, 600);
	ui_test_settle();
	CHECK(ui_test_shows("Flashlight"));
	CHECK(ui_test_shows("Home"));        /* the Wi-Fi network */
	CHECK(ui_test_shows("Bluetooth"));
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
#include "docs/videos.c"
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
#include "docs/snake.c"
#include "docs/terminal.c"
#include "docs/settings.c"

/* External apps, docs/external/<app>.md: each installed for its own tests only (seed()), as
 * `just app <app>` does, from the host builds the Makefile makes for their tests */

static bool plugin_copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
	char buf[65536];
	size_t n;
	bool ok = in && out;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if (in)
		fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	return ok;
}

/* The app built in `from` (its app.json, `so`, and `lib`, in lib/, unless NULL) into
 * /var/lib/apps/<id> */
static void plugin_install(const char *from, const char *id, const char *so, const char *lib)
{
	char src[512], dst[512];
	bool ok = true;
	const char *files[] = { "app.json", so, lib };
	for (int i = 0; i < 3 && ok; i++) {
		if (!files[i])
			continue;
		snprintf(src, sizeof(src), "%s/%s%s", from, i == 2 ? "lib/" : "", files[i]);
		snprintf(dst, sizeof(dst), "/var/lib/apps/%s/%s%s", id, i == 2 ? "lib/" : "", files[i]);
		ok = ui_test_write(dst, "") && plugin_copy(src, dst);
	}
	if (!ok)
		TEST_FAIL("cannot install %s", from);
}

#include "docs/telegram.c"
#include "docs/github.c"
#include "docs/trilium.c"
#include "docs/assistant.c"
#include "docs/doom.c"
#include "docs/genesis.c"

/* notifications.md: the lock screen with notifications, Maya's messages and a missed call */
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
	} else if (strncmp(test, "tasks_", 6) == 0) {
		tasks_seed(); /* read as it starts, for its reminders */
	} else if (strncmp(test, "telegram_", 9) == 0) {
		telegram_seed();
	} else if (strncmp(test, "trilium_", 8) == 0) {
		trilium_seed();
	} else if (strncmp(test, "github_", 7) == 0) {
		github_seed();
	} else if (strncmp(test, "assistant_", 10) == 0) {
		assistant_seed();
	} else if (strncmp(test, "doom_", 5) == 0) {
		doom_seed();
	} else if (strncmp(test, "genesis_", 8) == 0) {
		genesis_seed();
	}
}
