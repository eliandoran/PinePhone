/* Mobile data (modem.c, modem_at.c) through the UI (ui_test.h): the shade's toggle, the
 * modem's data session (AT$QCRMCALL on fake_modem.c) and the board's script that sets up
 * the network interface (ui_test_reset()'s logs what it is asked) */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "modem/modem.h"
#include "test.h"
#include "ui_symbols.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define CONF "/tmp/modem.conf" /* MODEM_CONF of modem.o */
#define OPERATOR "Fake Mobile"
#define TILE "Mobile data"

TEST_SETUP
{
	ui_test_reset();
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

static void shade_open(void)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
}

/* The shade's mobile data tile (open), NULL if it has none */
static lv_obj_t *tile(void)
{
	lv_obj_t *label = ui_test_find(TILE);
	return label ? lv_obj_get_parent(label) : NULL;
}

static bool tile_on(void)
{
	lv_obj_t *t = tile();
	return t && lv_obj_has_state(t, LV_STATE_CHECKED);
}

/* Registered on the home network, the shade open */
static void registered(void)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	shade_open();
	CHECK(ui_test_wait_for(OPERATOR, 3000));
}

/* The tile's icon is Font Awesome's tower-broadcast, as the network's row: a glyph of the
 * tile's font (symbols.txt), so it takes the tile's text color */
TEST(tile_icon)
{
	registered();
	lv_obj_t *t = tile();
	CHECK(t != NULL);
	if (!t) return;
	lv_obj_t *icon = lv_obj_get_child(t, 0);
	CHECK(lv_obj_check_type(icon, &lv_label_class));
	if (!lv_obj_check_type(icon, &lv_label_class)) return;
	CHECK_STR(lv_label_get_text(icon), UI_SYMBOL_TOWER_BROADCAST);
}

/* Off until turned on: nothing started */
TEST(off_by_default)
{
	registered();
	CHECK(tile());
	CHECK(!tile_on());
	ui_test_wait(2000);
	CHECK(!ui_test_modem_data(NULL));
	CHECK_STR(file("/tmp/mobile-data.log"), "down wwan0\n"); /* the startup's cleanup only */
}

/* On: the modem's session, then the interface up; remembered */
TEST(turned_on_connects)
{
	registered();
	CHECK(ui_test_tap_text(TILE));
	CHECK(tile_on());
	CHECK(ui_test_mobile_data_ran("up wwan0", 3000));
	CHECK(ui_test_modem_data(NULL));
	CHECK(strstr(file(SETTINGS), "mobile_data=1"));
}

/* Off again: the interface down, the session ended */
TEST(turned_off_disconnects)
{
	registered();
	CHECK(ui_test_tap_text(TILE));
	CHECK(ui_test_mobile_data_ran("up wwan0", 3000));
	CHECK(ui_test_tap_text(TILE));
	CHECK(!tile_on());
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	ui_test_wait(500);
	CHECK(!ui_test_modem_data(NULL));
	CHECK(strstr(file(SETTINGS), "mobile_data=0"));
}

/* On from the last time: connected once registered, without a tap */
TEST(remembered_at_startup)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(ui_test_modem_data(NULL));
}

/* On while searching: it waits for the network, then connects */
TEST(waits_for_the_network)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(3000);
	CHECK(!ui_test_modem_data(NULL));
	ui_test_modem_network(UI_TEST_NET_HOME, OPERATOR);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
}

/* Not while roaming: data abroad costs */
TEST(not_while_roaming)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 2000)); /* the startup's cleanup */
	ui_test_wait(4000);
	CHECK(!ui_test_modem_data(NULL));
	CHECK_STR(file("/tmp/mobile-data.log"), ""); /* and nothing after it */
}

/* The start refused (as after the modem's USB reset): the board resets the interface,
 * then it tries again */
TEST(refused_then_reset)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_data_fails(1);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("reset wwan0", 5000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 15000));
	CHECK(ui_test_modem_data(NULL));
}

