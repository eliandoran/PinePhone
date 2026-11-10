/* Clock (clock/), through the UI: phone-ui on the host (ui_test.h). Its monotonic
 * clock is the tests' (ui_test.c), the wall clock is real. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include "data/data.h"
#include "test.h"
#include "ui_symbols.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"

/* Started by ui_test_open(), after the settings a test may change */
TEST_SETUP
{
	ui_test_reset();
	ui_test_write(SETTINGS, "tz=UTC0\n");
}

/* The time shown: two wheels' values (clock.c, setting the timer or the alarm), or the
 * running digits (digits.c: a label per character), "" if none */
static const char *digits(void)
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

/* The row `rows` below the middle (above if negative, 2 at most) tapped on a wheel: the
 * timer's minutes (0) or seconds (1), the alarm's hours (0) or minutes (1) */
static lv_obj_t *wheel(int which)
{
	lv_obj_t *wheels[2];
	return ui_test_find_widgets(&lv_roller_class, wheels, 2) == 2 ? wheels[which] : NULL;
}

static int32_t wheel_row(lv_obj_t *w)
{
	return lv_font_get_line_height(lv_obj_get_style_text_font(w, LV_PART_MAIN)) +
	       lv_obj_get_style_text_line_space(w, LV_PART_MAIN);
}

static bool turn(int which, int rows)
{
	lv_obj_t *w = wheel(which);
	if (!w)
		return false;
	lv_area_t a;
	lv_obj_get_coords(w, &a);
	ui_test_tap_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 + rows * wheel_row(w));
	ui_test_wait(500); /* turned */
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

/* The pages from the bar at the bottom, an icon over each one's name */
TEST(pages_from_the_bar_at_the_bottom)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_shows(UI_SYMBOL_CLOCK));
	CHECK(ui_test_shows(UI_SYMBOL_STOPWATCH));
	CHECK(ui_test_shows(UI_SYMBOL_HOURGLASS_HALF));
	CHECK(ui_test_shows(LV_SYMBOL_BELL));
	lv_area_t timer;
	lv_obj_get_coords(ui_test_find("Timer"), &timer);
	CHECK(timer.y1 > 1440 - 128);
	CHECK(ui_test_tap_text("Timer"));
	CHECK(ui_test_shows("Minutes"));
	CHECK(!ui_test_shows("World clock"));
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

/* Clock left, it runs on: opened again, its time and laps */
TEST(stopwatch_runs_after_leaving)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(ui_test_tap_text("Start"));
	long start = now();
	ui_test_wait(500);
	CHECK(ui_test_tap_text("Lap"));
	CHECK(ui_test_back());
	ui_test_wait(1500);
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(near(digits_ms(digits()), now() - start));
	CHECK(ui_test_shows("Stop"));
	CHECK(ui_test_shows("Lap 1"));
	CHECK(near(lap_ms("Lap 1"), 500));
	ui_test_wait(1000); /* still running */
	CHECK(near(digits_ms(digits()), now() - start));

	/* Stopped and reset as before */
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_test_tap_text("Reset"));
	CHECK_STR(digits(), "00:00.00");
	CHECK(!ui_test_shows("Lap 1"));
}

/* Asleep, it counts on (CLOCK_BOOTTIME) */
TEST(stopwatch_counts_asleep)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(ui_test_tap_text("Start"));
	long start = now();
	ui_test_wait(1000);
	ui_test_suspended(5 * 60000);
	ui_test_wait(100);
	CHECK(near(digits_ms(digits()), now() - start + 5 * 60000));
}

TEST(timer_set)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(digits(), "05:00");
	CHECK(ui_test_shows("Minutes"));
	CHECK(turn(0, 1));
	CHECK_STR(digits(), "06:00");
	CHECK(turn(1, -1));
	CHECK_STR(digits(), "06:59"); /* each wheel on its own */
	CHECK(ui_test_tap_text("10 min"));
	CHECK_STR(digits(), "10:00");
	CHECK(ui_test_tap_text("1 min"));
	CHECK(turn(1, 1));
	CHECK_STR(digits(), "01:01");

	/* Round: 00 minutes, then 99 */
	CHECK(turn(0, -1));
	CHECK_STR(digits(), "00:01");
	CHECK(turn(0, -1));
	CHECK_STR(digits(), "99:01");
	CHECK(turn(0, 1));
	/* Zero does not start */
	CHECK(turn(1, -1));
	CHECK_STR(digits(), "00:00");
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_shows("Start"));
	CHECK(ui_test_shows("Minutes"));
}

