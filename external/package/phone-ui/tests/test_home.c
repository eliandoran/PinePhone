/* The home screen and the menu (home.c), through the UI: phone-ui on the host
 * (ui_test.h) */

#include <stdlib.h>
#include <time.h>

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

/* A built-in app named with a space gets its icon (icons/web_lite.svg), not the
 * colored square with a symbol */
TEST(menu_icon_name_with_space)
{
	ui_test_start();
	ui_open_menu();
	ui_test_settle();
	lv_obj_t *label = ui_test_find("Web Lite");
	CHECK(label != NULL);
	if (!label)
		return;
	lv_obj_t *icon = lv_obj_get_child(lv_obj_get_parent(label), 0);
	CHECK_INT(lv_obj_get_child_count(icon), 1);
	CHECK(lv_obj_check_type(lv_obj_get_child(icon, 0), &lv_3dtexture_class));
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
	CHECK(!ui_test_shows("Essentials"));
}

static bool menu_open(void)
{
	return lv_screen_active() != home && lv_screen_active() == ui_current() && ui_at_home() &&
	       ui_test_shows("Essentials");
}

static bool menu_closed(void)
{
	return lv_screen_active() == home && ui_current() == home && !ui_test_shows("Essentials");
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

/* No title over the apps but a handle to pull: they start right below the status bar.
 * The arrow beside it still closes the menu. */
TEST(menu_has_a_handle_not_a_title)
{
	at_home();
	ui_open_menu();
	ui_test_settle();
	CHECK(menu_open());
	CHECK(!ui_test_shows("Menu"));
	lv_area_t a;
	lv_obj_get_coords(ui_test_find("Essentials"), &a);
	CHECK(a.y1 < UI_STATUS_H + 72);
	CHECK(ui_test_tap_text(LV_SYMBOL_DOWN));
	ui_test_settle();
	CHECK(menu_closed());
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

/* The status bar: between its texts (the time centered, the battery on the right) */
#define BAR_X 480

static int channel_diff(lv_color_t a, lv_color_t b)
{
	int d = abs(a.red - b.red), g = abs(a.green - b.green), bl = abs(a.blue - b.blue);
	return d > g ? (d > bl ? d : bl) : (g > bl ? g : bl);
}

static lv_color_t wallpaper_pixel(lv_opa_t dim, int32_t x, int32_t y)
{
	const lv_draw_buf_t *w = ui_wallpaper(dim);
	const uint8_t *p = w->data + y * w->header.stride + x * 4;
	return lv_color_make(p[2], p[1], p[0]);
}

/* In an app, the status bar has the app's background (its dimmed wallpaper), not
 * a band of its own: what is drawn there is the wallpaper as it is below it */
static void app_bar_is_the_background(bool light)
{
	if (light)
		ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	CHECK(ui_test_open("Notes"));
	for (int32_t y = 2; y < UI_STATUS_H; y += 8) {
		lv_color_t shown = ui_test_pixel(BAR_X, y), wall = wallpaper_pixel(LV_OPA_70, BAR_X, y);
		if (channel_diff(shown, wall) > 1) {
			printf("at %d,%d: %02x%02x%02x, the background %02x%02x%02x\n", BAR_X, (int)y,
			       shown.red, shown.green, shown.blue, wall.red, wall.green, wall.blue);
			TEST_FAIL("the status bar is not the app's background");
			return;
		}
	}
}

TEST(app_status_bar_is_the_background)
{
	app_bar_is_the_background(false);
}

TEST(app_status_bar_is_the_background_light)
{
	app_bar_is_the_background(true);
}

/* A row's brightness around BAR_X, averaged over the wallpaper's noise */
static int row_level(int32_t y)
{
	int sum = 0;
	for (int32_t x = BAR_X; x < BAR_X + 16; x++) {
		lv_color_t c = ui_test_pixel(x, y);
		sum += c.red + c.green + c.blue;
	}
	return sum / 16;
}

/* On the home screen's bright wallpaper, the top is shaded for the status bar's
 * texts, fading out with no edge where the bar ends */
static void home_bar_fades(bool light)
{
	if (light)
		ui_test_write(SETTINGS, "light=1\n");
	ui_test_start();
	ui_test_settle();
	/* At the top, as much as the bar's background was (40%) */
	CHECK(channel_diff(ui_test_pixel(BAR_X, 2), wallpaper_pixel(LV_OPA_40, BAR_X, 2)) <= 1);
	for (int32_t y = 1; y < UI_STATUS_H * 3; y++) {
		int d = abs(row_level(y - 1) - row_level(y));
		if (d > 4) {
			printf("rows %d and %d differ by %d\n", (int)y - 1, (int)y, d);
			TEST_FAIL("an edge in the status bar's shade");
			return;
		}
	}
}

TEST(home_status_bar_fades)
{
	home_bar_fades(false);
}

TEST(home_status_bar_fades_light)
{
	home_bar_fades(true);
}

/* The home screen has a big clock: none in the status bar. Elsewhere (an app, the
 * menu) the status bar has it. */
TEST(status_bar_clock_only_away_from_home)
{
	ui_test_start();
	ui_test_settle();
	CHECK(!ui_test_status_bar_clock());
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_status_bar_clock());
	CHECK(ui_test_back()); /* to the menu, where Notes was opened */
	ui_test_settle();
	CHECK(ui_test_status_bar_clock());
	ui_test_swipe(X, 200, X, 1000, 250); /* the menu dragged down: home */
	ui_test_settle();
	CHECK(!ui_test_status_bar_clock());
	ui_test_swipe(X, LOW, X, 300, 250); /* and up again: the menu */
	ui_test_settle();
	CHECK(ui_test_status_bar_clock());
}

/* A wall clock set by the test (documentation screenshots, dates shown): the home
 * screen's clock and date are that moment, and it runs on with the tests' time */
TEST(wall_clock_set_by_the_test)
{
	ui_test_wall_clock(1773481300); /* Saturday 14 March 2026, 09:41:40 UTC */
	ui_test_start();
	ui_test_settle();
	CHECK(ui_test_shows("09:41"));
	CHECK(ui_test_shows("Saturday 14 March"));
	ui_test_wait(25000); /* less than the screen's timeout */
	CHECK(ui_test_shows("09:42"));
	CHECK(time(NULL) >= 1773481325);
}

/* In Romanian (Settings › Language): the apps' and the sections' names, the date; apps are
 * still found by their English names (--open) */
TEST(romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_wall_clock(1773481300); /* Saturday 14 March 2026 */
	ui_test_start();
	ui_test_settle();
	CHECK(ui_test_shows("sâmbătă 14 martie"));
	ui_open_menu();
	ui_test_settle();
	CHECK(ui_test_shows("Esențiale"));
	CHECK(ui_test_shows("Telefon"));
	CHECK(ui_test_shows("Setări"));
	CHECK(!ui_test_shows("Phone"));
	CHECK(ui_app_find("Phone"));
	CHECK(ui_test_tap_text("Setări"));
	CHECK(ui_test_shows("Afișaj"));
}
