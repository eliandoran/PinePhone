/* Clock (clock/), through the UI: phone-ui on the host (ui_test.h). Its monotonic
 * clock is the tests' (ui_test.c), the wall clock is real. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"

/* Started by ui_test_open(), after the settings a test may change */
TEST_SETUP
{
	ui_test_reset();
	ui_test_write(SETTINGS, "tz=UTC0\n");
}

/* The running digits shown (digits.c: a label per character), "" if none */
static const char *digits(void)
{
	static char buf[16];
	buf[0] = '\0';
	lv_obj_t *sep = ui_test_find(":");
	if (!sep)
		return buf;
	lv_obj_t *row = lv_obj_get_parent(sep);
	for (uint32_t i = 0; i < lv_obj_get_child_count(row) && strlen(buf) < 8; i++)
		strcat(buf, lv_label_get_text(lv_obj_get_child(row, (int32_t)i)));
	return buf;
}

/* The stopwatch's "MM:SS.CC" in ms */
static long digits_ms(const char *text)
{
	unsigned m, s, cs;
	if (sscanf(text, "%u:%u.%u", &m, &s, &cs) != 3)
		return -1;
	return (m * 60 + s) * 1000L + cs * 10;
}

/* The text of a lap row ("Lap 1") and its time, in ms */
static long lap_ms(const char *name)
{
	lv_obj_t *label = ui_test_find(name);
	if (!label)
		return -1;
	lv_obj_t *row = lv_obj_get_parent(label);
	return digits_ms(lv_label_get_text(lv_obj_get_child(row, 1)));
}

/* The tests' time (ui_test.c: phone-ui's monotonic clock), in ms */
static long now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* A tap acts on release, then waits for what it started: times between taps are
 * exact, a time since one is up to its wait longer; the digits change every 30 ms */
static bool near(long ms, long expected)
{
	if (ms >= expected - 50 && ms <= expected + 250)
		return true;
	printf("  %ld ms, expected about %ld\n", ms, expected);
	return false;
}

/* The minutes' (0) or seconds' (1) - or + button of the timer */
static bool adjust(int which, const char *sign)
{
	lv_obj_t *found[2];
	if (ui_test_find_all(sign, found, 2) != 2)
		return false;
	ui_test_tap(found[which]);
	return true;
}

static bool now_shown(time_t t, const char *fmt)
{
	char buf[64];
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, sizeof(buf), fmt, &tm);
	return ui_test_shows(buf);
}

TEST(local_time_and_world_clocks)
{
	time_t before = time(NULL);
	CHECK(ui_test_open("Clock"));
	time_t after = time(NULL);
	CHECK(now_shown(before, "%H:%M") || now_shown(after, "%H:%M"));
	CHECK(now_shown(before, "%A %-d %B %Y") || now_shown(after, "%A %-d %B %Y"));
	CHECK(ui_test_shows("UTC"));
	CHECK(ui_test_shows("World clock"));
	CHECK(ui_test_shows("New York"));
	CHECK(ui_test_shows("London"));
	CHECK(ui_test_shows("Sydney"));
	/* Tokyo has no summer time: 9 hours ahead, today or tomorrow */
	CHECK(ui_test_shows("Tokyo"));
	CHECK(ui_test_shows_part(", +9 h"));
	/* The other pages are hidden */
	CHECK(!ui_test_shows("Lap"));
	CHECK(!ui_test_shows("Minutes"));
}

TEST(world_clocks_from_another_zone)
{
	ui_test_write(SETTINGS, "tz=JST-9\n");
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_shows("JST, UTC+9"));
	CHECK(ui_test_shows("Today, same time"));     /* Tokyo */
	CHECK(ui_test_shows_part(", -14 h") || ui_test_shows_part(", -13 h")); /* New York */
}

