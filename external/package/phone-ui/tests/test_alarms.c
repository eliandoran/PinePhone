/* Alarms (alarms.c): their callbacks in the UI loop, and the phone woken for them (its
 * wake timers, ui_test_wake_at()) */

#include <stdlib.h>

#include "test.h"
#include "ui_test.h"

#define NOW 1773481260 /* Saturday 14 March 2026, 09:41:00 UTC (TZ=UTC0 in the tests) */

static unsigned fired;
static char last[16];

static void fire(const char *key, void *data)
{
	LV_UNUSED(data);
	fired++;
	snprintf(last, sizeof(last), "%s", key);
}

/* That Saturday at h:m, local */
static struct tm at(int h, int m)
{
	return (struct tm){ .tm_year = 126, .tm_mon = 2, .tm_mday = 14, .tm_hour = h, .tm_min = m, .tm_isdst = -1 };
}

static uint64_t boot_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

TEST_SETUP
{
	ui_test_reset();
	ui_test_wall_clock(NOW);
	ui_test_start();
}

TEST(none_no_wake)
{
	CHECK(ui_test_wake_at() == 0);
	CHECK(ui_test_wake_boot_ms() == 0);
}

TEST(wakes_for_the_first_then_the_next)
{
	struct tm evening = at(18, 0), morning = at(10, 0);
	ui_alarm_at("Test", "evening", &evening, fire, NULL);
	ui_alarm_at("Test", "morning", &morning, fire, NULL);
	CHECK(ui_test_wake_at() == NOW + 19 * 60);
	ui_test_wait(1500);
	CHECK(fired == 0);

	/* The clock at 10:00 (set forward: the same as reached) */
	ui_test_wall_clock(NOW + 19 * 60);
	ui_test_wait(1500);
	CHECK(fired == 1);
	CHECK_STR(last, "morning");
	CHECK(ui_test_wake_at() == NOW + (8 * 60 + 19) * 60);
}

TEST(replaced_by_its_key)
{
	struct tm morning = at(10, 0), noon = at(12, 0);
	ui_alarm_at("Test", "a", &morning, fire, NULL);
	ui_alarm_at("Test", "a", &noon, fire, NULL);
	CHECK(ui_test_wake_at() == NOW + (2 * 60 + 19) * 60);
	ui_test_wall_clock(NOW + 19 * 60);
	ui_test_wait(1500);
	CHECK(fired == 0);
}

TEST(cancelled_no_wake)
{
	struct tm morning = at(10, 0);
	ui_alarm_at("Test", "a", &morning, fire, NULL);
	ui_alarm_cancel("Test", "a");
	CHECK(ui_test_wake_at() == 0);
	ui_test_wall_clock(NOW + 3600);
	ui_test_wait(1500);
	CHECK(fired == 0);
}

TEST(passed_fires_at_once)
{
	struct tm earlier = at(9, 0);
	ui_alarm_at("Test", "a", &earlier, fire, NULL);
	ui_test_wait(1500);
	CHECK(fired == 1);
	CHECK(ui_test_wake_at() == 0);
}

/* A delay counts asleep too (CLOCK_BOOTTIME): a timer */
TEST(delay_counted_asleep)
{
	uint64_t start = boot_ms();
	ui_alarm_in("Test", "timer", 60000, fire, NULL);
	CHECK(ui_test_wake_boot_ms() == start + 60000);
	CHECK(ui_test_wake_at() == 0);
	ui_test_suspended(61000);
	ui_test_wait(1500);
	CHECK(fired == 1);
	CHECK_STR(last, "timer");
	CHECK(ui_test_wake_boot_ms() == 0);
}

/* Another time zone (Settings): 10:00 there, woken then */
TEST(time_zone_changed_wakes_at_its_time)
{
	struct tm morning = at(10, 0);
	ui_alarm_at("Test", "a", &morning, fire, NULL);
	CHECK(ui_test_wake_at() == NOW + 19 * 60);
	setenv("TZ", "UTC-1", 1); /* an hour ahead: 10:00 there is 09:00 here, gone */
	tzset();
	setenv("TZ", "UTC+1", 1); /* an hour behind: 10:00 there is 11:00 here */
	tzset();
	ui_test_wait(1500);
	CHECK(fired == 0);
	CHECK(ui_test_wake_at() == NOW + 19 * 60 + 3600);
}
