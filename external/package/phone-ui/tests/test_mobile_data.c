/* Mobile data (modem.c, modem_at.c) through the UI (ui_test.h): the shade's toggle, the
 * modem's data session (AT$QCRMCALL on fake_modem.c) and the board's script that sets up
 * the network interface (ui_test_reset()'s logs what it is asked) */

#include <stdio.h>
#include <string.h>

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
	ui_test_wait(4000);
	CHECK(!ui_test_modem_data(NULL));
	CHECK_STR(file("/tmp/mobile-data.log"), "down wwan0\n"); /* the startup's cleanup only */
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

/* The status bar's technology ("4G"): lit while the data session is up, dim otherwise */
static bool tech_lit(void)
{
	lv_obj_t *tech = ui_test_find("4G");
	return tech && lv_color_eq(lv_obj_get_style_text_color(tech, 0), lv_color_hex(UI_COLOR_TEXT));
}

static bool tech_dim(void)
{
	lv_obj_t *tech = ui_test_find("4G");
	return tech && lv_color_eq(lv_obj_get_style_text_color(tech, 0), lv_color_hex(UI_COLOR_DIM));
}

/* Data off: the technology dim, no arrows */
TEST(status_bar_data_off)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for("4G", 3000));
	ui_test_wait(2000);
	CHECK(tech_dim());
	CHECK(!ui_test_shows(UI_SYMBOL_SORT));
}

/* Connected: the technology lit, the arrows after it */
TEST(status_bar_data_connected)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("up wwan0", 5000));
	CHECK(ui_test_wait_for(UI_SYMBOL_SORT, 2000));
	CHECK(tech_lit());
	lv_area_t tech, arrows;
	lv_obj_get_coords(ui_test_find("4G"), &tech);
	lv_obj_get_coords(ui_test_find(UI_SYMBOL_SORT), &arrows);
	CHECK(arrows.x1 > tech.x2);
	CHECK(arrows.x1 - tech.x2 - 1 <= 8); /* beside it */
}

/* On but refused: not shown as connected until the session is up */
TEST(status_bar_data_refused)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_modem_data_fails(1);
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_mobile_data_ran("reset wwan0", 5000));
	ui_test_wait(1500);
	CHECK(!ui_test_shows(UI_SYMBOL_SORT));
	CHECK(tech_dim());
	CHECK(ui_test_mobile_data_ran("up wwan0", 15000));
	CHECK(ui_test_wait_for(UI_SYMBOL_SORT, 2000));
}

/* Turned off, or the network lost: the arrows go */
TEST(status_bar_data_ends)
{
	ui_test_write(SETTINGS, "mobile_data=1\n");
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_for(UI_SYMBOL_SORT, 5000));
	shade_open();
	CHECK(ui_test_tap_text(TILE));
	ui_test_wait(1500);
	CHECK(!ui_test_shows(UI_SYMBOL_SORT));
	CHECK(tech_dim());
	CHECK(ui_test_tap_text(TILE));
	CHECK(ui_test_wait_for(UI_SYMBOL_SORT, 5000));
	ui_test_modem_network(UI_TEST_NET_SEARCHING, NULL);
	ui_test_wait(2000);
	CHECK(!ui_test_shows(UI_SYMBOL_SORT));
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
