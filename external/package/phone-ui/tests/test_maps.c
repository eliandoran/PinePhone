/*
 * Maps (maps.c) with OpenStreetMap's tiles served by the tests (fake_curl.c):
 * downloads two at a time, the tile cache, offline, zooming and panning.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define TILES "/var/lib/maps/tiles"
#define TILE_URL "https://tile.openstreetmap.org/"

static char *file(const char *path)
{
	static char buf[1024];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

static bool exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static void serve_tiles(int size)
{
	size_t n;
	void *png = ui_test_png(size, size, 0xaad3df, &n);
	ui_test_serve_data(TILE_URL "*", 200, "image/png", png, n);
	free(png);
}

/* Tiles shown: their quads not hidden */
static unsigned shown(void)
{
	lv_obj_t *found[64];
	return ui_test_find_widgets(&lv_3dtexture_class, found, 64);
}

TEST_SETUP
{
	ui_test_reset();
}

/* Zoom 2 at first: the world is 4 x 4 tiles, all of them in view */
TEST(tiles_downloaded_and_kept)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(ui_test_requested(TILE_URL "2/1/1.png") != NULL);
	CHECK(shown() >= 12);
	CHECK(exists(TILES "/2/1/1.png"));
	CHECK(exists(TILES "/2/3/3.png"));
	CHECK(!ui_test_shows("No internet connection"));
	CHECK(ui_test_shows("\xc2\xa9 OpenStreetMap contributors"));
}

/* As the tile usage policy asks */
TEST(two_downloads_at_a_time)
{
	serve_tiles(256);
	ui_test_network_hold(true);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	CHECK_INT(ui_test_request_count(), 2);
	ui_test_network_hold(false);
	ui_test_wait(2000);
	CHECK(ui_test_request_count() >= 16);
}

TEST(kept_tiles_not_downloaded)
{
	size_t n;
	void *png = ui_test_png(256, 256, 0xf2efe9, &n);
	char path[64];
	for (int x = 0; x < 4; x++)
		for (int y = 0; y < 4; y++) {
			snprintf(path, sizeof(path), TILES "/2/%d/%d.png", x, y);
			ui_test_write(path, "");
			FILE *f = fopen(path, "w");
			fwrite(png, 1, n, f);
			fclose(f);
		}
	free(png);
	ui_test_write(TILES "/2/0/0.png", "not a PNG"); /* unreadable: fetched again */
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(shown() >= 12);
	CHECK_INT(ui_test_request_count(), 1);
	CHECK(ui_test_requested(TILE_URL "2/0/0.png") != NULL);
}

/* Not retried for 30 s, then shown */
TEST(offline)
{
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(ui_test_shows("No internet connection"));
	CHECK_INT(shown(), 0);
	unsigned tried = ui_test_request_count();
	CHECK(tried >= 16);
	serve_tiles(256);
	ui_test_wait(2000);
	CHECK_INT(ui_test_request_count(), tried);
	ui_test_wait(30000);
	CHECK(ui_test_request_count() > tried);
	CHECK(!ui_test_shows("No internet connection"));
	CHECK(shown() >= 12);
}

/* A tile the server lacks, or not a tile: not a connection problem */
TEST(missing_and_wrong_tiles)
{
	serve_tiles(128); /* not 256 x 256 */
	ui_test_serve(TILE_URL "2/1/1.png", 404, "text/html", "<h1>Not Found</h1>");
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(!ui_test_shows("No internet connection"));
	CHECK_INT(shown(), 0);
	CHECK(!exists(TILES "/2/1/1.png"));
	CHECK(!exists(TILES "/2/0/0.png"));
}

TEST(zoom_buttons)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	ui_test_wait(2000);
	CHECK(ui_test_requested(TILE_URL "3/*") != NULL);
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	ui_test_wait(2000);
	CHECK(ui_test_requested(TILE_URL "4/*") != NULL);
	CHECK(ui_test_tap_text(LV_SYMBOL_MINUS));
	ui_test_wait(500);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=3\n") != NULL);
}

TEST(double_tap_zooms_in)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	ui_test_tap_at(300, 700);
	ui_test_wait(100);
	ui_test_tap_at(300, 700);
	ui_test_wait(2000);
	CHECK(ui_test_requested(TILE_URL "3/*") != NULL);
}

