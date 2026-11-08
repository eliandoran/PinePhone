/* Going between screens (nav.c, transition.c, gesture.c): the back gesture from the left edge and the home
 * gesture from the bottom edge, through the UI: phone-ui on the host (ui_test.h) */

#include "test.h"
#include "ui_test.h"

#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h" /* the keys' areas */

#define Y 700        /* the middle of the screen, below the status bar */
#define EDGE_X 4     /* in the edge strip (28 px) */
#define HALF 360     /* half the screen's width */
#define X 360        /* the middle of the screen's width */
#define BOTTOM_Y 1436 /* in the bottom strip */

static lv_obj_t *home; /* the home screen, shown at startup */
static lv_obj_t *app;  /* the app opened, on top */

TEST_SETUP
{
	ui_test_start();
	home = ui_current();
	if (!ui_test_open("Notes"))
		TEST_FAIL("Notes did not open");
	app = ui_current();
}

/* The finger from the edge to x1 in ms1, then to x2 in ms2, let go */
static void drag(int32_t x1, uint32_t ms1, int32_t x2, uint32_t ms2)
{
	lv_test_mouse_move_to(EDGE_X, Y);
	lv_test_mouse_press();
	ui_test_wait(16);
	int32_t x = EDGE_X;
	const int32_t to[2] = { x1, x2 };
	const uint32_t ms[2] = { ms1, ms2 };
	for (int s = 0; s < 2; s++) {
		uint32_t frames = ms[s] / 16 ? ms[s] / 16 : 1;
		for (uint32_t i = 1; i <= frames; i++) {
			lv_test_mouse_move_to(x + (to[s] - x) * (int32_t)i / (int32_t)frames, Y);
			ui_test_wait(16);
		}
		x = to[s];
	}
	lv_test_mouse_release();
	ui_test_settle();
}

/* The app went, the menu is back on the screen */
static bool went_back(void)
{
	return ui_current() != app && ui_at_home() && lv_screen_active() == ui_current();
}

/* Still in the app, which the screen shows, and its back button still goes back */
static void stayed(void)
{
	CHECK(ui_current() == app);
	CHECK(lv_screen_active() == app);
	CHECK(ui_test_back());
	CHECK(went_back());
}

TEST(swipe_across_goes_back)
{
	ui_test_swipe(EDGE_X, Y, 650, Y, 250);
	CHECK(went_back());
}

TEST(slow_swipe_past_half_goes_back)
{
	ui_test_swipe(EDGE_X, Y, HALF + 140, Y, 2500);
	CHECK(went_back());
}

TEST(quick_short_flick_goes_back)
{
	ui_test_swipe(EDGE_X, Y, 150, Y, 100);
	CHECK(went_back());
}

TEST(slow_short_swipe_stays)
{
	ui_test_swipe(EDGE_X, Y, 150, Y, 1500);
	stayed();
}

/* Past half way, then flicked back to the left */
TEST(swipe_past_half_flicked_back_stays)
{
	drag(HALF + 140, 800, HALF + 40, 64);
	stayed();
}

/* Not past the slop: the screen did not follow, a tap on the edge does nothing */
TEST(touch_on_the_edge_stays)
{
	ui_test_swipe(EDGE_X, Y, EDGE_X + 6, Y, 100);
	stayed();
}

/* At home and in the menu, there is nothing to go back to: no edge strip */
TEST(no_back_gesture_in_the_menu)
{
	CHECK(ui_test_back());
	lv_obj_t *menu = lv_screen_active();
	ui_test_swipe(EDGE_X, Y, 650, Y, 250);
	CHECK(lv_screen_active() == menu);
	CHECK(ui_current() == menu);
}

/* Back, then into an app again and back again: the stack is as before */
TEST(swipe_back_twice)
{
	ui_test_swipe(EDGE_X, Y, 650, Y, 250);
	CHECK(went_back());
	CHECK(ui_test_tap_text("Notes"));
	app = ui_current();
	CHECK(lv_screen_active() == app);
	ui_test_swipe(EDGE_X, Y, 650, Y, 250);
	CHECK(went_back());
}

/* Home gesture: a swipe up from the bottom edge */

