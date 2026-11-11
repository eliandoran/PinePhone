/* The modem (modem.c) through the UI (ui_test.h): whether there is one and the SIM's
 * state, in the status bar and Settings › Network, on the fake modem of fake_modem.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "modem/modem.h"
#include "ui_symbols.h"
#include "test.h"
#include "ui_test.h"

#define OPERATOR "Fake Mobile" /* fake_modem.c's, registered unless told otherwise */
#define CONF "/tmp/modem.conf" /* MODEM_CONF of modem.o, the Makefile's */

/* The board's modem.conf (ui_test_reset(): the PinePhone's, its port the fake's) can be
 * changed before starting */
TEST_SETUP
{
	ui_test_reset();
}

/* Settings › Network shows `text` (its Mobile section's Network row) */
static bool settings_shows(const char *text)
{
	return ui_test_open("Settings") && ui_test_tap_text("Network") && ui_test_shows(text);
}

/* The shade (pulled down from the status bar, opened by tapping it) shows `text`
 * within `ms`: the network's row, the operator's name. Closed again after */
static bool shade_shows(const char *text, uint32_t ms)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
	bool shown = ui_test_wait_for(text, ms);
	ui_test_tap_at(360, UI_STATUS_H / 2);
	return shown;
}

/* The status bar's badge after the signal bars, what is wrong with the modem: no
 * modem or a SIM error (warning), no SIM (sim-card), SIM locked or blocked (lock), no
 * service (close); NULL when none shows */
static const char *const badges[] = { LV_SYMBOL_WARNING, UI_SYMBOL_SIM_CARD, UI_SYMBOL_LOCK, LV_SYMBOL_CLOSE };

static lv_obj_t *badge(void)
{
	for (unsigned i = 0; i < sizeof(badges) / sizeof(badges[0]); i++) {
		lv_obj_t *found[4];
		uint32_t n = ui_test_find_all(badges[i], found, 4);
		for (uint32_t j = 0; j < LV_MIN(n, 4); j++) {
			lv_area_t a;
			lv_obj_get_coords(found[j], &a);
			if (a.y2 <= UI_STATUS_H)
				return found[j];
		}
	}
	return NULL;
}

static const char *badge_text(void)
{
	lv_obj_t *b = badge();
	return b ? lv_label_get_text(b) : "";
}

/* The badge is `symbol` ("" none) within `ms`: the fake answers in real time */
static bool badge_becomes(const char *symbol, uint32_t ms)
{
	for (uint32_t t = 0;; t += 48) {
		if (strcmp(badge_text(), symbol) == 0)
			return true;
		if (t >= ms) {
			fprintf(stderr, "  badge \"%s\", expected \"%s\"\n", badge_text(), symbol);
			return false;
		}
		ui_test_wait(48);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
}

/* Red: what will not get better by waiting (no modem, SIM blocked) */
static bool badge_red(void)
{
	lv_obj_t *b = badge();
	return b && lv_color_eq(lv_obj_get_style_text_color(b, 0), lv_color_hex(UI_COLOR_RED));
}

/* None of the modem's states as text in the status bar: icons there, words in the
 * shade and Settings */
static bool no_state_text(void)
{
	static const char *const texts[] = { "No modem", "Modem...", "No SIM", "SIM locked", "SIM blocked",
		                             "SIM error", "SIM ready", "Searching...", "No service" };
	for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
		if (ui_test_shows(texts[i])) {
			fprintf(stderr, "  \"%s\" shown\n", texts[i]);
			return false;
		}
	return true;
}

TEST(no_modem)
{
	ui_test_start();
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 0));
	CHECK(badge_red());
	CHECK(no_state_text());
	CHECK(settings_shows("No modem"));
}

TEST(no_sim)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(!badge_red());
	CHECK(no_state_text());
	CHECK(settings_shows("No SIM card"));
}

/* The signal bars' slot (30 px, 16 px left of the battery, the bar's padding) */
#define SLOT_X1 (720 - 20 - 40 - 16 - 30)
#define SLOT_X2 (720 - 20 - 40 - 16 - 1)

/* In place of the signal bars (all dim when it shows), centered in their slot */
TEST(badge_in_place_of_the_bars)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	ui_test_wait(1100);
	lv_obj_t *b = badge();
	CHECK(b);
	if (!b)
		return;
	lv_area_t a;
	lv_obj_get_coords(b, &a);
	CHECK(abs((a.x1 + a.x2) / 2 - (SLOT_X1 + SLOT_X2) / 2) <= 1);
	/* No bars drawn under it: the first bar's place, left of the badge's ink, is the
	 * wallpaper as just left of the slot */
	lv_color_t c = ui_test_pixel(SLOT_X1 + 2, UI_STATUS_H / 2 + 10), w = ui_test_pixel(SLOT_X1 - 4, UI_STATUS_H / 2 + 10);
	CHECK(abs(c.red - w.red) < 0x10 && abs(c.green - w.green) < 0x10 && abs(c.blue - w.blue) < 0x10);
}

/* How many of the four bars are lit as drawn: a bar's lower rows (the first, shortest,
 * included) much brighter than the wallpaper just left of the slot, a dim bar only a
 * little (30%). Bars 5 px wide, 3 px apart */