/* A wheel dragged up turns to later values, the timer set from it */
TEST(timer_wheel_dragged)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(digits(), "05:00");
	lv_obj_t *w = wheel(1);
	CHECK(w);
	if (!w)
		return;
	lv_area_t a;
	lv_obj_get_coords(w, &a);
	int32_t x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
	ui_test_swipe(x, y + wheel_row(w), x, y - 2 * wheel_row(w), 1500); /* slowly: no throw */
	ui_test_wait(1000);
	CHECK_STR(digits(), "05:03");
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_shows("Pause"));
	CHECK_STR(digits(), "05:03");
}

/* Flicked, a wheel turns on a little after the finger, not through every value */
TEST(timer_wheel_flicked)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	lv_obj_t *w = wheel(1);
	CHECK(w);
	if (!w)
		return;
	lv_area_t a;
	lv_obj_get_coords(w, &a);
	int32_t x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
	ui_test_swipe(x, y + wheel_row(w), x, y - 2 * wheel_row(w), 150);
	ui_test_wait(1500);
	unsigned s = (unsigned)lv_roller_get_selected(w);
	printf("  flicked 3 rows: %u\n", s);
	CHECK(s >= 3 && s <= 7);

	/* Fast, 4 rows in 3 frames, it goes on further than a slow drag would, 8 rows at most */
	ui_test_swipe(x, y + 2 * wheel_row(w), x, y - 2 * wheel_row(w), 48);
	ui_test_wait(1500);
	unsigned t = (unsigned)lv_roller_get_selected(w);
	printf("  swiped 4 rows fast: %u more\n", (t + 60 - s) % 60);
	CHECK((t + 60 - s) % 60 >= 6 && (t + 60 - s) % 60 <= 12);
}

/* Long swipes one after the other, each starting while the last still turns: the wheel
 * goes round with the finger (LVGL's roller repeats its values three times and only
 * moves back to the middle copy once it stops turning: it ran off its end, and jumped
 * back the other way) */
TEST(alarm_wheel_swiped_again_and_again)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Alarm"));
	lv_obj_t *w = wheel(0);
	CHECK(w);
	if (!w)
		return;
	lv_area_t a;
	lv_obj_get_coords(w, &a);
	int32_t x = (a.x1 + a.x2) / 2, top = 300, rows = 15;
	for (int i = 0; i < 3; i++) {
		lv_test_mouse_move_to(x, top);
		lv_test_mouse_press();
		ui_test_wait(16);
		for (int f = 1; f <= 60; f++) {
			lv_test_mouse_move_to(x, top + rows * wheel_row(w) * f / 60);
			ui_test_wait(16);
		}
		lv_test_mouse_release();
		ui_test_wait(48); /* still turning */
	}
	ui_test_wait(1500);
	unsigned h = (unsigned)lv_roller_get_selected(w);
	printf("  07 turned down 3 x 15: %02u\n", h);
	CHECK(h >= 4 && h <= 10); /* (7 - 45) mod 24 is 10, the throws 2 at most each */
}

/* The timer set to 3 s */
static bool timer_3s(void)
{
	return ui_test_open("Clock") && ui_test_tap_text("Timer") && ui_test_tap_text("1 min") &&
	       turn(0, -1) && turn(1, 2) && turn(1, 1) && !strcmp(digits(), "00:03");
}

/* Its alert (alert.c), over whatever shows: rings until stopped */
static bool alert_shown(void)
{
	return ui_test_shows("Time's up") && ui_test_shows("Stop") && ui_test_shows("+1 min");
}

static uint64_t boot_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
	CHECK(!alert_shown());
	ui_test_wait(2100);
	CHECK(alert_shown());
	CHECK(ui_test_shows("Timer"));
	ui_test_wait(5000); /* still ringing */
	CHECK(alert_shown());

	/* Stopped: the timer as it was set, ready again */
	CHECK(ui_test_tap_text("Stop"));
	CHECK(!alert_shown());
	CHECK(ui_test_shows("Minutes"));
	CHECK(!ui_test_find_widget(&lv_bar_class));
	CHECK_STR(digits(), "00:03");
	CHECK(ui_test_shows("Start"));
}

/* The bar drawn on every frame down to empty and after (an LVGL error fails it: the
 * GPU draw unit cannot clip a rounded bar shorter than it is high) */
TEST(timer_bar_to_the_end)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	for (int t = 0; t < 3200 && !alert_shown(); t += 16) {
		lv_obj_invalidate(lv_screen_active());
		ui_test_wait(16);
	}
	CHECK(alert_shown());
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
	CHECK(!alert_shown());
	CHECK(ui_test_wake_boot_ms() == 0); /* nothing to wake for */
}

