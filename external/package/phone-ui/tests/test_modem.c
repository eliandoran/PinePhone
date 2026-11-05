/* The modem (modem.c) through the UI (ui_test.h): whether there is one and the SIM's
 * state, in the status bar and Settings › Network, on the fake modem of fake_modem.c */

#include <stdio.h>
#include <unistd.h>

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

/* Settings › Network's "Mobile network" row shows `text` */
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

TEST(no_modem)
{
	ui_test_start();
	CHECK(ui_test_shows("No modem"));
	CHECK(!ui_test_shows("No SIM"));
	CHECK(settings_shows("No modem"));
}

TEST(no_sim)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
	CHECK(settings_shows("No SIM card"));
}

TEST(sim_ready_registered)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	/* Not in the status bar: nothing there while all is well */
	CHECK(!ui_test_shows(OPERATOR));
	CHECK(!ui_test_shows("SIM ready"));
	CHECK(!ui_test_shows("No modem"));
	CHECK(settings_shows(OPERATOR));
}

/* A problem stays in the status bar, and the shade says it in full */
TEST(no_sim_in_the_shade)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
	CHECK(shade_shows("No SIM card", 3000));
}

/* The shade's row has an antenna, drawn (no symbol for it), not the phone's symbol */
TEST(antenna_in_the_shade)
{
	ui_test_start();
	ui_test_tap_at(360, UI_STATUS_H / 2);
	CHECK(ui_test_wait_for("No modem", 2000));
	lv_obj_t *found[4];
	uint32_t n = ui_test_find_all("No modem", found, 4);
	lv_obj_t *icon = NULL;
	for (uint32_t i = 0; i < n; i++) /* the shade's, not the status bar's */
		if (lv_obj_get_index(found[i]) == 1)
			icon = lv_obj_get_child(lv_obj_get_parent(found[i]), 0);
	CHECK(icon);
	if (!icon)
		return;
	CHECK(!lv_obj_check_type(icon, &lv_label_class));
	CHECK(lv_obj_get_width(icon) > 0 && lv_obj_get_height(icon) > 0);
	CHECK(!ui_test_shows(LV_SYMBOL_CALL));
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
	CHECK(ui_test_wait_for("SIM locked", 3000));
	CHECK(settings_shows("SIM locked (PIN)"));
}

TEST(sim_puk)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_PUK);
	CHECK(ui_test_wait_for("SIM blocked", 3000));
	CHECK(settings_shows("SIM blocked (PUK)"));
}

/* The modem says when the card goes and comes back (+QSIMSTAT) */
TEST(sim_removed_and_inserted)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
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

/* The port is there but the modem does not answer yet (still starting) */
TEST(not_answering)
{
	ui_test_start();
	ui_test_modem_silent(true);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("Modem...", 3000));
	ui_test_wait(6000);
	CHECK(ui_test_shows("Modem..."));
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
	CHECK(ui_test_wait_for("No modem", 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 5000));
}

/* It restarted on its own ("RDY", echo on again): set up and asked again */
TEST(restarted)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(shade_shows(OPERATOR, 3000));
	ui_test_modem_restart(UI_TEST_SIM_PIN);
	CHECK(ui_test_wait_for("SIM locked", 3000));
}

/* The status bar's other symbols move with the text: the USB one right of it */
TEST(usb_symbol_after_the_text)
{
	ui_test_start();
	ui_test_write("/sys/class/net/usb0/carrier", "1\n");
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
	ui_test_wait(1100);
	lv_obj_t *text = ui_test_find("No SIM"), *usb = ui_test_find(LV_SYMBOL_USB);
	CHECK(text && usb);
	if (!text || !usb)
		return;
	lv_area_t a, b;
	lv_obj_get_coords(text, &a);
	lv_obj_get_coords(usb, &b);
	CHECK_INT(b.x1 - a.x2 - 1, 16); /* x2 inclusive */
}


/* The value of Settings › Network's row `title`, NULL if none */
static const char *row_value(const char *title)
{
	if (!ui_test_open("Settings") || !ui_test_tap_text("Network"))
		return NULL;
	lv_obj_t *label = ui_test_find(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++) {
		lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
		if (child != label && lv_obj_check_type(child, &lv_label_class))
			return lv_label_get_text(child);
	}
	return NULL;
}

/* A device without a modem (no modem.conf): nothing in the status bar, none probed */
TEST(no_modem_on_this_device)
{
	remove(CONF);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY); /* a port there, not asked */
	ui_test_wait(2000);
	CHECK(!ui_test_shows("No modem"));
	CHECK(!ui_test_shows(OPERATOR));
	CHECK(!ui_test_shows("Modem..."));
	const char *value = row_value("Mobile network");
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
	const char *value = row_value("Mobile network");
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
	CHECK(ui_test_shows("No modem"));
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
	CHECK(!ui_test_shows("No SIM")); /* the network lost (+CREG: 0), the card not known gone yet */
	CHECK(ui_test_wait_for("No SIM", 15000));
}

/* Network registration */

TEST(searching_then_registered)
{
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("Searching...", 3000));
	CHECK(settings_shows("Searching"));
	ui_test_modem_network(UI_TEST_NET_HOME, "Orange F"); /* +CREG: 1, then asked +COPS? */
	CHECK(shade_shows("Orange F", 3000));
}

TEST(not_registered)
{
	ui_test_modem_network(UI_TEST_NET_NONE, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("No service", 3000));
	const char *value = row_value("Mobile network"); /* not the status bar's */
	CHECK(value);
	if (value)
		CHECK_STR(value, "No service");
}

TEST(registration_denied)
{
	ui_test_modem_network(UI_TEST_NET_DENIED, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("No service", 3000));
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
	CHECK(ui_test_wait_for("Searching...", 3000));
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
	CHECK(ui_test_wait_for("No SIM", 3000));
	CHECK(!ui_test_shows(OPERATOR));
}
