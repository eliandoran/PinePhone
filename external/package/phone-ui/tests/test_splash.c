/*
 * The boot splash (splash.c), which main.c shows over the home screen at startup:
 * the kernel's logo on black, taking the touches, then faded out; once per boot.
 */

#include "test.h"
#include "ui_test.h"

TEST_SETUP
{
	ui_test_start();
}

static bool black(lv_color_t c)
{
	return c.red + c.green + c.blue < 10;
}

TEST(fades_into_home)
{
	lv_obj_t *home = lv_screen_active();
	uint32_t layer = lv_obj_get_child_count(lv_layer_top());
	CHECK(!black(ui_test_pixel(40, 1000)));
	ui_splash();
	ui_test_wait(16);
	CHECK_INT(lv_obj_get_child_count(lv_layer_top()), layer + 1);
	lv_obj_t *splash = lv_obj_get_child(lv_layer_top(), -1);
	CHECK(lv_obj_check_type(lv_obj_get_child(splash, 0), &lv_3dtexture_class)); /* the logo */
	CHECK(black(ui_test_pixel(40, 1000)));
	CHECK(black(ui_test_pixel(700, 1400)));
	/* Held as the kernel left it, then fading */
	ui_test_wait(100);
	CHECK(black(ui_test_pixel(40, 1000)));
	/* The touches are its until it is gone */
	ui_test_tap_at(360, 1000);
	CHECK(lv_screen_active() == home);
	ui_test_settle();
	CHECK_INT(lv_obj_get_child_count(lv_layer_top()), layer);
	CHECK(!black(ui_test_pixel(40, 1000)));
	CHECK(ui_test_open("Notes"));
}

/* phone-ui started again (`just ui`): no splash */
TEST(once_per_boot)
{
	uint32_t layer = lv_obj_get_child_count(lv_layer_top());
	ui_splash();
	ui_test_settle();
	CHECK_INT(lv_obj_get_child_count(lv_layer_top()), layer);
	ui_splash();
	ui_test_wait(16);
	CHECK_INT(lv_obj_get_child_count(lv_layer_top()), layer);
	CHECK(!black(ui_test_pixel(40, 1000)));
}
