/* docs/apps/maps.md's screenshots (included by test_docs.c): tiles of a made-up town drawn
 * here in OpenStreetMap's colors (streets, a river, a park, houses), served by the tests */

#include <math.h>
#include <png.h>

#define MAPS_TILES "https://tile.openstreetmap.org/"
#define MAPS_ZOOM 16
#define MAPS_LAT "47.00000"
#define MAPS_LON "8.00000"
#define MAPS_LAND 0xf2efe9
#define MAPS_WATER 0xaad3df
#define MAPS_GRASS 0xc8facc
#define MAPS_ROAD 0xfcd6a4

/* Streets of the town, pixels from the view's center; MAPS_MAIN_* are the wide ones */
static const int maps_streets_u[] = { -700, -480, -280, -90, 30, 200, 330, 520, 700 };
static const int maps_streets_v[] = { -900, -760, -590, -420, -250, 0, 170, 330, 620, 800, 950 };
#define MAPS_MAIN_U 30
#define MAPS_MAIN_V 0

/* The block of s (n streets) around x: its streets a and b; false outside the town */
static bool maps_block(const int *s, int n, int x, int *a, int *b)
{
	for (int i = 0; i + 1 < n; i++)
		if (x >= s[i] && x < s[i + 1]) {
			*a = s[i];
			*b = s[i + 1];
			return true;
		}
	return false;
}

/* The town at (u, v), pixels from the view's center: a main street each way, a river
 * winding across, a park with a pond, houses around the blocks */
static uint32_t maps_town(int u, int v)
{
	int u0, u1, v0, v1;
#define MAPS_COUNT(a) (int)(sizeof(a) / sizeof(*(a)))
	if (!maps_block(maps_streets_u, MAPS_COUNT(maps_streets_u), u, &u0, &u1) ||
	    !maps_block(maps_streets_v, MAPS_COUNT(maps_streets_v), v, &v0, &v1))
		return MAPS_LAND;
	int du = LV_MIN(u - u0, u1 - u), dv = LV_MIN(v - v0, v1 - v);
	int su = u - u0 < u1 - u ? u0 : u1, sv = v - v0 < v1 - v ? v0 : v1; /* the nearest */
	double river = fabs(v - (500 + 110 * sin(u / 260.0)));

	if ((su == MAPS_MAIN_U && du <= 9) || (sv == MAPS_MAIN_V && dv <= 9))
		return MAPS_ROAD;
	if ((su == MAPS_MAIN_U && du <= 12) || (sv == MAPS_MAIN_V && dv <= 12))
		return 0xd6a86a;
	if (river <= 34)
		return MAPS_WATER;
	if (river <= 60)
		return MAPS_GRASS;
	/* The park: two blocks, the street between them gone */
	if (u >= -272 && u <= -98 && v >= -582 && v <= -258) {
		double pu = (u + 185) / 55.0, pv = (v + 470) / 36.0;
		if (pu * pu + pv * pv <= 1)
			return MAPS_WATER;
		return abs(v + 330) <= 2 || abs(u + 140) <= 2 ? 0xfa8072 : MAPS_GRASS;
	}
	if (du <= 8 || dv <= 8)
		return LV_MIN(du, dv) <= 6 ? 0xffffff : 0xd3cbc2;
	/* A school: one large building and its yard */
	if (u0 == 200 && v0 == -250) {
		if (u >= 220 && u <= 310 && v >= -230 && v <= -150)
			return 0xd9d0c9;
		return u > 214 && u < 316 && v > -236 && v < -20 ? 0xe9e2d6 : MAPS_LAND;
	}
	/* Houses around the block's edge, of a width each block has */
	int edge = LV_MIN(du, dv) - 8;
	int along = du < dv ? v - v0 : u - u0;
	int lot = 26 + (abs(u0 * 7 + v0 * 13) % 17);
	if (edge >= 5 && edge < 38 && along % lot >= 3)
		return 0xd9d0c9;
	return MAPS_LAND;
}

/* Tile (x, y) of MAPS_ZOOM as a PNG, (ox, oy) the view's center in the world's pixels */
static void *maps_tile(int x, int y, double ox, double oy, size_t *size)
{
	uint8_t *px = malloc(256 * 256 * 3);
	for (int j = 0; j < 256; j++)
		for (int i = 0; i < 256; i++) {
			uint32_t c = maps_town((int)floor(x * 256 + i - ox), (int)floor(y * 256 + j - oy));
			uint8_t *p = px + (j * 256 + i) * 3;
			p[0] = c >> 16;
			p[1] = c >> 8;
			p[2] = c;
		}
	png_image image = { .version = PNG_IMAGE_VERSION, .width = 256, .height = 256, .format = PNG_FORMAT_RGB };
	png_alloc_size_t n = 0;
	void *png = NULL;
	if (png_image_write_to_memory(&image, NULL, &n, 0, px, 0, NULL) && (png = malloc(n)) &&
	    !png_image_write_to_memory(&image, png, &n, 0, px, 0, NULL)) {
		free(png);
		png = NULL;
	}
	free(px);
	*size = png ? n : 0;
	return png;
}

/* maps.md: the map, zoomed in on a town */
TEST(maps_town)
{
	/* The view left there last time (the app keeps it) */
	ui_setting_set("maps.lat", MAPS_LAT);
	ui_setting_set("maps.lon", MAPS_LON);
	ui_setting_set_int("maps.zoom", MAPS_ZOOM);
	double world = 256.0 * (1 << MAPS_ZOOM), lat = atof(MAPS_LAT), lon = atof(MAPS_LON);
	double ox = (lon + 180) / 360 * world;
	double oy = (1 - asinh(tan(lat * M_PI / 180)) / M_PI) / 2 * world;

	size_t n;
	void *land = ui_test_png(256, 256, MAPS_LAND, &n);
	ui_test_serve_data(MAPS_TILES "*", 200, "image/png", land, n);
	free(land);
	int tx = (int)(ox / 256), ty = (int)(oy / 256);
	for (int x = tx - 3; x <= tx + 3; x++)
		for (int y = ty - 4; y <= ty + 4; y++) {
			char url[96];
			snprintf(url, sizeof(url), MAPS_TILES "%d/%d/%d.png", MAPS_ZOOM, x, y);
			void *png = maps_tile(x, y, ox, oy, &n);
			ui_test_serve_data(url, 200, "image/png", png, n);
			free(png);
		}

	CHECK(ui_test_open("Maps"));
	ui_test_wait(3000);
	ui_test_settle();
	char url[96];
	snprintf(url, sizeof(url), MAPS_TILES "%d/%d/%d.png", MAPS_ZOOM, tx, ty);
	CHECK(ui_test_requested(url) != NULL);
	lv_obj_t *tiles[64];
	CHECK(ui_test_find_widgets(&lv_3dtexture_class, tiles, 64) >= 12);
	CHECK(ui_test_shows("\xc2\xa9 OpenStreetMap contributors"));
	CHECK(ui_test_shows(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows(LV_SYMBOL_MINUS));
	CHECK(!ui_test_shows("No internet connection"));
	/* The map's center: the wide street */
	lv_obj_t *map = lv_obj_get_parent(lv_obj_get_parent(tiles[0]));
	lv_area_t a;
	lv_obj_get_coords(map, &a);
	lv_color_t c = ui_test_pixel((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
	CHECK(c.red == 0xfc && c.green == 0xd6 && c.blue == 0xa4);
	shot("maps-town");
}