/* Clock left: it runs on, and rings over what shows then */
TEST(timer_runs_out_elsewhere)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_back());
	CHECK(ui_test_open("Notes"));
	ui_test_wait(3100);
	CHECK(alert_shown());
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_test_shows("No notes"));
	CHECK(ui_test_back());
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK_STR(digits(), "00:03");
	CHECK(ui_test_shows("Minutes"));
}

/* Left running, Clock opened again: still running */
TEST(timer_shown_running_again)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_back());
	ui_test_wait(1100);
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_tap_text("Timer"));
	CHECK(strcmp(digits(), "00:03") < 0); /* counting down */
	CHECK(ui_test_shows("Pause"));
}

/* Asleep it counts, and the phone wakes for it (alarms.c) */
TEST(timer_counts_asleep)
{
	CHECK(timer_3s());
	uint64_t start = boot_ms();
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_wake_boot_ms() >= start + 3000 && ui_test_wake_boot_ms() <= start + 3100);
	ui_test_suspended(5000);
	ui_test_wait(1100);
	CHECK(alert_shown());
}

TEST(timer_a_minute_more)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	CHECK(alert_shown());
	uint64_t at = boot_ms();
	CHECK(ui_test_tap_text("+1 min"));
	CHECK(!alert_shown());
	CHECK(ui_test_shows("Pause"));
	CHECK_STR(digits(), "01:00");
	CHECK(ui_test_wake_boot_ms() >= at + 60000 && ui_test_wake_boot_ms() <= at + 60100);
	ui_test_wait(60100);
	CHECK(alert_shown());
}

/* Power stops it, and does not put the phone to sleep */
TEST(timer_stopped_with_power)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	CHECK(alert_shown());
	ui_test_button_press(KEY_POWER, 100);
	CHECK(!alert_shown());
	CHECK(!ui_locked());
	CHECK(ui_test_shows("Minutes"));
}

/* Over the lock screen, which is there again once it is stopped */
TEST(timer_over_the_lock_screen)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_button_press(KEY_POWER, 100); /* asleep and woken at once here: locked */
	CHECK(ui_locked());
	ui_test_wait(3100);
	CHECK(alert_shown());
	CHECK(ui_test_tap_text("Stop"));
	CHECK(!alert_shown());
	CHECK(ui_locked());
}

/* Not stopped: it gives up after a minute, a notification left */
TEST(timer_unanswered)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	CHECK(alert_shown());
	CHECK(notification_app_count(ui_app_find("Clock")) == 0);
	ui_test_wait(61000);
	CHECK(!alert_shown());
	CHECK(notification_app_count(ui_app_find("Clock")) == 1);
	CHECK(ui_test_wait_for("Minutes", 2000)); /* back on Clock */
}

/* Its notification tapped with Clock open on another page: the timer's page, seen */
TEST(timer_unanswered_opened_from_the_shade)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	ui_test_wait(3100);
	ui_test_wait(61000);
	CHECK(!alert_shown());
	/* The screen went off meanwhile: woken and unlocked */
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(500);
	CHECK(ui_locked());
	ui_test_swipe(360, 1200, 360, 500, 600);
	CHECK(ui_test_tap_text("Stopwatch"));
	CHECK(!ui_test_shows("Minutes"));
	ui_test_tap_at(360, 20); /* the shade */
	CHECK(ui_test_tap_text("Time's up"));
	CHECK(ui_test_wait_for("Minutes", 2000));
	CHECK(notification_app_count(ui_app_find("Clock")) == 0);
}

/* Swiped home from it: stopped as with its button */
TEST(alert_swiped_home)
{
	CHECK(timer_3s());
	CHECK(ui_test_tap_text("Start"));
	CHECK(ui_test_back());
	ui_test_wait(3100);
	CHECK(alert_shown());
	ui_test_swipe(360, 1436, 360, 900, 250);
	CHECK(!alert_shown());
	CHECK(ui_at_home());
	ui_test_wait(61000);
	CHECK(notification_app_count(ui_app_find("Clock")) == 0); /* stopped, not missed */
}

/* The alarm (clock/alarm.c) */

#define ALARMS "/var/lib/clock/alarms.json"
#define SAT_0659 1773471540 /* Saturday 14 March 2026, 06:59 UTC: the alarm's 07:00 a minute away */

static bool alarm_page(void)
{
	ui_test_wall_clock(SAT_0659);
	return ui_test_open("Clock") && ui_test_tap_text("Alarm");
}

/* Its alert, over whatever shows */
static bool alarm_rings(void)
{
	return ui_test_shows("07:00") && ui_test_shows("Snooze") && ui_test_shows("Stop");
}

