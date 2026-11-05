/* docs/apps/calendar.md's screenshots (included by test_docs.c) */

/* calendar.md: March 2026, today (the 14th) marked, another day selected */
TEST(calendar_month)
{
	CHECK(ui_test_open("Calendar"));
	CHECK(ui_test_shows("March 2026"));
	CHECK(ui_test_shows("Mon"));
	CHECK(ui_test_shows("Sun"));
	CHECK(ui_test_shows("Saturday 14 March 2026"));
	CHECK(ui_test_shows("Today \xc2\xb7 Week 11 \xc2\xb7 Day 73 of 365"));
	CHECK(!ui_test_shows("Today")); /* no button while today is selected */
	/* The grid starts with February's last week: its 27 first */
	lv_obj_t *days[2];
	unsigned n = ui_test_find_all("27", days, 2);
	CHECK_INT(n, 2);
	if (n == 2)
		ui_test_tap(days[1]);
	CHECK(ui_test_shows("Friday 27 March 2026"));
	CHECK(ui_test_shows("In 13 days \xc2\xb7 Week 13 \xc2\xb7 Day 86 of 365"));
	CHECK(ui_test_shows("Today")); /* the button back to it */
	shot("calendar-month");
}
