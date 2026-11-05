/* docs/apps/calculator.md's screenshots (included by test_docs.c) */

/* The keypad's keys in order (calculator.c): AC, +/-, %, divide, 7, 8, 9, times,
 * 4, 5, 6, minus, 1, 2, 3, plus, 0, ., equals. Division and equals are drawn, not
 * text: found by their place. */
static bool calculator_key(int index)
{
	lv_obj_t *dot = ui_test_find(".");
	if (!dot)
		return false;
	lv_obj_t *pad = lv_obj_get_parent(lv_obj_get_parent(dot));
	lv_obj_t *key = lv_obj_get_child(pad, index);
	if (!key)
		return false;
	ui_test_tap(key);
	return true;
}

#define CALCULATOR_EQUALS 18

/* Taps the keys of a text of digits, ".", "+", "%" */
static bool calculator_keys(const char *keys)
{
	for (const char *k = keys; *k; k++) {
		char text[2] = { *k, '\0' };
		const char *label = *k == '+' ? LV_SYMBOL_PLUS : text;
		if (!ui_test_tap_text(label))
			return false;
	}
	return true;
}

/* calculator.md: a tip of 15% added to a bill */
TEST(calculator_tip)
{
	CHECK(ui_test_open("Calculator"));
	CHECK(ui_test_shows("AC"));
	CHECK(ui_test_shows("+/-"));
	CHECK(calculator_keys("48.5"));
	CHECK(ui_test_shows("C")); /* clears the entry only */
	CHECK(calculator_keys("+15%"));
	CHECK(ui_test_shows("7.275")); /* 15% of 48.5 */
	CHECK(calculator_key(CALCULATOR_EQUALS));
	CHECK(ui_test_shows("48.5 + 7.275 ="));
	CHECK(ui_test_shows("55.775"));
	shot("calculator-tip");
}