static bool saved(const char *part)
{
	char buf[1024] = "";
	FILE *f = fopen(ALARMS, "r");
	if (f) {
		buf[fread(buf, 1, sizeof(buf) - 1, f)] = '\0';
		fclose(f);
	}
	if (strstr(buf, part))
		return true;
	printf("  " ALARMS ": %s\n", buf);
	return false;
}

TEST(alarm_set)
{
	CHECK(alarm_page());
	CHECK_STR(digits(), "07:00"); /* the default */
	CHECK(ui_test_shows("Off"));
	CHECK(ui_test_shows("Hours"));
	CHECK(ui_test_shows("Minutes"));
	CHECK(ui_test_shows("Turn on"));
	CHECK(turn(0, 1));
	CHECK_STR(digits(), "08:00");
	CHECK(turn(1, -1));
	CHECK_STR(digits(), "08:59"); /* each wheel on its own */
	/* Round the clock: 00, then 23 */
	for (int i = 0; i < 9; i++)
		CHECK(turn(0, -1));
	CHECK_STR(digits(), "23:59");
	CHECK(turn(1, 1));
	CHECK_STR(digits(), "23:00");
	CHECK(turn(0, 2));
	CHECK_STR(digits(), "01:00");
	CHECK(ui_test_shows("Off")); /* setting it does not turn it on */
}

TEST(alarm_rings)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_shows("Rings in 1 min"));
	CHECK(ui_test_shows("Turn off"));
	CHECK(ui_test_wake_at() == SAT_0659 + 60); /* the phone wakes for it */
	CHECK(saved("\"time\": \"07:00\""));
	CHECK(saved("\"on\": true"));
	ui_test_wait(30000);
	CHECK(!alarm_rings());
	ui_test_wait(31000);
	CHECK(alarm_rings());
	CHECK(ui_test_shows("Alarm"));

	/* Once: off after ringing */
	CHECK(ui_test_tap_text("Stop"));
	CHECK(!alarm_rings());
	CHECK(ui_test_shows("Off"));
	CHECK(ui_test_shows("Turn on"));
	CHECK(saved("\"on\": false"));
	CHECK(ui_test_wake_at() == 0);
}

TEST(alarm_turned_off)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_tap_text("Turn off"));
	CHECK(ui_test_shows("Off"));
	CHECK(saved("\"on\": false"));
	ui_test_wait(61000);
	CHECK(!alarm_rings());
}

/* Changed while on: it rings at the new time */
TEST(alarm_changed_while_on)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(turn(1, 1));
	CHECK(ui_test_shows("Rings in 2 min"));
	CHECK(ui_test_wake_at() == SAT_0659 + 120);
	CHECK(ui_test_back()); /* saved on leaving */
	CHECK(saved("\"time\": \"07:01\""));
	ui_test_wait(61000);
	CHECK(!ui_test_shows("Snooze"));
	ui_test_wait(60000);
	CHECK(ui_test_shows("07:01") && ui_test_shows("Snooze"));
}

/* Asleep and locked: the phone wakes, it rings over the lock screen */
TEST(alarm_wakes_the_phone)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_back());
	ui_test_button_press(KEY_POWER, 100);
	CHECK(ui_locked());
	ui_test_suspended(60500);
	ui_test_wait(1100);
	CHECK(alarm_rings());
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_locked());
}

TEST(alarm_snoozed)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	ui_test_wait(61000);
	CHECK(alarm_rings());
	CHECK(ui_test_tap_text("Snooze"));
	CHECK(!alarm_rings());
	CHECK(ui_test_shows("Snoozed until 07:10"));
	ui_test_wait(10 * 60000 - 2000);
	CHECK(!alarm_rings());
	ui_test_wait(3000);
	CHECK(alarm_rings());
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_test_shows("Off"));
}

/* Turned off while snoozed: it does not ring again */
TEST(alarm_snooze_turned_off)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	ui_test_wait(61000);
	CHECK(ui_test_tap_text("Snooze"));
	CHECK(ui_test_shows("Turn off")); /* snoozed: still on */
	CHECK(ui_test_tap_text("Turn off"));
	CHECK(ui_test_shows("Off"));
	ui_test_wait(11 * 60000);
	CHECK(!alarm_rings());
}