/* The session ends on the modem's side: found by asking, then again */
TEST(dropped_reconnects)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	ui_test_modem_data_drop();
	CHECK(ui_test_mobile_data_ran("down wwan0", 40000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 10000));
	unsigned starts;
	CHECK(ui_test_modem_data(&starts));
	CHECK_INT(starts, 2);
}

/* The network lost: down; found again: up */
TEST(network_lost_and_back)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	CHECK(ui_test_mobile_data_ran("down wwan0", 5000));
	ui_test_modem_network(UI_TEST_NET_HOME, OPERATOR);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
}

/* The modem gone from USB (a reset): down; back, up again */
TEST(modem_gone_and_back)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	ui_test_modem_unplug();
	CHECK(ui_test_mobile_data_ran("down wwan0", 5000));
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 8000));
	CHECK(ui_test_modem_data(NULL));
}

/* The status bar shows `text` (the shade's row has the technology too: not that one) */
static lv_obj_t *in_status_bar(const char *text)
{
	lv_obj_t *found[4];
	uint32_t n = ui_test_find_all(text, found, 4);
	for (uint32_t i = 0; i < LV_MIN(n, 4); i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H)
			return found[i];
	}
	return NULL;
}

/* The status bar shows the technology within `ms` */
static bool status_bar_tech(uint32_t ms)
{
	for (uint32_t t = 0; t <= ms; t += 100) {
		if (in_status_bar("4G"))
			return true;
		ui_test_wait(100);
		nanosleep(&(struct timespec){ 0, 10000000 }, NULL); /* the fake modem answers in real time */
	}
	return false;
}

#define ARROWS "\xEF\x83\x9C" /* Font Awesome's sort, the arrows once beside the technology */

/* Data off: no technology (the shade has it) */
TEST(status_bar_data_off)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	shade_open();
	CHECK(ui_test_wait_for("4G", 3000));
	ui_test_wait(2000);
	CHECK(!in_status_bar("4G"));
}

/* Connected: the technology, alone: no arrows, "4G" says data is up */
TEST(status_bar_data_connected)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(status_bar_tech(2000));
	lv_obj_t *t = in_status_bar("4G");
	CHECK(t && lv_color_eq(lv_obj_get_style_text_color(t, 0), lv_color_hex(UI_COLOR_TEXT)));
	CHECK(!ui_test_shows(ARROWS));
}

/* On but refused: nothing until the session is up */
TEST(status_bar_data_refused)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_data_fails(1);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("reset wwan0", 5000));
	ui_test_wait(1500);
	CHECK(!in_status_bar("4G"));
	CHECK(ui_test_mobile_data_ran("up wwan0", 15000));
	CHECK(status_bar_tech(2000));
}

/* Turned off, or the network lost: the technology goes */
TEST(status_bar_data_ends)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(status_bar_tech(5000));
	shade_open();
	CHECK(ui_test_tap_text(TILE));
	ui_test_wait(1500);
	CHECK(!in_status_bar("4G"));
	CHECK(ui_test_tap_text(TILE));
	CHECK(status_bar_tech(5000));
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_wait(2000);
	CHECK(!in_status_bar("4G"));
}

/* A board without data (no data= in modem.conf): no tile */
TEST(no_tile_without_data)
{
	ui_test_write(CONF, "provider=at\nport=/tmp/modem\nquirks=quectel\n");
	registered();
	CHECK(!tile());
	CHECK(ui_test_shows("Wi-Fi"));
}

/* Without a modem at all: no tile */
TEST(no_tile_without_a_modem)
{
	remove(CONF);
	ui_test_start();
	shade_open();
	CHECK(ui_test_wait_for("Wi-Fi", 2000));
	CHECK(!tile());
}

/* What an earlier phone-ui left (it does not end the session as it exits): the
 * interface taken down and the modem's session ended at startup, data being off */
