/*
 * calculator.c, included for its state. Keys are pressed through their own
 * event callbacks, on the screen calculator_open() builds (LVGL without a display
 * driver: nothing is drawn).
 */

#include "test.h"

#include "../src/calculator.c"

TEST_SETUP
{
	lv_init();
	lv_display_create(720, 1440);
	calculator_open();
}

/* The key whose callback is key_cb for `key`, under obj */
static lv_obj_t *find_key(lv_obj_t *obj, char key)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
		lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, i);
		if (lv_event_dsc_get_cb(dsc) == key_cb &&
		    (char)(uintptr_t)lv_event_dsc_get_user_data(dsc) == key)
			return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_key(lv_obj_get_child(obj, (int32_t)i), key);
		if (found)
			return found;
	}
	return NULL;
}

/* Clicks the keys of `keys`: digits, ".", "+-* /", "=", "C" (AC), "n" (+/-), "%" */
static void press(const char *keys)
{
	for (; *keys; keys++) {
		lv_obj_t *key = find_key(lv_screen_active(), *keys);
		if (!key) {
			TEST_FAIL("no key '%c'", *keys);
			return;
		}
		lv_obj_send_event(key, LV_EVENT_CLICKED, NULL);
	}
}

static const char *shown(void)
{
	return lv_label_get_text(calc.result);
}

static const char *expression(void)
{
	return lv_label_get_text(calc.expr);
}

TEST(starts_at_zero)
{
	CHECK_STR(shown(), "0");
	CHECK_STR(expression(), "");
	CHECK_STR(lv_label_get_text(calc.clear_label), "AC");
}

TEST(every_key_exists)
{
	for (const char *k = "0123456789.+-*/=Cn%"; *k; k++)
		if (!find_key(lv_screen_active(), *k))
			TEST_FAIL("no key '%c'", *k);
}

TEST(addition)
{
	press("12+3");
	CHECK_STR(shown(), "3");
	CHECK_STR(expression(), "12 +");
	press("=");
	CHECK_STR(shown(), "15");
	CHECK_STR(expression(), "12 + 3 =");
}

TEST(left_to_right)
{
	press("2+3*4=");
	CHECK_STR(shown(), "20");
	CHECK_STR(expression(), "5 x 4 =");
}

TEST(operator_shows_the_running_result)
{
	press("2+3*");
	CHECK_STR(shown(), "5");
	CHECK_STR(expression(), "5 x");
}

TEST(operator_replaced)
{
	press("8+-2=");
	CHECK_STR(shown(), "6");
}

TEST(equals_repeats)
{
	press("2+3===");
	CHECK_STR(shown(), "11");
	CHECK_STR(expression(), "8 + 3 =");
}

TEST(new_calculation_after_equals)
{
	press("2+3=7");
	CHECK_STR(shown(), "7");
	CHECK_STR(expression(), "");
	press("=");
	CHECK_STR(shown(), "7"); /* the + 3 is forgotten */
}

TEST(result_continues)
{
	press("2+3=*2=");
	CHECK_STR(shown(), "10");
}

TEST(decimals)
{
	press("0.1+0.2=");
	CHECK_STR(shown(), "0.3");
	press("C1..5");
	CHECK_STR(shown(), "1.5");
	press(".");
	CHECK_STR(shown(), "1.5");
}

TEST(leading_point)
{
	press(".5*2=");
	CHECK_STR(shown(), "1");
}

TEST(thousands_separators)
{
	press("1234567");
	CHECK_STR(shown(), "1,234,567");
	press(".25");
	CHECK_STR(shown(), "1,234,567.25");
	press("*1000=");
	CHECK_STR(shown(), "1,234,567,250");
	press("C");
	press("123");
	CHECK_STR(shown(), "123");
}

TEST(max_digits)
{
	press("1111111111111111");
	CHECK_STR(shown(), "111,111,111,111");
}

TEST(big_results_in_exponent_form)
{
	press("99999999999*99999999999=");
	CHECK_STR(shown(), "1e+22");
}

TEST(division)
{
	press("10/4=");
	CHECK_STR(shown(), "2.5");
	press("C1/3=");
	CHECK_STR(shown(), "0.3333333333");
}

TEST(division_by_zero)
{
	press("7/0=");
	CHECK_STR(shown(), "Error");
	CHECK_STR(expression(), "");
	press("=n%");
	CHECK_STR(shown(), "Error");
	press("4");
	CHECK_STR(shown(), "4");
	/* An operator starts from 0 */
	press("1/0=+2=");
	CHECK_STR(shown(), "2");
}

TEST(negate)
{
	press("5n");
	CHECK_STR(shown(), "-5");
	press("n");
	CHECK_STR(shown(), "5");
	press("C1234n");
	CHECK_STR(shown(), "-1,234");
	press("+=");
	CHECK_STR(shown(), "-2,468");
	press("n");
	CHECK_STR(shown(), "2,468");
}

TEST(negative_zero_result)
{
	press("0-0=n");
	CHECK_STR(shown(), "0");
}

TEST(percent)
{
	press("50%");
	CHECK_STR(shown(), "0.5");
	/* Of the left operand when adding or subtracting */
	press("C100+5%");
	CHECK_STR(shown(), "5");
	press("=");
	CHECK_STR(shown(), "105");
	press("C200-10%=");
	CHECK_STR(shown(), "180");
	press("C200*10%=");
	CHECK_STR(shown(), "20");
}

TEST(clear)
{
	press("12+34");
	CHECK_STR(lv_label_get_text(calc.clear_label), "C");
	/* C: only the entry */
	press("C");
	CHECK_STR(shown(), "0");
	CHECK_STR(lv_label_get_text(calc.clear_label), "AC");
	press("5=");
	CHECK_STR(shown(), "17");
	/* AC: everything */
	press("C");
	CHECK_STR(shown(), "0");
	CHECK_STR(expression(), "");
	press("=");
	CHECK_STR(shown(), "0");
}

TEST(operator_highlighted)
{
	press("2+");
	lv_obj_t *plus = find_key(lv_screen_active(), '+');
	CHECK_INT(lv_color_to_u32(lv_obj_get_style_bg_color(plus, 0)) & 0xffffff, 0xffffff);
	press("3");
	CHECK_INT(lv_color_to_u32(lv_obj_get_style_bg_color(plus, 0)) & 0xffffff, COLOR_OP);
}
