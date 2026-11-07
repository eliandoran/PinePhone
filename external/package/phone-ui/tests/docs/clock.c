/* docs/apps/clock.md's screenshots (included by test_docs.c) */

#include <string.h>

/* The time shown: two wheels' values (clock/clock.c, setting the timer or the alarm), or
 * the running digits (clock/digits.c: a label per character), "" if none */
static const char *clock_digits(void)
{
	static char buf[16];
	buf[0] = '\0';
	lv_obj_t *wheels[2];
	if (ui_test_find_widgets(&lv_roller_class, wheels, 2) == 2) {
		snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)lv_roller_get_selected(wheels[0]),
		         (unsigned)lv_roller_get_selected(wheels[1]));
		return buf;
	}
	lv_obj_t *sep = ui_test_find(":");
	if (!sep)
		return buf;
	lv_obj_t *row = lv_obj_get_parent(sep);
	for (uint32_t i = 0; i < lv_obj_get_child_count(row) && strlen(buf) < 8; i++)
		strcat(buf, lv_label_get_text(lv_obj_get_child(row, (int32_t)i)));
	return buf;
}

/* clock.md: the local time and the world clocks */
TEST(clock_world)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_shows("09:41"));
	CHECK(ui_test_shows("Saturday 14 March 2026"));
	CHECK(ui_test_shows("UTC"));
	CHECK(ui_test_shows("World clock"));
	CHECK(ui_test_shows("New York"));
	CHECK(ui_test_shows("05:41"));             /* summer time since 8 March */
	CHECK(ui_test_shows("Today, -4 h"));
	CHECK(ui_test_shows("Today, same time")); /* London */
	CHECK(ui_test_shows("Today, +9 h"));      /* Tokyo */
	CHECK(ui_test_shows("Today, +11 h"));     /* Sydney */
	shot("clock-world");
}

/* clock.md: the stopwatch, stopped after three laps */
TEST(clock_stopwatch)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK_STR(clock_digits(), "00:00.00");
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_shows("Stop"));
	ui_test_wait(12300);
	CHECK(ui_test_tap_text("Lap"));
	ui_test_wait(11800);
	CHECK(ui_test_tap_text("Lap"));
	ui_test_wait(12600);
	CHECK(ui_test_tap_text("Lap"));
	ui_test_wait(3200);
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_test_shows("Lap 1"));
	CHECK(ui_test_shows("Lap 3"));
	CHECK(ui_test_shows("Reset")); /* Lap became Reset */
	CHECK(ui_test_shows("Start"));
	CHECK(!strncmp(clock_digits(), "00:3", 4) || !strncmp(clock_digits(), "00:4", 4));
	shot("clock-stopwatch");
}

/* clock.md: the timer set to 3 minutes; then started and paused (not shown) */
TEST(clock_timer)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(clock_digits(), "05:00"); /* the default */
	CHECK(ui_test_shows("Minutes"));
	CHECK(ui_test_shows("Seconds"));
	CHECK(ui_test_shows("1 min"));
	CHECK(ui_test_shows("5 min"));
	CHECK(ui_test_shows("10 min"));
	CHECK(ui_test_tap_text("3 min"));
	CHECK_STR(clock_digits(), "03:00");
	shot("clock-timer");

	CHECK(ui_test_tap_text("Start"));
	CHECK(!ui_test_shows("Minutes")); /* the setup goes while it runs */
	CHECK(ui_test_find_widget(&lv_bar_class));
	ui_test_wait(47000);
	CHECK(ui_test_tap_text("Pause"));
	CHECK(ui_test_shows("Resume"));
	CHECK(ui_test_shows("Cancel"));
	CHECK_STR(clock_digits(), "02:13");
}

/* clock.md: the alarm at 07:00 on weekdays, turned on */
TEST(clock_alarm)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Alarm"));
	CHECK_STR(clock_digits(), "07:00"); /* the default */
	CHECK(ui_test_shows("Off"));
	CHECK(ui_test_shows("Hours"));
	CHECK(ui_test_shows("Repeat"));
	static const char *const days[] = { "Mon", "Tue", "Wed", "Thu", "Fri" };
	for (int i = 0; i < 5; i++)
		CHECK(ui_test_tap_text(days[i]));
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_shows("Rings on Monday")); /* from Saturday */
	CHECK(ui_test_shows("Turn off"));
	shot("clock-alarm");
}
