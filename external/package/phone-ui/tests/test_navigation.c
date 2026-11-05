/* Going between screens (ui.c): the back gesture from the left edge, through the UI:
 * phone-ui on the host (ui_test.h) */

#include "test.h"
#include "ui_test.h"

#define Y 700        /* the middle of the screen, below the status bar */
#define EDGE_X 4     /* in the edge strip (28 px) */
#define HALF 360     /* half the screen's width */

static lv_obj_t *app; /* the app opened, on top */

TEST_SETUP
{
	ui_test_start();
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