/* The home screen on the screen, nothing above it */
static bool went_home(void)
{
	return ui_current() == home && lv_screen_active() == home;
}

TEST(swipe_up_from_the_bottom_goes_home)
{
	ui_test_swipe(X, BOTTOM_Y, X, 900, 250);
	CHECK(went_home());
}

TEST(quick_short_flick_up_goes_home)
{
	ui_test_swipe(X, BOTTOM_Y, X, 1300, 100);
	CHECK(went_home());
}

TEST(slow_short_swipe_up_stays)
{
	ui_test_swipe(X, BOTTOM_Y, X, 1300, 1500);
	stayed();
}

/* The finger from (x1, y1) quickly to (x2, y2), then held still there, or let go while
 * the UI was busy: either way, still when it lifted */
static void flick_and_hold(int32_t x1, int32_t y1, int32_t x2, int32_t y2, bool busy)
{
	lv_test_mouse_move_to(x1, y1);
	lv_test_mouse_press();
	ui_test_wait(16);
	for (int i = 1; i <= 4; i++) {
		lv_test_mouse_move_to(x1 + (x2 - x1) * i / 4, y1 + (y2 - y1) * i / 4);
		ui_test_wait(16);
	}
	if (busy) {
		ui_test_wait(16);
		lv_test_mouse_release();
		ui_test_stall(300);
	} else {
		ui_test_wait(300);
		lv_test_mouse_release();
	}
	ui_test_settle();
}

/* A flick held still before letting go is no flick: short of the way, it stays */
TEST(quick_short_flick_held_stays)
{
	flick_and_hold(EDGE_X, Y, 150, Y, false);
	stayed();
}

TEST(quick_short_flick_up_held_stays)
{
	flick_and_hold(X, BOTTOM_Y, X, 1300, false);
	stayed();
}

/* Lifted after a long frame: the flick's speed is that old, it stays too */
TEST(quick_short_flick_lifted_in_a_slow_frame_stays)
{
	flick_and_hold(EDGE_X, Y, 150, Y, true);
	stayed();
}

TEST(quick_short_flick_up_lifted_in_a_slow_frame_stays)
{
	flick_and_hold(X, BOTTOM_Y, X, 1300, true);
	stayed();
}

/* From a page of an app, past the screens below it: the menu shows next time */
TEST(swipe_up_from_a_page_goes_home)
{
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	lv_obj_t *page = ui_current();
	ui_test_swipe(X, BOTTOM_Y, X, 900, 250);
	CHECK(went_home());
	CHECK(!lv_obj_is_in_widget_tree(page));
	ui_open_menu();
	ui_test_settle();
	CHECK(ui_test_shows("Settings"));
}

/* At home, a swipe up from the bottom opens the menu, as from anywhere on it */
TEST(swipe_up_at_home_opens_the_menu)
{
	ui_home();
	ui_test_settle();
	ui_test_swipe(X, BOTTOM_Y, X, 600, 250);
	CHECK(ui_at_home());
	CHECK(ui_current() != home);
}

/* The keyboard's keys end above the strip, which goes home while typing too */
TEST(keyboard_above_the_strip)
{
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	lv_obj_t *kb = ui_test_find_widget(&lv_keyboard_class);
	CHECK(kb);
	if (!kb)
		return;
	lv_area_t c;
	lv_obj_get_coords(kb, &c);
	CHECK(c.y2 >= BOTTOM_Y); /* down to the edge, its color under the strip */
	/* Each key above the strip */
	const lv_buttonmatrix_t *m = (const lv_buttonmatrix_t *)kb;
	for (uint32_t i = 0; i < m->btn_cnt; i++)
		CHECK(c.y1 + m->button_areas[i].y2 < 1440 - UI_HOME_EDGE_H);
	ui_test_tap_at(X, 1440 - UI_HOME_EDGE_H - 30); /* the space bar */
	CHECK_STR(ui_test_typed(), " ");

	ui_test_swipe(X, BOTTOM_Y, X, 900, 250);
	CHECK(went_home());
}

TEST(keyboard_strip_tap_types_nothing)
{
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	ui_test_tap_at(X, BOTTOM_Y);
	CHECK(ui_current() != home);
	CHECK_STR(ui_test_typed(), "");
}