static int lit_bars(void)
{
	int32_t y = UI_STATUS_H / 2 + 10;
	lv_color_t w = ui_test_pixel(SLOT_X1 - 4, y);
	int lit = 0;
	for (int i = 0; i < 4; i++) {
		lv_color_t c = ui_test_pixel(SLOT_X1 + i * 8 + 2, y);
		if (c.red + c.green + c.blue - (w.red + w.green + w.blue) > 300)
			lit++;
	}
	return lit;
}

/* The bars lit as seen over `ms`, a frame each 100 ms: a mask of the counts seen
 * (bit n: n bars lit) */
static unsigned lit_bars_seen(uint32_t ms)
{
	unsigned seen = 0;
	for (uint32_t t = 0; t < ms; t += 100) {
		ui_test_wait(100);
		seen |= 1u << lit_bars();
	}
	return seen;
}

/* While the modem starts or looks for a network, the bars light up one after another
 * over and over (something is going on); still once it is registered */
TEST(bars_dance_while_starting)
{
	ui_test_start();
	ui_test_modem_silent(true);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Starting", 3000));
	unsigned seen = lit_bars_seen(2000);
	CHECK_INT(seen, 0x1f); /* 0, 1, 2, 3 and 4 bars, each in turn */
	ui_test_modem_silent(false);
	CHECK(shade_shows(OPERATOR, 15000));
	ui_test_wait(1100);
	CHECK_INT(lit_bars_seen(2000), 1u << 4); /* -80 dBm: all four, steady */
}

TEST(bars_dance_while_searching)
{
	ui_test_start();
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Searching", 3000));
	CHECK_INT(lit_bars_seen(2000), 0x1f);
}

TEST(sim_ready_registered)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	/* Not in the status bar: nothing there while all is well */
	CHECK(!ui_test_shows(OPERATOR));
	CHECK(badge_becomes("", 0));
	CHECK(no_state_text());
	CHECK(settings_shows(OPERATOR));
}

/* A problem stays in the status bar, and the shade says it in full */
TEST(no_sim_in_the_shade)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(shade_shows("No SIM card", 3000));
}

/* The modem's state on the status bar's left while the shade is out, where the
 * notifications are otherwise */
TEST(state_in_the_bar_while_the_shade_is_out)
{
	ui_test_start();
	CHECK(!ui_test_shows("No modem"));
	ui_test_tap_at(360, UI_STATUS_H / 2);
	CHECK(ui_test_wait_for("No modem", 2000));
	lv_obj_t *label = ui_test_find("No modem");
	CHECK(label);
	if (!label)
		return;
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	CHECK(a.y2 <= UI_STATUS_H && a.x1 < 60);
}

/* Without a modem, the shade has no row for it */
TEST(no_modem_row_without_a_modem)
{
	remove(CONF);
	ui_test_start();
	ui_test_tap_at(360, UI_STATUS_H / 2);
	CHECK(ui_test_shows("Not connected")); /* the USB row: the shade is open */
	CHECK(!ui_test_shows("Not available"));
	CHECK(!ui_test_shows("No modem"));
}

TEST(sim_pin)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_PIN);
	CHECK(badge_becomes(UI_SYMBOL_LOCK, 3000));
	CHECK(!badge_red());
	CHECK(no_state_text());
	CHECK(settings_shows("SIM locked (PIN)"));
}

/* The SIM failing (+CME ERROR: 13): an error, not a missing card */
TEST(sim_failure)
{
	ui_test_start();
	ui_test_modem_sim_answer("+CME ERROR: 13");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 3000));
	CHECK(settings_shows("SIM error"));
}

/* No card, as modems say it in words (no AT+CMEE=1) and as a state */
TEST(no_sim_in_words)
{
	ui_test_start();
	ui_test_modem_sim_answer("+CME ERROR: SIM not inserted");
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(settings_shows("No SIM card"));
}

TEST(no_sim_as_a_state)
{
	ui_test_start();
	ui_test_modem_sim_answer("+CPIN: NOT INSERTED");
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(settings_shows("No SIM card"));
}

/* The card not ready yet (or no answer but OK): asked again until it is */
TEST(sim_not_ready_at_first)
{
	ui_test_start();
	ui_test_modem_sim_answer("+CPIN: NOT READY");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 10000));
}

TEST(sim_state_unsaid_at_first)
{
	ui_test_start();
	ui_test_modem_sim_answer("");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 10000));
}

TEST(sim_puk)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_PUK);
	CHECK(badge_becomes(UI_SYMBOL_LOCK, 3000));
	CHECK(badge_red());
	CHECK(no_state_text());
	CHECK(settings_shows("SIM blocked (PUK)"));
}

/* The modem says when the card goes and comes back (+QSIMSTAT) */
TEST(sim_removed_and_inserted)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
}

/* Busy just after power-up (+CME ERROR: 14): asked again */
TEST(sim_busy_at_first)
{
	ui_test_start();
	ui_test_modem_sim_busy(2);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 10000));
}

/* modem.conf's boot_wait=: the board powers the modem at boot and it shows up within
 * that many seconds of it, `s` from now */
static void boot_wait_in(long s)
{
	struct timespec now;
	clock_gettime(CLOCK_BOOTTIME, &now); /* the tests' clock */
	FILE *f = fopen(CONF, "a");
	fprintf(f, "boot_wait=%ld\n", (long)now.tv_sec + s);
	fclose(f);
}

/* Just after boot the modem is not on USB yet (about 12 s after the board powered it):
 * starting, not missing */