TEST(leftovers_cleaned_at_startup)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	unsigned starts;
	CHECK(!ui_test_modem_data(&starts));
	CHECK_INT(starts, 0);
}

/* A session left on: ended at startup, started again when data is on */
TEST(left_on_started_again)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
}

static bool ready(void)
{
	return modem_network() == MODEM_NET_HOME;
}

static bool data_connected(void)
{
	return modem_data_state() == MODEM_DATA_CONNECTED;
}

/* What an earlier phone-ui left: the modem's session, and the interface it brought up
 * (IFF_UP in its flags, as the board's script left it) */
static void left_on(void)
{
	ui_test_modem_data_left();
	ui_test_write("/sys/class/net/wwan0/flags", "0x1003\n");
}

/* A session the modem kept from an earlier phone-ui (`just ui`, a crash), data on:
 * taken over, the interface as it is (its udhcpc still renews the lease), neither ended
 * nor started again, nor once the network is known or the session checked (30 s) */
TEST(left_on_kept)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	left_on();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(data_connected, 3000));
	CHECK(ui_test_wait_until(ready, 3000));
	ui_test_wait(31000);
	CHECK(data_connected());
	unsigned starts;
	CHECK(ui_test_modem_data(&starts));
	CHECK_INT(starts, 0);
	CHECK_STR(file("/tmp/mobile-data.log"), "");
	shade_open();
	CHECK(tile_on());
}

/* Kept, then ended on the modem's side: down, and a new one up */
TEST(left_on_kept_then_dropped)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	left_on();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(data_connected, 3000));
	ui_test_modem_data_drop();
	CHECK(ui_test_mobile_data_ran("down wwan0", 35000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	unsigned starts;
	CHECK(ui_test_modem_data(&starts));
	CHECK_INT(starts, 1);
}

/* Left on with data now off: ended, the interface down */
TEST(left_on_ended_when_off)
{
	left_on();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	ui_test_wait(500);
	CHECK(!ui_test_modem_data(NULL));
	CHECK(modem_data_state() == MODEM_DATA_OFF);
}

/* Left on while roaming without data roaming: ended once the network is known */
TEST(left_on_ended_while_roaming)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Other Mobile");
	left_on();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	ui_test_wait(500);
	CHECK(!ui_test_modem_data(NULL));
	CHECK(modem_data_state() == MODEM_DATA_WAITING);
}

/* Left on, but the network lost meanwhile: ended, started again once back */
TEST(left_on_network_lost)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	left_on();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	ui_test_wait(500);
	CHECK(!ui_test_modem_data(NULL));
	ui_test_modem_network(UI_TEST_NET_HOME, OPERATOR);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(ui_test_modem_data(NULL));
}

/* The session left on, but its interface down since (its driver bound again, the board's
 * reset): not taken over, ended and started again, the interface up */
TEST(left_on_without_its_interface)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_data_left();
	ui_test_write("/sys/class/net/wwan0/flags", "0x1002\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	unsigned starts;
	CHECK(ui_test_modem_data(&starts));
	CHECK_INT(starts, 1);
}

/* The switch of the open page's row `title`, NULL if none */
static lv_obj_t *row_switch(const char *title)
{
	lv_obj_t *label = ui_test_find(title);
	lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++)
		if (lv_obj_check_type(lv_obj_get_child(row, (int32_t)i), &lv_switch_class))
			return lv_obj_get_child(row, (int32_t)i);
	return NULL;
}

static bool network_page(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Network");
}

/* Settings › Network's Mobile data switch (open), NULL if none */
static lv_obj_t *settings_switch(void)
{
	return network_page() ? row_switch("Mobile data") : NULL;
}

static bool switch_on(lv_obj_t *sw)
{
	return sw && lv_obj_has_state(sw, LV_STATE_CHECKED);
}

/* Registered, the shade closed */
static void registered_shade_closed(void)
{
	registered();
	shade_open(); /* a tap on the bar closes it */
	ui_test_wait(500);
}

