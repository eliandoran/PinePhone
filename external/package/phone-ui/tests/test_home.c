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

/* Turning with the phone (turn.c), held still: upright, on either side, flat, upside
 * down. The accelerometer reads up, turned by its mount matrix (ui_test_sensors()). */
static lv_display_rotation_t held(float x, float y, float z)
{
	const float up[3] = { x, y, z }, field[3] = { 0, 0.2f, -0.4f };
	ui_test_sensors_feed(up, field, 1);
	ui_test_wait(1000); /* read a few times */
	return lv_display_get_rotation(lv_display_get_default());
}

TEST(turns_with_the_phone)
{
	ui_test_sensors(true, false);
	ui_test_start();
	CHECK_INT(held(0, 1, 0), LV_DISPLAY_ROTATION_0);
	CHECK_INT(held(1, 0, 0), LV_DISPLAY_ROTATION_90);  /* right edge up: the top to the left */
	CHECK_INT(held(0, 0, 1), LV_DISPLAY_ROTATION_90);  /* laid flat: as it was */
	CHECK_INT(held(-1, 0, 0), LV_DISPLAY_ROTATION_270);
	CHECK_INT(held(0, -1, 0), LV_DISPLAY_ROTATION_270); /* upside down: as it was */
	CHECK_INT(held(0, 0.95f, 0.3f), LV_DISPLAY_ROTATION_0); /* leaning back */
}

/* The menu follows the finger up from the home screen and down from its top (home.c's
 * drag, ui.c's ui_menu_drag_*): let go past a third or flicked, it opens or closes; else
 * it returns. Then the screens stack as with the buttons. */

#define X 360
#define LOW 1100 /* above the dock */

static lv_obj_t *home;

static void at_home(void)
{
	ui_test_start();
	home = lv_screen_active();
	CHECK(!ui_test_shows("Menu"));
}

static bool menu_open(void)
{
	return lv_screen_active() != home && lv_screen_active() == ui_current() && ui_at_home() &&
	       ui_test_shows("Menu");
}

static bool menu_closed(void)
{
	return lv_screen_active() == home && ui_current() == home && !ui_test_shows("Menu");
}

/* An app opened from the menu goes back to it */
static void menu_works(void)
{
	lv_obj_t *menu = lv_screen_active();
	CHECK(ui_test_tap_text("Notes"));
	CHECK(ui_current() != menu);
	CHECK(ui_test_back());
	CHECK(lv_screen_active() == menu);
	CHECK(menu_open());
}

TEST(drag_up_opens_the_menu)
{
	at_home();
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
	menu_works();
}

TEST(slow_drag_past_a_third_opens_the_menu)
{
	at_home();
	ui_test_swipe(X, LOW, X, LOW - 600, 2500);
	CHECK(menu_open());
}

TEST(quick_short_flick_opens_the_menu)
{
	at_home();
	ui_test_swipe(X, LOW, X, LOW - 150, 100);
	CHECK(menu_open());
}

TEST(slow_short_drag_stays_home)
{
	at_home();
	ui_test_swipe(X, LOW, X, LOW - 200, 1500);
	CHECK(menu_closed());
	/* And the menu still opens, its apps go back to it */
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
	menu_works();
}

TEST(drag_down_closes_the_menu)
{
	at_home();
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
	ui_test_swipe(X, 200, X, 1000, 250);
	CHECK(menu_closed());
	/* Opens again */
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
}

TEST(slow_short_drag_down_keeps_the_menu)
{
	at_home();
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
	ui_test_swipe(X, 200, X, 400, 1500);
	CHECK(menu_open());
	menu_works();
}

/* Down is not the way to the menu, nor up once it is open */
TEST(drag_the_wrong_way_does_nothing)
{
	at_home();
	ui_test_swipe(X, 300, X, LOW, 250);
	CHECK(menu_closed());
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
	ui_test_swipe(X, LOW, X, 300, 250);
	CHECK(menu_open());
}