TEST(starting_after_boot)
{
	boot_wait_in(10);
	ui_test_start();
	CHECK(shade_shows("Starting", 2000));
	ui_test_wait(3000);
	CHECK(badge_becomes("", 0));
	CHECK(!ui_test_shows("No modem"));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
}

/* Not there by then: missing (powered off, its switch off) */
TEST(no_modem_after_boot_wait)
{
	boot_wait_in(3);
	ui_test_start();
	CHECK(badge_becomes("", 0));
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 5000));
	CHECK(settings_shows("No modem"));
}

/* Started after it (phone-ui restarted), no modem shows at once */
TEST(no_modem_long_after_boot)
{
	boot_wait_in(-1);
	ui_test_start();
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 2000));
}

/* The port is there but the modem does not answer yet (still starting) */
TEST(not_answering)
{
	ui_test_start();
	ui_test_modem_silent(true);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Starting", 3000));
	ui_test_wait(6000);
	/* The bars, unlit, and nothing else: as before a network is found */
	CHECK(badge_becomes("", 0));
	CHECK(no_state_text());
	CHECK(settings_shows("Starting"));
	ui_test_modem_silent(false);
	CHECK(shade_shows(OPERATOR, 15000));
}

/* The modem goes from USB (powered off, restarted) and comes back */
TEST(unplugged_and_back)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem_unplug();
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 5000));
}

/* It restarted on its own ("RDY", echo on again): set up and asked again */
TEST(restarted)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem_restart(UI_TEST_SIM_PIN);
	CHECK(badge_becomes(UI_SYMBOL_LOCK, 3000));
}

/* USB 10 px left of the bars' slot as the icons are, as wide as its ink (17 px,
 * upright), and put through the modem's states: starting, no SIM, registered */
TEST(usb_symbol_stays_through_the_modem_states)
{
	ui_test_write("/sys/class/net/usb0/carrier", "1\n");
	ui_test_start();
	ui_test_wait(1100);
	lv_obj_t *usb = ui_test_find(UI_SYMBOL_USB_270);
	CHECK(usb);
	if (!usb)
		return;
	lv_area_t b;
	lv_obj_get_coords(usb, &b);
	CHECK_INT(b.x2, SLOT_X1 - 10 - 1); /* starting: the bars dim */
	CHECK(lv_area_get_width(&b) <= 18);
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	ui_test_wait(1100);
	lv_obj_get_coords(usb, &b);
	CHECK_INT(b.x2, SLOT_X1 - 10 - 1);
	ui_test_modem_restart(UI_TEST_SIM_READY);
	CHECK(badge_becomes("", 3000));
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_wait(1100);
	lv_obj_get_coords(usb, &b);
	CHECK_INT(b.x2, SLOT_X1 - 10 - 1);
}


/* The open page's row `title`: its title's label, the last showing it (a row's
 * "Network" below the page's), NULL if none */
static lv_obj_t *row_title(const char *title)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(title, found, 8);
	return n ? found[LV_MIN(n, 8) - 1] : NULL;
}

/* The value of the open page's row `title`, NULL if none */
static const char *page_value(const char *title)
{
	lv_obj_t *label = row_title(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++) {
		lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
		if (child != label && lv_obj_check_type(child, &lv_label_class))
			return lv_label_get_text(child);
	}
	return NULL;
}

/* The value of Settings › Network's row `title`, NULL if none */
static const char *row_value(const char *title)
{
	if (!ui_test_open("Settings") || !ui_test_tap_text("Network"))
		return NULL;
	return page_value(title);
}

/* A device without a modem (no modem.conf): nothing in the status bar, none probed */
TEST(no_modem_on_this_device)
{
	remove(CONF);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY); /* a port there, not asked */
	ui_test_wait(2000);
	CHECK(badge_becomes("", 0));
	CHECK(!ui_test_shows(OPERATOR));
	CHECK(no_state_text());
	const char *value = row_value("Network");
	CHECK(value);
	if (value)
		CHECK_STR(value, "Not available");
}

/* A provider phone-ui does not have: as without a modem */
TEST(unknown_provider)
{
	ui_test_write(CONF, "provider=qmi\nport=/tmp/modem\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(2000);
	CHECK(!ui_test_shows(OPERATOR));
	const char *value = row_value("Network");
	CHECK(value);
	if (value)
		CHECK_STR(value, "Not available");
}

/* The port is the board's: none other is opened */
TEST(port_from_the_config)
{
	ui_test_write(CONF, "# elsewhere\nprovider=at\nport=/tmp/other\nquirks=quectel\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(2000);
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 0));
	CHECK(symlink("/tmp/modem", "/tmp/other") == 0);
	CHECK(shade_shows(OPERATOR, 3000));
	remove("/tmp/other");
}

/* A modem without Quectel's reports (no quirks): the SIM asked for again while ready,
 * a card removed found out that way */
TEST(standard_modem_asked_again)
{
	ui_test_write(CONF, "provider=at\nport=/tmp/modem\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT); /* no +QSIMSTAT: not asked for */
	ui_test_wait(1000);
	CHECK(strcmp(badge_text(), UI_SYMBOL_SIM_CARD) != 0); /* the network lost (+CREG: 0), the card not known gone yet */
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 15000));
}

/* Network registration */

TEST(searching_then_registered)
{
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Searching", 3000));
	CHECK(badge_becomes("", 0)); /* the bars, unlit */
	CHECK(no_state_text());
	CHECK(settings_shows("Searching"));
	ui_test_modem_network(UI_TEST_NET_HOME, "Orange F"); /* +CREG: 1, then asked +COPS? */
	CHECK(shade_shows("Orange F", 3000));
}