/* The switch does what the shade's tile does: on, connected and remembered */
TEST(settings_switch_connects)
{
	registered_shade_closed();
	lv_obj_t *sw = settings_switch();
	CHECK(sw);
	if (!sw)
		return;
	CHECK(!switch_on(sw));
	ui_test_tap(sw);
	CHECK(switch_on(sw));
	CHECK(ui_test_mobile_data_ran("up wwan0", 3000));
	CHECK(ui_test_modem_data(NULL));
	CHECK(strstr(file(SETTINGS), "mobile_data=1"));
}

/* Off: disconnected, and the shade's tile off too */
TEST(settings_switch_disconnects)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	registered_shade_closed();
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	lv_obj_t *sw = settings_switch();
	CHECK(switch_on(sw));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(!switch_on(sw));
	CHECK(ui_test_mobile_data_ran("down wwan0", 3000));
	CHECK(strstr(file(SETTINGS), "mobile_data=0"));
	shade_open();
	CHECK(ui_test_wait_for(TILE, 2000));
	CHECK(!tile_on());
}

/* Turned on from the shade while the page is open: the switch follows */
TEST(settings_switch_follows_the_shade)
{
	registered_shade_closed();
	lv_obj_t *sw = settings_switch();
	CHECK(!switch_on(sw));
	shade_open();
	CHECK(ui_test_wait_for(TILE, 2000));
	CHECK(ui_test_tap_text(TILE));
	shade_open();
	ui_test_wait(3000);
	CHECK(switch_on(sw));
}

/* A board without data, or no modem: no switch */
TEST(no_settings_switch_without_data)
{
	ui_test_write(CONF, "provider=at\nport=/tmp/modem\nquirks=quectel\n");
	registered_shade_closed();
	CHECK(!settings_switch());
	CHECK(ui_test_shows("IMEI"));
}

TEST(no_settings_switch_without_a_modem)
{
	remove(CONF);
	ui_test_start();
	CHECK(!settings_switch());
}

/* Data roaming (data_roaming in the settings, off unless turned on) */

/* On: connected while roaming too */
TEST(roaming_with_data_roaming_on)
{
	ui_test_write(SETTINGS, "mobile_data=1\ndata_roaming=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(ui_test_modem_data(NULL));
}

/* Settings › Network's switch, under Mobile data: off, then turned on while roaming */
TEST(data_roaming_switch_connects)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(3000);
	CHECK(!ui_test_modem_data(NULL));
	CHECK(network_page());
	lv_obj_t *sw = row_switch("Data roaming");
	CHECK(sw);
	if (!sw)
		return;
	CHECK(!switch_on(sw));
	ui_test_tap(sw);
	CHECK(switch_on(sw));
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(strstr(file(SETTINGS), "data_roaming=1"));
}

