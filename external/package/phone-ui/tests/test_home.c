/* The home screen and the menu (home.c), through the UI: phone-ui on the host
 * (ui_test.h) */

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define MAX_ICONS 32
#define INSIDE 12 /* px along the diagonal: past the rounded corner (8 px in), before the symbol */

TEST_SETUP
{
	ui_test_reset();
}

static int min(int a, int b)
{
	return a < b ? a : b;
}

/* The app icons' rounded corners blend into the wallpaper: from the corner (the
 * wallpaper) to inside the icon, each pixel lies between the two, no darker. Their
 * edges are half transparent; blending them with their alpha applied twice drew a
 * dark outline, seen on the light wallpaper. */
TEST(menu_icons_edges)
{
	ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	ui_open_menu();
	ui_test_settle();
	lv_obj_t *icons[MAX_ICONS];
	unsigned n = min(ui_test_find_widgets(&lv_3dtexture_class, icons, MAX_ICONS), MAX_ICONS);
	CHECK(n > 0);
	for (unsigned i = 0; i < n; i++) {
		lv_area_t a;
		lv_obj_get_coords(icons[i], &a);
		if (a.y1 < 0 || a.y1 + INSIDE >= 1440)
			continue;
		lv_color_t wall = ui_test_pixel(a.x1, a.y1), in = ui_test_pixel(a.x1 + INSIDE, a.y1 + INSIDE);
		for (int d = 0; d < INSIDE; d++) {
			lv_color_t c = ui_test_pixel(a.x1 + d, a.y1 + d);
			if (c.red + 4 < min(wall.red, in.red) || c.green + 4 < min(wall.green, in.green) ||
			    c.blue + 4 < min(wall.blue, in.blue)) {
				TEST_FAIL("icon at (%d, %d), %d px in: #%02x%02x%02x, darker than "
				          "#%02x%02x%02x and #%02x%02x%02x", (int)a.x1, (int)a.y1, d,
				          c.red, c.green, c.blue, wall.red, wall.green, wall.blue,
				          in.red, in.green, in.blue);
				break;
			}
		}
	}
}