TEST(not_registered)
{
	ui_test_modem_network(UI_TEST_NET_NONE, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(badge_becomes(LV_SYMBOL_CLOSE, 3000));
	CHECK(!badge_red());
	CHECK(no_state_text());
	const char *value = row_value("Network"); /* not the status bar's */
	CHECK(value);
	if (value)
		CHECK_STR(value, "No service");
}

TEST(registration_denied)
{
	ui_test_modem_network(UI_TEST_NET_DENIED, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(badge_becomes(LV_SYMBOL_CLOSE, 3000));
	CHECK(settings_shows("No service (denied)"));
}

TEST(roaming)
{
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Vodafone (roaming)", 3000));
	CHECK(settings_shows("Vodafone (roaming)"));
}

/* LTE only: +CREG says not registered, +CEREG does */
TEST(registered_on_lte_only)
{
	ui_test_modem_lte_only(true);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
}

/* Lost, found again: on another network, its name asked again */
TEST(network_lost_and_another)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	CHECK(shade_shows("Searching", 3000));
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	CHECK(shade_shows("Vodafone (roaming)", 3000));
}

/* The SIM goes: its state again, not the network's */
TEST(sim_removed_while_registered)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(!ui_test_shows(OPERATOR));
}

/* Signal: bars and technology */

/* Waits until the modem's bars are `bars`: the fake answers in real time */
static bool bars_become(int bars, uint32_t ms)
{
	for (uint32_t t = 0; t <= ms; t += 48) {
		if (modem_signal_bars() == bars)
			return true;
		ui_test_wait(48);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	fprintf(stderr, "  bars %d, expected %d\n", modem_signal_bars(), bars);
	return false;
}

TEST(lte_bars_and_4g)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
	CHECK(bars_become(4, 3000)); /* -80 dBm RSRP */
}

/* The technology is in the status bar while the shade is out, beside the bars, the
 * network's name on the left: otherwise only while mobile data is up
 * (test_mobile_data.c), as Android */
TEST(technology_while_the_shade_is_out)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(bars_become(4, 8000));
	ui_test_wait(1100);
	CHECK(!ui_test_shows("4G"));
	ui_test_tap_at(360, UI_STATUS_H / 2);
	CHECK(ui_test_wait_for("4G", 1000));
	lv_obj_t *tech = ui_test_find("4G"), *name = ui_test_find(OPERATOR);
	CHECK(tech && name);
	if (!tech || !name)
		return;
	lv_area_t t, n;
	lv_obj_get_coords(tech, &t);
	lv_obj_get_coords(name, &n);
	CHECK(t.y2 <= UI_STATUS_H && n.y2 <= UI_STATUS_H);
	CHECK(n.x1 < 60 && t.x1 > 360);
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	CHECK(ui_test_wait_for("Searching", 3000));
	CHECK(!ui_test_shows("4G"));
}

/* Android's RSRP thresholds: -85, -95, -105, -115 dBm; asked again every 10 s */
TEST(lte_thresholds)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(bars_become(4, 3000));
	static const int levels[][2] = { { -90, 3 }, { -100, 2 }, { -110, 1 }, { -120, 0 }, { -85, 4 } };
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		ui_test_modem_signal(levels[i][0], UI_TEST_TECH_LTE);
		CHECK(bars_become(levels[i][1], 15000));
	}
}

TEST(umts_3g)
{
	ui_test_modem_signal(-90, UI_TEST_TECH_UMTS); /* RSCP */
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("3G", 3000));
	CHECK(bars_become(3, 3000));
}

TEST(hspa_3g)
{
	ui_test_modem_signal(-80, UI_TEST_TECH_HSPA);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("3G", 3000));
}

/* GSM: RxLev, GSM's thresholds (-89, -97, -103, -107 dBm) */
TEST(gsm_2g)
{
	ui_test_modem_signal(-100, UI_TEST_TECH_GSM);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("2G", 3000));
	CHECK(bars_become(2, 3000));
}

TEST(edge_2g)
{
	ui_test_modem_signal(-80, UI_TEST_TECH_EDGE);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("2G", 3000));
}

TEST(nr_5g)
{
	ui_test_modem_signal(-80, UI_TEST_TECH_NR_NSA);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("5G", 3000));
	CHECK(bars_become(4, 3000));
}

/* Without AT+CESQ: AT+CSQ's RSSI */
TEST(csq_without_cesq)
{
	ui_test_modem_no_cesq();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
	CHECK(bars_become(4, 3000)); /* RSSI 16: -81 dBm */
	ui_test_modem_signal(-102, UI_TEST_TECH_LTE);
	CHECK(bars_become(2, 15000));
}

/* From LTE to UMTS without the registration changing: asked again */
TEST(handover_to_3g)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
	ui_test_modem_signal(-90, UI_TEST_TECH_UMTS);
	CHECK(shade_shows("3G", 15000));
	CHECK(!shade_shows("4G", 0));
}

/* Not registered yet: no technology, no bars lit */
TEST(searching_no_bars)
{
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("Searching", 3000));
	ui_test_wait(1000);
	CHECK(!ui_test_shows("4G"));
	CHECK_INT(modem_signal_bars(), -1);
	ui_test_modem_network(UI_TEST_NET_HOME, OPERATOR);
	CHECK(shade_shows("4G", 3000));
	CHECK(bars_become(4, 3000));
}

