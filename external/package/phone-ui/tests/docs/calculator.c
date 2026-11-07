/* docs/apps/calculator.md's screenshots (included by test_docs.c) */

/* Taps the keys of a text of digits, ".", "+", "%" */
static bool calculator_keys(const char *keys)
{
	for (const char *k = keys; *k; k++) {
		char text[2] = { *k, '\0' };
		const char *label = *k == '+' ? UI_SYMBOL_PLUS : text;
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
	CHECK(ui_test_tap_text(UI_SYMBOL_EQUALS));
	CHECK(ui_test_shows("48.5 + 7.275 ="));
	CHECK(ui_test_shows("55.775"));
	shot("calculator-tip");
}