TEST(stopwatch_laps)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK_STR(digits(), "00:00.00");
	CHECK(ui_test_shows("Lap"));
	/* Lap does nothing before it ran */
	CHECK(ui_test_tap_text("Lap"));
	CHECK(!ui_test_shows("Lap 1"));

	CHECK(ui_test_tap_text("Start"));
	long start = now();
	CHECK(ui_test_shows("Stop"));
	ui_test_wait(1000);
	CHECK(near(digits_ms(digits()), 1000));
	CHECK(ui_test_tap_text("Lap"));
	long lap = now();
	ui_test_wait(2000);
	CHECK(ui_test_tap_text("Lap"));
	long lap2_end = now();
	CHECK(ui_test_tap_text("Stop"));
	long total = digits_ms(digits());
	CHECK(near(total, now() - start));
	CHECK(total >= 3000);

	/* Each lap's own time, the newest on top */
	CHECK(near(lap_ms("Lap 1"), lap - start));
	CHECK(near(lap_ms("Lap 2"), lap2_end - lap));
	lv_obj_t *lap1 = ui_test_find("Lap 1"), *lap2 = ui_test_find("Lap 2");
	CHECK(lap1 && lap2);
	if (lap1 && lap2) {
		lv_area_t a1, a2;
		lv_obj_get_coords(lap1, &a1);
		lv_obj_get_coords(lap2, &a2);
		CHECK(a2.y1 < a1.y1);
	}

	/* Stopped, it stays; Lap became Reset */
	ui_test_wait(1000);
	CHECK_INT(digits_ms(digits()), total);
	CHECK(ui_test_shows("Start"));
	CHECK(ui_test_tap_text("Reset"));
	CHECK_STR(digits(), "00:00.00");
	CHECK(!ui_test_shows("Lap 1"));
	CHECK(ui_test_shows("Lap"));
}

TEST(stopwatch_resumes)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(ui_test_tap_text("Start"));
	long start = now();
	ui_test_wait(1000);
	CHECK(ui_test_tap_text("Stop"));
	long run = now() - start;
	ui_test_wait(1000); /* not counted */
	CHECK(ui_test_tap_text("Start"));
	start = now();
	ui_test_wait(1000);
	CHECK(near(digits_ms(digits()), run + now() - start));
}

TEST(stopwatch_runs_on_other_pages)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(ui_test_tap_text("Start"));
	long start = now();
	CHECK(ui_test_tap_text("Timer"));
	ui_test_wait(2000);
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(near(digits_ms(digits()), now() - start));
}

TEST(stopwatch_reset_by_leaving)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(500);
	CHECK(ui_test_tap_text("Lap"));
	CHECK(ui_test_back());
	ui_test_wait(500);
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK_STR(digits(), "00:00.00");
	CHECK(ui_test_shows("Start"));
	CHECK(!ui_test_shows("Lap 1"));
}

TEST(timer_set)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(digits(), "05:00");
	CHECK(ui_test_shows("Minutes"));
	CHECK(adjust(0, LV_SYMBOL_PLUS));
	CHECK_STR(digits(), "06:00");
	CHECK(adjust(1, LV_SYMBOL_MINUS));
	CHECK_STR(digits(), "05:59");
	CHECK(ui_test_tap_text("10 min"));
	CHECK_STR(digits(), "10:00");
	CHECK(ui_test_tap_text("1 min"));
	CHECK(adjust(1, LV_SYMBOL_PLUS));
	CHECK_STR(digits(), "01:01");

	/* Not below zero, and zero does not start */
	CHECK(adjust(0, LV_SYMBOL_MINUS));
	CHECK(adjust(0, LV_SYMBOL_MINUS));
	CHECK_STR(digits(), "00:00");
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_shows("Start"));
	CHECK(ui_test_shows("Minutes"));
}

/* The timer set to 3 s */
static bool timer_3s(void)
{
	return ui_test_open("Clock") && ui_test_tap_text("Timer") && ui_test_tap_text("1 min") &&
	       adjust(0, LV_SYMBOL_MINUS) && adjust(1, LV_SYMBOL_PLUS) && adjust(1, LV_SYMBOL_PLUS) &&
	       adjust(1, LV_SYMBOL_PLUS) && !strcmp(digits(), "00:03");
}