/* The network lost: the bars and technology go */
TEST(lost_no_bars)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
	ui_test_modem_network(UI_TEST_NET_NONE, NULL);
	CHECK(badge_becomes(LV_SYMBOL_CLOSE, 3000));
	CHECK(!ui_test_shows("4G"));
	CHECK_INT(modem_signal_bars(), -1);
}

/* The port by its USB device and interface (modem.conf's usb= and interface=), not
 * by its number: after a reset with the old one still open, interface 2 came back
 * as ttyUSB3 on the phone */
#define USB_CONF "provider=at\nusb=2c7c:0125\ninterface=2\nquirks=quectel\n"

TEST(port_by_usb_interface)
{
	ui_test_write(CONF, USB_CONF);
	ui_test_start();
	ui_test_modem_usb("ttyUSB2");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
}

TEST(port_renumbered_after_a_reset)
{
	ui_test_write(CONF, USB_CONF);
	ui_test_start();
	ui_test_modem_usb("ttyUSB2");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 3000));
	ui_test_modem_unplug();
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 3000));
	ui_test_modem_usb("ttyUSB3");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows("4G", 5000));
}

/* Another device's ports, or the EG25-G's other interfaces: not opened */
TEST(other_usb_device_not_the_modem)
{
	ui_test_write(CONF, "provider=at\nusb=2c7c:0125\ninterface=3\n");
	ui_test_start();
	ui_test_modem_usb("ttyUSB2"); /* interface 2 */
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(2000);
	CHECK(badge_becomes(LV_SYMBOL_WARNING, 0));
}

static bool about_open(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("About phone");
}

/* Within `ms`, on the open page: the row `title` shows `value` */
static bool page_shows(const char *title, const char *value, uint32_t ms)
{
	for (uint32_t t = 0;; t += 100) {
		const char *v = page_value(title);
		if (v && strcmp(v, value) == 0)
			return true;
		if (t >= ms) {
			printf("The row %s: \"%s\", not \"%s\"\n", title, v ? v : "(no row)", value);
			return false;
		}
		ui_test_wait(100);
	}
}

/* The modem's IMEI and the SIM's own number in About phone */
TEST(imei_and_number_in_about)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	CHECK(about_open());
	CHECK(page_shows("Phone number", UI_TEST_NUMBER, 0));
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 0));
}

/* Found while the page is open: filled in as the modem answers */
TEST(about_filled_in_as_found)
{
	ui_test_start();
	CHECK(about_open());
	CHECK(page_shows("IMEI", "Unknown", 0));
	CHECK(page_shows("Phone number", "Unknown", 0));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 5000));
	CHECK(page_shows("Phone number", UI_TEST_NUMBER, 5000));
}

/* Many SIMs do not hold their number: not known, the IMEI still */
TEST(number_not_on_the_sim)
{
	ui_test_modem_number(NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	CHECK(about_open());
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 0));
	CHECK(page_shows("Phone number", "Unknown", 0));
}

/* No SIM: no number, the modem's IMEI still */
TEST(no_sim_no_number)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(about_open());
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 3000));
	CHECK(page_shows("Phone number", "No SIM card", 0));
}

/* Another SIM: its number, not the last one's */
TEST(sim_changed_its_number)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(about_open());
	CHECK(page_shows("Phone number", UI_TEST_NUMBER, 5000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(page_shows("Phone number", "No SIM card", 5000));
	ui_test_modem_number("+40798765432");
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(page_shows("Phone number", "+40798765432", 8000));
}

/* A device without a modem: neither row */
TEST(no_imei_without_a_modem)
{
	remove(CONF);
	ui_test_start();
	CHECK(about_open());
	CHECK(ui_test_shows("Kernel"));
	CHECK(!ui_test_find("IMEI"));
	CHECK(!ui_test_find("Phone number"));
}

static bool network_open(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Network");
}

/* The card of the open page's row `title`, NULL if none */
static lv_obj_t *row_card(const char *title)
{
	lv_obj_t *label = row_title(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	return row ? lv_obj_get_parent(row) : NULL;
}

/* Settings › Network: the mobile network's name, the number and the IMEI together,
 * in a section of their own (not the Wi-Fi's) */
TEST(mobile_section_in_network)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	CHECK(network_open());
	CHECK(ui_test_shows("Mobile"));
	CHECK(page_shows("Network", OPERATOR, 0));
	CHECK(page_shows("Phone number", UI_TEST_NUMBER, 0));
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 0));
	lv_obj_t *card = row_card("Network");
	CHECK(card);
	CHECK(row_card("Phone number") == card);
	CHECK(row_card("IMEI") == card);
	CHECK(row_card("Wi-Fi") != card);
}

/* Filled in, and changed, while the page is open */
TEST(network_page_follows_the_modem)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(badge_becomes(UI_SYMBOL_SIM_CARD, 3000));
	CHECK(network_open());
	CHECK(page_shows("Network", "No SIM card", 0));
	CHECK(page_shows("Phone number", "No SIM card", 0));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(page_shows("Network", OPERATOR, 8000));
	CHECK(page_shows("Phone number", UI_TEST_NUMBER, 8000));
	CHECK(page_shows("IMEI", UI_TEST_IMEI, 0));
}

