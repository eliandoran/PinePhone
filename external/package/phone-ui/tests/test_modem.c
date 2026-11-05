/* The modem (modem.c) through the UI (ui_test.h): whether there is one and the SIM's
 * state, in the status bar and Settings › Network, on the fake modem of fake_modem.c */

#include "test.h"
#include "ui_test.h"

TEST_SETUP
{
	ui_test_start();
}

/* Settings › Network's "Mobile network" row shows `text` */
static bool settings_shows(const char *text)
{
	return ui_test_open("Settings") && ui_test_tap_text("Network") && ui_test_shows(text);
}

TEST(no_modem)
{
	CHECK(ui_test_shows("No modem"));
	CHECK(!ui_test_shows("No SIM"));
	CHECK(settings_shows("No modem"));
}

TEST(no_sim)
{
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
	CHECK(settings_shows("No SIM card"));
}

TEST(sim_ready)
{
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 3000));
	CHECK(!ui_test_shows("No modem"));
	CHECK(settings_shows("SIM ready"));
}

TEST(sim_pin)
{
	ui_test_modem(UI_TEST_SIM_PIN);
	CHECK(ui_test_wait_for("SIM locked", 3000));
	CHECK(settings_shows("SIM locked (PIN)"));
}

TEST(sim_puk)
{
	ui_test_modem(UI_TEST_SIM_PUK);
	CHECK(ui_test_wait_for("SIM blocked", 3000));
	CHECK(settings_shows("SIM blocked (PUK)"));
}

/* The modem says when the card goes and comes back (+QSIMSTAT) */
TEST(sim_removed_and_inserted)
{
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 3000));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 3000));
}

/* Busy just after power-up (+CME ERROR: 14): asked again */
TEST(sim_busy_at_first)
{
	ui_test_modem_sim_busy(2);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 10000));
}

/* The port is there but the modem does not answer yet (still starting) */
TEST(not_answering)
{
	ui_test_modem_silent(true);
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("Modem...", 3000));
	ui_test_wait(6000);
	CHECK(ui_test_shows("Modem..."));
	CHECK(settings_shows("Starting"));
	ui_test_modem_silent(false);
	CHECK(ui_test_wait_for("SIM ready", 15000));
}

/* The modem goes from USB (powered off, restarted) and comes back */
TEST(unplugged_and_back)
{
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 3000));
	ui_test_modem_unplug();
	CHECK(ui_test_wait_for("No modem", 3000));
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_for("No SIM", 5000));
}

/* It restarted on its own ("RDY", echo on again): set up and asked again */
TEST(restarted)
{
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("SIM ready", 3000));
	ui_test_modem_restart(UI_TEST_SIM_PIN);
	CHECK(ui_test_wait_for("SIM locked", 3000));
}

/* The status bar's other symbols move with the text: the USB one right of it */
TEST(usb_symbol_after_the_text)
{
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