/* Off again while roaming: disconnected, mobile data itself still on */
TEST(data_roaming_switched_off)
{
	ui_test_write(SETTINGS, "mobile_data=1\ndata_roaming=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(network_page());
	lv_obj_t *sw = row_switch("Data roaming");
	CHECK(switch_on(sw));
	if (!sw)
		return;
	ui_test_tap(sw);
	CHECK(ui_test_mobile_data_ran("down wwan0", 5000));
	CHECK(strstr(file(SETTINGS), "data_roaming=0"));
	CHECK(switch_on(row_switch("Mobile data")));
}

/* Data roaming on, mobile data off: nothing */
TEST(data_roaming_alone_does_nothing)
{
	ui_test_write(SETTINGS, "data_roaming=1\n");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_wait(4000);
	CHECK(!ui_test_modem_data(NULL));
}

/* A board without mobile data: no roaming switch either */
TEST(no_data_roaming_switch_without_data)
{
	ui_test_write(CONF, "provider=at\nport=/tmp/modem\nquirks=quectel\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(network_page());
	CHECK(ui_test_wait_for("IMEI", 2000));
	CHECK(!ui_test_find("Data roaming"));
}

/* The APN: a text field on Settings › Network, empty for the network's own
 * ("Automatic"), set on the modem's data context 1 and attached again with it */

/* Settings › Network's APN field (open), NULL if none */
static lv_obj_t *apn_field(void)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_widgets(&lv_textarea_class, found, 8);
	for (unsigned i = 0; i < LV_MIN(n, 8); i++)
		if (strcmp(lv_textarea_get_placeholder_text(found[i]), "Automatic") == 0)
			return found[i];
	return NULL;
}

/* Types `apn` in place of what the field has, then Enter */
static bool apn_enter(const char *apn)
{
	lv_obj_t *field = apn_field();
	if (!field || !ui_test_tap_field("Automatic"))
		return false;
	for (size_t n = strlen(lv_textarea_get_text(field)); n; n--)
		if (!ui_test_type("\b"))
			return false;
	return ui_test_type(apn) && ui_test_type("\n");
}

/* Waits until the network attached with `apn` */
static bool attached_with(const char *apn, uint32_t ms)
{
	for (uint32_t t = 0;; t += 100) {
		if (strcmp(ui_test_modem_apn(NULL), apn) == 0)
			return true;
		if (t >= ms) {
			printf("attached with \"%s\", not \"%s\"\n", ui_test_modem_apn(NULL), apn);
			return false;
		}
		ui_test_wait(100);
	}
}

/* Empty by default, "Automatic" in it: the network's own, nothing changed */
TEST(apn_automatic_by_default)
{
	registered_shade_closed();
	CHECK(network_page());
	lv_obj_t *field = apn_field();
	CHECK(field);
	if (!field)
		return;
	CHECK_STR(lv_textarea_get_text(field), "");
	ui_test_wait(2000);
	unsigned reattached;
	CHECK_STR(ui_test_modem_apn(&reattached), "internet");
	CHECK_INT(reattached, 0);
}

/* Set while connected: the session ended, the modem attached again with it, the
 * session started again; remembered */
TEST(apn_set_while_connected)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	registered_shade_closed();
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(network_page());
	CHECK(apn_enter("net.example"));
	CHECK(ui_test_mobile_data_ran("down wwan0", 5000));
	CHECK(attached_with("net.example", 5000));
	CHECK(ui_test_mobile_data_ran("up wwan0", 10000));
	unsigned starts;
	CHECK(ui_test_modem_data(&starts));
	CHECK_INT(starts, 2);
	CHECK(strstr(file(SETTINGS), "apn=net.example"));
}

/* Set with data off: the modem has it for later, no session */
TEST(apn_set_with_data_off)
{
	registered_shade_closed();
	CHECK(network_page());
	CHECK(apn_enter("net.example"));
	CHECK(attached_with("net.example", 5000));
	ui_test_wait(2000);
	CHECK(!ui_test_modem_data(NULL));
}

/* Typed, then the page left without Enter: set all the same */
TEST(apn_set_by_leaving)
{
	registered_shade_closed();
	CHECK(network_page());
	CHECK(ui_test_tap_field("Automatic"));
	CHECK(ui_test_type("net.example"));
	CHECK(ui_test_back());
	CHECK(attached_with("net.example", 5000));
	CHECK(strstr(file(SETTINGS), "apn=net.example"));
}