/* Without a modem: the network not available, no number or IMEI */
TEST(network_page_without_a_modem)
{
	remove(CONF);
	ui_test_start();
	CHECK(network_open());
	CHECK(page_shows("Network", "Not available", 0));
	CHECK(!ui_test_find("IMEI"));
	CHECK(!ui_test_find("Phone number"));
}

/* Roaming: an "R" after the signal bars, in the text color (information, not a
 * problem), as Android */

/* The status bar's "R", NULL if none */
static lv_obj_t *roaming_r(void)
{
	lv_obj_t *found[4];
	uint32_t n = ui_test_find_all("R", found, 4);
	for (uint32_t i = 0; i < LV_MIN(n, 4); i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H)
			return found[i];
	}
	return NULL;
}

/* The "R" shows (or not) within `ms`: the fake answers in real time */
static bool roaming_r_becomes(bool shown, uint32_t ms)
{
	for (uint32_t t = 0;; t += 48) {
		if ((roaming_r() != NULL) == shown)
			return true;
		if (t >= ms) {
			fprintf(stderr, "  roaming R %s\n", shown ? "not shown" : "still shown");
			return false;
		}
		ui_test_wait(48);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
}

TEST(roaming_r_in_the_status_bar)
{
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(roaming_r_becomes(true, 3000));
	lv_obj_t *r = roaming_r();
	if (!r)
		return;
	CHECK(lv_color_eq(lv_obj_get_style_text_color(r, 0), lv_color_hex(UI_COLOR_TEXT)));
	lv_area_t a;
	lv_obj_get_coords(r, &a);
	CHECK(a.x1 > 360);                       /* the right side, after the clock */
	CHECK_INT(a.x2, 720 - 20 - 40 - 16 - 1); /* the last before the battery */
	CHECK_STR(badge_text(), "");
}

TEST(no_roaming_r_at_home)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	CHECK(!roaming_r());
}

/* Back home, or the network lost: gone */
TEST(roaming_r_goes_home)
{
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(roaming_r_becomes(true, 3000));
	ui_test_modem_network(UI_TEST_NET_HOME, OPERATOR);
	CHECK(roaming_r_becomes(false, 3000));
}

TEST(roaming_r_goes_with_the_network)
{
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(roaming_r_becomes(true, 3000));
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	CHECK(roaming_r_becomes(false, 3000));
}

/* With data roaming on, connected: the "R", then the technology after it */
TEST(roaming_r_before_the_technology)
{
	ui_test_write("/var/lib/phone-ui/settings", "mobile_data=1\ndata_roaming=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Vodafone");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 8000));
	CHECK(roaming_r_becomes(true, 3000));
	ui_test_wait(1500); /* the status bar's next update */
	lv_obj_t *r = roaming_r(), *found[4], *tech = NULL;
	uint32_t n = ui_test_find_all("4G", found, 4);
	for (uint32_t i = 0; i < LV_MIN(n, 4); i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H)
			tech = found[i];
	}
	CHECK(r && tech);
	if (!r || !tech)
		return;
	lv_area_t ra, ta;
	lv_obj_get_coords(r, &ra);
	lv_obj_get_coords(tech, &ta);
	CHECK_INT(ta.x1 - ra.x2 - 1, 6); /* x2 inclusive */
}

/* IMS (ims= in modem.conf, Quectel's AT+QCFG="ims") */

static void conf_add(const char *line)
{
	FILE *f = fopen(CONF, "a");
	fprintf(f, "%s\n", line);
	fclose(f);
}

static bool home(void)
{
	return modem_network() == MODEM_NET_HOME;
}

/* Waits for the modem's IMS setting to be `value` after `restarts` restarts */
static bool ims_becomes(unsigned value, unsigned restarts, uint32_t ms)
{
	unsigned now_restarts;
	for (uint32_t t = 0; t <= ms; t += 100) {
		if (ui_test_modem_ims_now(&now_restarts) == value && now_restarts == restarts)
			return true;
		ui_test_wait(100);
	}
	fprintf(stderr, "  IMS %u after %u restarts, expected %u after %u\n", ui_test_modem_ims_now(&now_restarts),
	        now_restarts, value, restarts);
	return false;
}

/* ims=off: VoLTE calls Digi releases at once; off, calls fall back to 2G/3G. Written,
 * the modem restarted for it to take, once */
TEST(ims_off)
{
	conf_add("ims=off");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(2, 1, 8000));
	CHECK(ui_test_wait_until(home, 10000)); /* registered again after the restart */
	CHECK_INT(ui_test_modem_restart_commands(), 0); /* nothing sent while it restarted */
	ui_test_wait(3000);
	CHECK(ims_becomes(2, 1, 0)); /* not restarted again */
}