/* Dragged, then back: the view saved where it was left */
TEST(panned_view_saved)
{
	ui_test_write(SETTINGS, "maps.lat=48.85000\nmaps.lon=2.35000\nmaps.zoom=12\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	CHECK(ui_test_requested(TILE_URL "12/2074/1409.png") != NULL); /* Paris */
	ui_test_swipe(600, 700, 100, 700, 500); /* east */
	ui_test_wait(1000);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.lat=48.85000\n") != NULL);
	CHECK(strstr(file(SETTINGS), "maps.lon=2.35") == NULL);
	CHECK(strstr(file(SETTINGS), "maps.zoom=12\n") != NULL);
}

/* Closed while tiles download: nothing arrives in the freed map */
TEST(closed_while_downloading)
{
	serve_tiles(256);
	ui_test_network_hold(true);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	ui_test_wait(1000);
	CHECK(ui_at_home());
	CHECK(!exists(TILES "/2"));
}

/* Two fingers apart: the next level, then the one after, kept once they lift */
TEST(pinch_zooms_in)
{
	ui_test_write(SETTINGS, "maps.lat=48.85000\nmaps.lon=2.35000\nmaps.zoom=12\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	ui_test_pinch(360, 700, 200, 800, 500, true); /* 4 times as far: 2 levels */
	ui_test_wait(1000);
	CHECK(ui_test_requested(TILE_URL "14/*") != NULL);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=14\n") != NULL);
	/* Around the point between the fingers */
	CHECK(strstr(file(SETTINGS), "maps.lat=48.8") != NULL);
	CHECK(strstr(file(SETTINGS), "maps.lon=2.3") != NULL);
}

/* Together: out, not further than the whole world */
TEST(pinch_zooms_out)
{
	ui_test_write(SETTINGS, "maps.zoom=3\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	ui_test_pinch(360, 700, 600, 50, 500, true);
	ui_test_wait(500);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=2\n") != NULL);
}

/* The fingers' end not seen: the next touch settles on the level the pinch was drawing */
TEST(pinch_end_unseen)
{
	ui_test_write(SETTINGS, "maps.lat=48.85000\nmaps.lon=2.35000\nmaps.zoom=12\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	ui_test_pinch(360, 700, 200, 400, 300, false);
	ui_test_tap_at(360, 900);
	ui_test_wait(1000);
	CHECK(ui_test_requested(TILE_URL "13/*") != NULL);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=13\n") != NULL);
}

/* At the closest level already: the fingers apart change nothing */
TEST(pinch_beyond_closest_level)
{
	ui_test_write(SETTINGS, "maps.lat=48.85000\nmaps.lon=2.35000\nmaps.zoom=19\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	ui_test_pinch(360, 700, 200, 800, 500, true);
	ui_test_wait(500);
	CHECK(ui_test_requested(TILE_URL "20/*") == NULL);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=19\n") != NULL);
}

/* The whole world shown: zooming out does nothing */
TEST(zoom_out_at_widest)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	CHECK(ui_test_tap_text(LV_SYMBOL_MINUS));
	ui_test_wait(1000);
	CHECK(ui_test_requested(TILE_URL "1/*") == NULL);
	CHECK(shown() >= 12);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=2\n") != NULL);
}

/* Out of range in the settings: the whole world */
TEST(saved_zoom_out_of_range)
{
	ui_test_write(SETTINGS, "maps.zoom=40\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	CHECK(ui_test_requested(TILE_URL "2/1/1.png") != NULL);
	CHECK(shown() >= 12);
}

/* + pressed again before the first zoom ended: both levels */
TEST(zoom_pressed_twice_quickly)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1000);
	lv_obj_t *plus = ui_test_find(LV_SYMBOL_PLUS);
	CHECK(plus != NULL);
	if (!plus)
		return;
	lv_area_t a;
	lv_obj_get_coords(plus, &a);
	for (int i = 0; i < 2; i++) {
		lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
		lv_test_mouse_press();
		ui_test_wait(48);
		lv_test_mouse_release();
		ui_test_wait(48);
	}
	ui_test_wait(1000);
	CHECK(ui_test_back());
	CHECK(strstr(file(SETTINGS), "maps.zoom=4\n") != NULL);
}

/* Dragged east past the date line: the map goes round */
TEST(panned_round_the_world)
{
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(500);
	ui_test_swipe(50, 700, 650, 700, 500); /* the view west of longitude 10 by 210 deg */
	ui_test_wait(1000);
	CHECK(shown() >= 12);
	CHECK(ui_test_back());
	char *lon = strstr(file(SETTINGS), "maps.lon=");
	CHECK(lon != NULL);
	if (lon)
		CHECK(atof(lon + 9) > 100); /* east of the date line */
}

/* Dragged north as far as it goes: stops at the map's southern edge */
TEST(panned_to_the_south_edge)
{
	ui_test_write(SETTINGS, "maps.zoom=3\n");
	serve_tiles(256);
	char lat[2][32] = { "", "" };
	for (int i = 0; i < 2; i++) {
		CHECK(ui_test_open("Maps"));
		ui_test_wait(500);
		ui_test_swipe(360, 1300, 360, 300, 500);
		ui_test_swipe(360, 1300, 360, 300, 500);
		ui_test_wait(500);
		CHECK(ui_test_back());
		char *l = strstr(file(SETTINGS), "maps.lat=");
		CHECK(l != NULL);
		if (l)
			sscanf(l, "maps.lat=%31s", lat[i]);
	}
	CHECK(atof(lat[0]) < -45); /* the map's southern edge in view */
	CHECK_STR(lat[1], lat[0]);
}

/* More tiles than the GPU keeps: those no longer shown make room */
TEST(many_tiles_seen)
{
	ui_test_write(SETTINGS, "maps.lat=48.85000\nmaps.lon=2.35000\nmaps.zoom=10\n");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(1500);
	for (int i = 0; i < 5; i++) {
		CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
		ui_test_wait(1500);
	}
	CHECK(ui_test_requested(TILE_URL "15/*") != NULL);
	CHECK(shown() >= 12);
	for (int i = 0; i < 5; i++) {
		CHECK(ui_test_tap_text(LV_SYMBOL_MINUS));
		ui_test_wait(1500);
	}
	CHECK(shown() >= 12);
}

/* Cut short: not shown, not kept */
TEST(truncated_tile)
{
	size_t n;
	void *png = ui_test_png(256, 256, 0xaad3df, &n);
	ui_test_serve_data(TILE_URL "*", 200, "image/png", png, n / 2);
	free(png);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK_INT(shown(), 0);
	CHECK(!exists(TILES "/2/1/1.png"));
	CHECK(!ui_test_shows("No internet connection"));
}

/* Nowhere to keep them: shown all the same */
TEST(tiles_not_kept_when_storage_fails)
{
	ui_test_write("/var/lib/maps", "not a folder");
	serve_tiles(256);
	CHECK(ui_test_open("Maps"));
	ui_test_wait(2000);
	CHECK(shown() >= 12);
}