TEST(timer_runs_out)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	/* The setup goes, a bar shows what is left */
	CHECK(!ui_test_shows("Minutes"));
	CHECK(ui_test_find_widget(&lv_bar_class));
	ui_test_wait(1100);
	CHECK_STR(digits(), "00:02"); /* rounded up */

	CHECK(ui_test_tap_text("Pause"));
	ui_test_wait(2000);
	CHECK_STR(digits(), "00:02");
	CHECK(ui_test_tap_text("Resume"));
	CHECK(!ui_test_shows_part("Time's up"));
	ui_test_wait(2100);
	CHECK_STR(digits(), "00:00");
	CHECK(ui_test_shows_part("Time's up"));
	CHECK(!ui_test_find_widget(&lv_bar_class)); /* empty, it goes */
	CHECK(ui_test_shows("Start"));
	ui_test_wait(5000); /* the alarm */

	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows_part("Time's up"));
	CHECK(ui_test_shows("Minutes"));
	CHECK(!ui_test_find_widget(&lv_bar_class));
	CHECK_STR(digits(), "00:03");
}

/* The bar drawn on every frame down to empty and after (an LVGL error fails it: the
 * GPU draw unit cannot clip a rounded bar shorter than it is high) */
TEST(timer_bar_to_the_end)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	for (int t = 0; t < 3200 && !ui_test_shows_part("Time's up"); t += 16) {
		lv_obj_invalidate(lv_screen_active());
		ui_test_wait(16);
	}
	CHECK(ui_test_shows_part("Time's up"));
	lv_obj_invalidate(lv_screen_active());
	ui_test_wait(16);
}

TEST(timer_started_again_when_done)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	CHECK(ui_test_shows_part("Time's up"));
	CHECK(ui_test_tap_text("Start"));
	CHECK(!ui_test_shows_part("Time's up"));
	CHECK(ui_test_shows("Pause"));
	CHECK_STR(digits(), "00:03");
}

TEST(timer_cancelled)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(1100);
	CHECK(ui_test_tap_text("Cancel"));
	CHECK_STR(digits(), "00:03");
	CHECK(ui_test_shows("Start"));
	ui_test_wait(3000);
	CHECK(!ui_test_shows_part("Time's up"));
}

/* Leaving while the alarm goes: its timer goes with the screen */
TEST(leaving_during_the_alarm)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	CHECK(ui_test_shows_part("Time's up"));
	CHECK(ui_test_back());
	ui_test_wait(5000);
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(digits(), "05:00");
	CHECK(ui_test_shows("Minutes"));
}

/* In Romanian: the date, the world clocks, the stopwatch and the timer */
TEST(romanian)
{
	ui_test_write(SETTINGS, "tz=UTC0\nlanguage=ro\n");
	ui_test_wall_clock(1773481260); /* Saturday 14 March 2026, 09:41 UTC */
	ui_test_start();
	CHECK(ui_test_open("Ceas"));
	CHECK(ui_test_shows("sâmbătă 14 martie 2026"));
	CHECK(ui_test_shows("Ceas mondial"));
	CHECK(ui_test_shows("Azi, aceeași oră")); /* London */
	CHECK(ui_test_shows("Azi, +9 h")); /* Tokyo: 18:41 */
	CHECK(ui_test_tap_text("Cronometru"));
	CHECK(ui_test_tap_text("Pornește"));
	CHECK(ui_test_tap_text("Tur"));
	CHECK(ui_test_shows("Turul 1"));
	CHECK(ui_test_tap_text("Oprește"));
	CHECK(ui_test_shows("Resetează"));
	CHECK(ui_test_tap_text("Temporizator"));
	CHECK(ui_test_shows("Minute"));
	CHECK(ui_test_shows("Secunde"));
	CHECK(ui_test_shows("Anulează"));
	CHECK(ui_test_tap_text("1 min"));
	CHECK(ui_test_tap_text("Pornește"));
	CHECK(ui_test_shows("Pauză"));
	ui_test_wait(61000);
	CHECK(ui_test_shows_part("Timpul a expirat"));
}