/* Already so: neither written nor restarted */
TEST(ims_already_off)
{
	conf_add("ims=off");
	ui_test_modem_ims(2, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	ui_test_wait(3000);
	CHECK(ims_becomes(2, 0, 0));
}

/* ims=on sets it back */
TEST(ims_on)
{
	conf_add("ims=on");
	ui_test_modem_ims(2, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(1, 1, 8000));
}

/* Unset: the modem's own is left */
TEST(ims_unset)
{
	ui_test_modem_ims(0, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	ui_test_wait(3000);
	CHECK(ims_becomes(0, 0, 0));
}

/* With sms_domain= to change too (the PinePhone's modem.conf): both written, one restart */
TEST(ims_with_sms_domain)
{
	conf_add("ims=off");
	conf_add("sms_domain=cs");
	ui_test_modem_sms_domain(1, false, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(2, 1, 8000));
	CHECK_INT(ui_test_modem_sms_domain_now(NULL), 0);
	CHECK(ui_test_wait_until(home, 10000));
	ui_test_wait(3000);
	CHECK(ims_becomes(2, 1, 0));
}

/* A write that does not take: one restart, not one after another */
TEST(ims_not_taking)
{
	conf_add("ims=off");
	ui_test_modem_ims(1, true);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(1, 1, 8000));
	CHECK(ui_test_wait_until(home, 10000)); /* working all the same */
	ui_test_wait(3000);
	CHECK(ims_becomes(1, 1, 0));
}

/* Recording (just modem-record): while /var/lib/phone-ui/modem.trace exists, what phone-ui
 * sends the modem and every line it says are appended to it, timed */

#define TRACE "/var/lib/phone-ui/modem.trace"

static char trace_text[65536];

/* The recording's text so far ("" without one) */
static const char *trace(const char *path)
{
	trace_text[0] = '\0';
	FILE *f = fopen(path, "r");
	if (f) {
		size_t n = fread(trace_text, 1, sizeof(trace_text) - 1, f);
		trace_text[n] = '\0';
		fclose(f);
	}
	return trace_text;
}

static bool trace_has_sim(void)
{
	return strstr(trace(TRACE), "< +CPIN: READY") != NULL;
}

static bool trace_has_cnmi(void)
{
	return strstr(trace(TRACE), "> AT+CNMI=") != NULL;
}

static bool trace_has_sms(void)
{
	return strstr(trace(TRACE), "< +CMTI: ") != NULL;
}

static void trace_create(void)
{
	mkdir("/var/lib/phone-ui", 0755);
	FILE *f = fopen(TRACE, "w");
	fclose(f);
}

TEST(trace_records)
{
	trace_create();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(trace_has_sim, 8000));
	const char *t = trace(TRACE);
	CHECK(strstr(t, "> AT+CPIN?\n") != NULL);
	/* Each line timed: "hh:mm:ss.mmm > " */
	CHECK(t[2] == ':' && t[5] == ':' && t[8] == '.' && t[12] == ' ');
	CHECK(ui_test_wait_until(trace_has_cnmi, 10000)); /* SMS reported from then */
	ui_test_modem_sms("+40700000001", "Recorded");
	CHECK(ui_test_wait_until(trace_has_sms, 8000));
	CHECK(strstr(trace(TRACE), "> AT+CMGL=4\n") != NULL);
}

/* Moved away: no more lines in it; there again, a new recording */
TEST(trace_stops_when_moved)
{
	trace_create();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(trace_has_sim, 8000));
	CHECK(ui_test_wait_until(trace_has_cnmi, 10000));
	CHECK(ui_test_wait_until(home, 10000));
	CHECK(rename(TRACE, "/var/lib/phone-ui/done.trace") == 0);
	ui_test_wait(1500);
	size_t before = strlen(trace("/var/lib/phone-ui/done.trace"));
	ui_test_modem_sms("+40700000001", "Not recorded");
	ui_test_wait(3000);
	CHECK_INT(strlen(trace("/var/lib/phone-ui/done.trace")), before);
	trace_create();
	ui_test_wait(1500); /* found within a second */
	ui_test_modem_sms("+40700000001", "Recorded again");
	CHECK(ui_test_wait_until(trace_has_sms, 8000));
}

/* Without one, none is made */
TEST(trace_not_made)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	CHECK(access(TRACE, F_OK) != 0);
}

/* qcfg= lines in modem.conf: Quectel's AT+QCFG settings the modem keeps (the ring line the
 * phone wakes by), as the PinePhone's modem.conf has them. Read, written when they differ,
 * without a restart */

#define RING "\"pulse\",120,1000,5000,\"off\",1"

static bool qcfg_is(const char *name, const char *value)
{
	return strcmp(ui_test_modem_qcfg_now(name, NULL), value) == 0;
}

static bool risignal_physical(void)
{
	return qcfg_is("risignaltype", "\"physical\"");
}

static bool ring_pulses(void)
{
	return qcfg_is("urc/ri/ring", RING);
}

TEST(qcfg_written)
{
	conf_add("qcfg=\"risignaltype\",\"physical\"");
	conf_add("qcfg=\"urc/ri/ring\"," RING);
	ui_test_modem_qcfg("risignaltype", "\"respective\"");
	ui_test_modem_qcfg("urc/ri/ring", "\"auto\",120,1000,5000,\"off\",1");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(risignal_physical, 8000));
	CHECK(ui_test_wait_until(ring_pulses, 8000));
	CHECK(ui_test_wait_until(home, 10000));
	unsigned restarts = 0;
	ui_test_modem_ims_now(&restarts);
	CHECK_INT(restarts, 0);
	unsigned writes = 0;
	ui_test_modem_qcfg_now("risignaltype", &writes);
	CHECK_INT(writes, 2);
}

/* Already so: not written */
TEST(qcfg_already)
{
	conf_add("qcfg=\"risignaltype\",\"physical\"");
	conf_add("qcfg=\"urc/ri/ring\"," RING);
	ui_test_modem_qcfg("risignaltype", "\"physical\"");
	ui_test_modem_qcfg("urc/ri/ring", RING);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	ui_test_wait(3000);
	unsigned writes = 1;
	ui_test_modem_qcfg_now("risignaltype", &writes);
	CHECK_INT(writes, 0);
}