/* Saved from the last time, the modem without it: set once at startup */
TEST(apn_applied_at_startup)
{
	ui_test_write(SETTINGS, "mobile_data=1\napn=net.example\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 10000));
	unsigned reattached;
	CHECK_STR(ui_test_modem_apn(&reattached), "net.example");
	CHECK_INT(reattached, 1);
}

/* The modem has it already (it keeps it): not attached again */
TEST(apn_kept_by_the_modem)
{
	ui_test_write(SETTINGS, "mobile_data=1\napn=net.example\n");
	ui_test_modem_context_apn("net.example");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	unsigned reattached;
	CHECK_STR(ui_test_modem_apn(&reattached), "net.example");
	CHECK_INT(reattached, 0);
}

/* Emptied: back to the network's own */
TEST(apn_back_to_automatic)
{
	ui_test_write(SETTINGS, "apn=net.example\n");
	ui_test_modem_context_apn("net.example");
	registered_shade_closed();
	CHECK(network_page());
	lv_obj_t *field = apn_field();
	CHECK(field);
	if (!field)
		return;
	CHECK_STR(lv_textarea_get_text(field), "net.example");
	CHECK(apn_enter(""));
	CHECK(attached_with("internet", 5000));
	CHECK(!strstr(file(SETTINGS), "apn=net.example"));
}

/* Only what an APN is made of: letters, digits, dots and dashes (nothing that would
 * end the AT command's string) */
TEST(apn_characters)
{
	registered_shade_closed();
	CHECK(network_page());
	CHECK(apn_enter("my apn\",x-1.com"));
	CHECK(attached_with("myapnx-1.com", 5000));
}

/* Saved as the keyboard is put away, not as each key is typed */
TEST(apn_saved_when_done)
{
	registered_shade_closed();
	CHECK(network_page());
	CHECK(ui_test_tap_field("Automatic"));
	CHECK(ui_test_type("net"));
	ui_test_wait(2000);
	unsigned reattached;
	ui_test_modem_apn(&reattached);
	CHECK_INT(reattached, 0);
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(attached_with("net", 5000));
}

/* Done without a change: the modem not attached again */
TEST(apn_unchanged_not_applied)
{
	registered_shade_closed();
	CHECK(network_page());
	CHECK(ui_test_tap_field("Automatic"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	ui_test_wait(5000);
	unsigned reattached;
	ui_test_modem_apn(&reattached);
	CHECK_INT(reattached, 0);
	CHECK(!strstr(file(SETTINGS), "apn="));
}

/* Data up while Wi-Fi is connected: no technology, Wi-Fi is used (its route first); so
 * the status bar at its widest (roaming, data up, Wi-Fi, USB, "100%") clears the clock */
TEST(status_bar_no_technology_over_wifi)
{
	ui_test_write(SETTINGS, "mobile_data=1\ndata_roaming=1\nbattery_percent=1\n");
	ui_test_write("/sys/class/power_supply/axp20x-battery/capacity", "100\n");
	ui_test_write("/sys/class/net/usb0/carrier", "1\n");
	ui_test_wifi_ap("Home", -50, "[WPA2-PSK-CCMP][ESS]", "secret123");
	ui_test_wifi_saved("Home", "secret123");
	ui_test_modem_network(UI_TEST_NET_ROAMING, "Abroad");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	bool wifi = false;
	for (int t = 0; t < 5000 && !wifi; t += 48) {
		wifi = ui_wifi_bars() >= 0;
		ui_test_wait(48);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	CHECK(wifi);
	ui_test_wait(1100);
	lv_obj_t *usb = in_status_bar(UI_SYMBOL_USB_270), *clock = NULL;
	/* The clock: the bar's label with a colon */
	for (uint32_t i = 0; usb && i < lv_obj_get_child_count(lv_obj_get_parent(usb)); i++) {
		lv_obj_t *o = lv_obj_get_child(lv_obj_get_parent(usb), (int32_t)i);
		if (lv_obj_check_type(o, &lv_label_class) && strchr(lv_label_get_text(o), ':'))
			clock = o;
	}
	CHECK(!in_status_bar("4G"));
	CHECK(usb && clock && in_status_bar("R"));
	if (!usb || !clock)
		return;
	lv_area_t u, c;
	lv_obj_get_coords(usb, &u);
	lv_obj_get_coords(clock, &c);
	CHECK(u.x1 > c.x2);
}
