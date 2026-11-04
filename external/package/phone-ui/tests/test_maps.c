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