/* One the modem does not know (ERROR): left, the others written, the modem working */
TEST(qcfg_unknown)
{
	conf_add("qcfg=\"nosuchthing\",1");
	conf_add("qcfg=\"risignaltype\",\"physical\"");
	ui_test_modem_qcfg("risignaltype", "\"respective\"");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(risignal_physical, 8000));
	CHECK(ui_test_wait_until(home, 10000));
}

/* Calls over 4G (Settings › Network's switch, volte in the settings): the modem's IMS
 * turned on or off by the user, modem.conf's ims= what it is until then */

#define SETTINGS "/var/lib/phone-ui/settings"
#define VOLTE "Calls over 4G (VoLTE)"

/* Settings › Network's switch of the row `title` (opened), NULL if none */
static lv_obj_t *network_switch(const char *title)
{
	if (!ui_test_open("Settings") || !ui_test_tap_text("Network"))
		return NULL;
	lv_obj_t *label = ui_test_find(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++)
		if (lv_obj_check_type(lv_obj_get_child(row, (int32_t)i), &lv_switch_class))
			return lv_obj_get_child(row, (int32_t)i);
	return NULL;
}

static bool settings_file_has(const char *text)
{
	char data[4096] = "";
	FILE *f = fopen(SETTINGS, "r");
	if (f) {
		data[fread(data, 1, sizeof(data) - 1, f)] = '\0';
		fclose(f);
	}
	return strstr(data, text) != NULL;
}

/* The board's ims=off: the switch off; turned on, IMS written and the modem restarted
 * again, remembered */
TEST(volte_switch_turns_ims_on)
{
	conf_add("ims=off");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(2, 1, 8000));
	CHECK(ui_test_wait_until(home, 10000));
	lv_obj_t *sw = network_switch(VOLTE);
	CHECK(sw);
	if (!sw)
		return;
	CHECK(!lv_obj_has_state(sw, LV_STATE_CHECKED));
	ui_test_tap(sw);
	CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED));
	CHECK(ims_becomes(1, 2, 8000));
	CHECK(ui_test_wait_until(home, 10000));
	CHECK(settings_file_has("volte=1"));
	ui_test_wait(3000);
	CHECK(ims_becomes(1, 2, 0)); /* not restarted again */
}

/* And off again */
TEST(volte_switch_turns_ims_off)
{
	conf_add("ims=on");
	ui_test_modem_ims(1, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	lv_obj_t *sw = network_switch(VOLTE);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED)); /* the board's ims=on */
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(ims_becomes(2, 1, 8000));
	CHECK(settings_file_has("volte=0"));
}

/* Turned on in an earlier run: the user's choice, not modem.conf's ims=off */
TEST(volte_remembered_at_startup)
{
	conf_add("ims=off");
	ui_test_write(SETTINGS, "volte=1\n");
	ui_test_modem_ims(2, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(1, 1, 8000));
	CHECK(ui_test_wait_until(home, 10000));
	lv_obj_t *sw = network_switch(VOLTE);
	CHECK(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* A board that does not say (no ims=): the modem's own left, no switch */
TEST(volte_no_switch_without_ims)
{
	ui_test_write(SETTINGS, "volte=1\n");
	ui_test_modem_ims(2, false);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(home, 10000));
	CHECK(!network_switch(VOLTE));
	CHECK(ui_test_shows("Mobile data")); /* the page is there */
	CHECK(ims_becomes(2, 0, 0));
}

static bool in_call(void)
{
	return modem_call_state() != MODEM_CALL_NONE;
}

static bool no_call(void)
{
	return !in_call();
}

/* Changed during a call: the modem is not restarted under it, only once it ended */
TEST(volte_waits_for_the_call)
{
	conf_add("ims=off");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(2, 1, 8000));
	CHECK(ui_test_wait_until(home, 10000));
	ui_test_modem_incoming("0712345678");
	CHECK(ui_test_wait_until(in_call, 5000));
	modem_volte_set_enabled(true);
	ui_test_wait(3000);
	CHECK(in_call());
	CHECK(ims_becomes(2, 1, 0));
	ui_test_modem_remote_hang_up();
	CHECK(ui_test_wait_until(no_call, 5000));
	CHECK(ims_becomes(1, 2, 8000));
}

/* modem.conf as a person writes it: spaces around "=" and at the ends, a list of quirks,
 * a value a setting does not take (left alone) */
TEST(config_spaced_and_listed)
{
	ui_test_write(CONF, "provider = at\nport=/tmp/modem  \nquirks=usb_reset, quectel\n"
	                    "ims\t= off \nsms_domain=ps\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ims_becomes(2, 1, 8000)); /* a Quectel quirk: written */
	CHECK(ui_test_wait_until(home, 10000));
	unsigned restarts;
	CHECK_INT(ui_test_modem_sms_domain_now(&restarts), 1); /* "ps": not one of its values */
	CHECK_INT(restarts, 1);
}

/* The AT provider without a port, nor a USB device to find it by: as a device without a
 * modem, none probed */
TEST(config_without_a_port)
{
	ui_test_write(CONF, "provider=at\nquirks=quectel\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(2000);
	CHECK(badge_becomes("", 0));
	CHECK(!ui_test_shows(OPERATOR));
	const char *value = row_value("Network");
	CHECK(value);
	if (value)
		CHECK_STR(value, "Not available");
}