/* On chosen days: it rings on each, and stays on */
TEST(alarm_repeats)
{
	CHECK(alarm_page());
	CHECK(ui_test_shows("Repeat"));
	CHECK(ui_test_tap_text("Mon"));
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_shows("Rings on Monday"));
	CHECK(ui_test_wake_at() == SAT_0659 + 60 + 2 * 86400);
	CHECK(ui_test_tap_text("Sat"));
	CHECK(ui_test_shows("Rings in 1 min"));
	CHECK(saved("\"days\": [1, 6]"));
	ui_test_wait(61000);
	CHECK(alarm_rings());
	CHECK(ui_test_tap_text("Stop"));
	CHECK(ui_test_shows("Turn off"));
	CHECK(ui_test_shows("Rings on Monday"));
	CHECK(ui_test_wake_at() == SAT_0659 + 60 + 2 * 86400);
	CHECK(saved("\"on\": true"));
}

/* Kept: set again as phone-ui starts, without opening Clock */
TEST(alarm_kept)
{
	ui_test_write(ALARMS, "{\"alarms\": [{\"id\": 1, \"name\": \"Wake up\", \"time\": \"07:00\", \"on\": true}]}\n");
	ui_test_wall_clock(SAT_0659);
	ui_test_start();
	CHECK(ui_test_wake_at() == SAT_0659 + 60);
	ui_test_wait(61000);
	CHECK(alarm_rings());
	CHECK(ui_test_shows("Wake up")); /* its name */
}

/* Not stopped: it gives up after a minute, a notification left */
TEST(alarm_unanswered)
{
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(ui_test_back());
	ui_test_wait(61000);
	CHECK(alarm_rings());
	ui_test_wait(61000);
	CHECK(!alarm_rings());
	CHECK(notification_app_count(ui_app_find("Clock")) == 1);
}

/* Its missed notification names it; tapped in the shade, the alarm's page */
TEST(alarm_unanswered_named)
{
	ui_test_write(ALARMS, "{\"alarms\": [{\"id\": 1, \"name\": \"Wake up\", \"time\": \"07:00\", \"on\": true}]}\n");
	ui_test_wall_clock(SAT_0659);
	ui_test_start();
	ui_test_wait(61000);
	CHECK(alarm_rings());
	ui_test_wait(61000);
	CHECK(!alarm_rings());
	ui_test_tap_at(360, 20); /* the shade */
	CHECK(ui_test_tap_text("Wake up, 07:00"));
	CHECK(ui_test_shows("Repeat"));
	CHECK(ui_test_shows("Turn on"));
	CHECK(notification_app_count(ui_app_find("Clock")) == 0);
}

/* Not a list of alarms: as none */
TEST(alarm_file_without_alarms)
{
	ui_test_write(ALARMS, "{\"version\": 1}\n");
	CHECK(alarm_page());
	CHECK_STR(digits(), "07:00");
	CHECK(ui_test_shows("Off"));
}

/* Alarms without an id or a time of day left out, days not of the week ignored */
TEST(alarm_file_with_wrong_entries)
{
	ui_test_write(ALARMS, "{\"alarms\": [{\"id\": 0, \"time\": \"05:00\", \"on\": true},"
	                      "{\"id\": 2, \"time\": \"25:00\", \"on\": true},"
	                      "{\"id\": 3, \"time\": \"06:30\", \"days\": [1, 9, -1], \"on\": true}]}\n");
	CHECK(alarm_page());
	CHECK_STR(digits(), "06:30");
	CHECK(ui_test_shows("Rings on Monday"));
}

/* Nowhere to keep it: on all the same, until phone-ui ends */
TEST(alarm_not_saved)
{
	ui_test_write("/var/lib/clock", "not a folder");
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	ui_test_wait(61000);
	CHECK(alarm_rings());
}

TEST(alarm_not_saved_over_a_folder)
{
	mkdir("/var/lib/clock", 0755);
	mkdir(ALARMS, 0755);
	CHECK(alarm_page());
	CHECK(ui_test_tap_text("Turn on"));
	CHECK(access(ALARMS ".new", F_OK) != 0);
	ui_test_wait(61000);
	CHECK(alarm_rings());
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
	CHECK(ui_test_tap_text("Alarmă"));
	CHECK(ui_test_shows("Ore"));
	CHECK(ui_test_shows("Repetă"));
	CHECK(ui_test_shows("Oprită"));
	CHECK(ui_test_shows("sâm."));
	CHECK(ui_test_tap_text("Pornește"));
	CHECK(ui_test_shows("Sună peste 21 h 19 min")); /* 07:00 from 09:41 */
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
	CHECK(ui_test_shows("Oprește"));
}

/* The local time turns with the minute, not up to a second after it */
TEST(clock_turns_with_the_minute)
{
	CHECK(ui_test_open("Clock"));
	CHECK(ui_test_clock_turns(ui_test_shows));
}
