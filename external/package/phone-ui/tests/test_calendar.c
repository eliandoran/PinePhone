/* Calendar (calendar.c), through the UI: phone-ui on the host (ui_test.h), in UTC.
 * Today is the real date. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"

TEST_SETUP
{
	ui_test_reset();
	ui_test_write(SETTINGS, "tz=UTC0\n");
}

static struct tm today(void)
{
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	return tm;
}

/* A date's text in this format */
static const char *date_text(int year, int month, int day, const char *fmt)
{
	static char buf[64];
	struct tm tm = { .tm_year = year - 1900, .tm_mon = month, .tm_mday = day, .tm_hour = 12 };
	time_t t = timegm(&tm);
	gmtime_r(&t, &tm);
	strftime(buf, sizeof(buf), fmt, &tm);
	return buf;
}

/* The month shown in the title, as "November 2026" */
static bool month_shown(int year, int month)
{
	return ui_test_shows(date_text(year, month, 1, "%B %Y"));
}

/* A day of this month that is not today, and not in the next month's first week */
static int other_day(void)
{
	return today().tm_mday == 15 ? 16 : 15;
}

/* The month arrow (LV_SYMBOL_LEFT is the back button's too: the arrow is below) */
static bool arrow(const char *symbol)
{
	lv_obj_t *found[2];
	unsigned n = ui_test_find_all(symbol, found, 2);
	if (!n)
		return false;
	ui_test_tap(found[n - 1]);
	return true;
}

/* Swipes sideways across the grid */
static bool swipe(int dx)
{
	lv_obj_t *day = ui_test_find("15");
	if (!day)
		return false;
	lv_area_t a;
	lv_obj_get_coords(day, &a);
	ui_test_swipe(360 - dx / 2, a.y1, 360 + dx / 2, a.y1, 160);
	return true;
}

TEST(opens_on_today)
{
	struct tm t = today();
	CHECK(ui_test_open("Calendar"));
	CHECK(month_shown(t.tm_year + 1900, t.tm_mon));
	CHECK(ui_test_shows("Mon"));
	CHECK(ui_test_shows("Sun"));
	CHECK(ui_test_shows(date_text(t.tm_year + 1900, t.tm_mon, t.tm_mday, "%A %-d %B %Y")));
	CHECK(ui_test_shows_part("Today \xc2\xb7 Week "));
	/* Today is shown: no button to go back to it */
	CHECK(!ui_test_find_all("Today", NULL, 0));
}

TEST(day_selected)
{
	struct tm t = today();
	int day = other_day(), diff = day - t.tm_mday;
	char rel[32];
	if (diff == 1)
		snprintf(rel, sizeof(rel), "Tomorrow");
	else if (diff == -1)
		snprintf(rel, sizeof(rel), "Yesterday");
	else if (diff > 0)
		snprintf(rel, sizeof(rel), "In %d days", diff);
	else
		snprintf(rel, sizeof(rel), "%d days ago", -diff);

	CHECK(ui_test_open("Calendar"));
	char text[8];
	snprintf(text, sizeof(text), "%d", day);
	CHECK(ui_test_tap_text(text));
	CHECK(ui_test_shows(date_text(t.tm_year + 1900, t.tm_mon, day, "%A %-d %B %Y")));
	CHECK(ui_test_shows_part(rel));

	/* Back to today */
	CHECK(ui_test_tap_text("Today"));
	CHECK(ui_test_shows(date_text(t.tm_year + 1900, t.tm_mon, t.tm_mday, "%A %-d %B %Y")));
	CHECK(ui_test_shows_part("Today \xc2\xb7"));
}

TEST(arrows_change_the_month)
{
	struct tm t = today();
	int y = t.tm_year + 1900, m = t.tm_mon;
	CHECK(ui_test_open("Calendar"));
	CHECK(arrow(LV_SYMBOL_RIGHT));
	CHECK(month_shown(m == 11 ? y + 1 : y, (m + 1) % 12));
	CHECK(arrow(LV_SYMBOL_LEFT));
	CHECK(arrow(LV_SYMBOL_LEFT));
	CHECK(month_shown(m == 0 ? y - 1 : y, (m + 11) % 12));
	/* A year back, across January */
	for (int i = 0; i < 11; i++)
		arrow(LV_SYMBOL_LEFT);
	CHECK(month_shown(y - 1, m));
	/* The selected day stays */
	CHECK(ui_test_shows(date_text(y, m, t.tm_mday, "%A %-d %B %Y")));
}

TEST(swipes_change_the_month)
{
	struct tm t = today();
	int y = t.tm_year + 1900, m = t.tm_mon;
	CHECK(ui_test_open("Calendar"));
	CHECK(swipe(-400));
	CHECK(month_shown(m == 11 ? y + 1 : y, (m + 1) % 12));
	CHECK(swipe(400));
	CHECK(swipe(400));
	CHECK(month_shown(m == 0 ? y - 1 : y, (m + 11) % 12));
	/* No day was tapped by the swipes */
	CHECK(ui_test_shows_part("Today \xc2\xb7"));
}

/* The grid's last days are the next month's: tapping one goes there */
TEST(day_of_the_next_month)
{
	struct tm t = today();
	int y = t.tm_year + 1900, m = t.tm_mon;
	int ny = m == 11 ? y + 1 : y, nm = (m + 1) % 12;
	CHECK(ui_test_open("Calendar"));
	lv_obj_t *ones[2];
	CHECK_INT(ui_test_find_all("1", ones, 2), 2);
	ui_test_tap(ones[1]);
	CHECK(month_shown(ny, nm));
	CHECK(ui_test_shows(date_text(ny, nm, 1, "%A %-d %B %Y")));
}

/* 29 February 2028: a leap year's 60th day, in ISO week 9 */
TEST(leap_day)
{
	struct tm t = today();
	int months = (2028 - (t.tm_year + 1900)) * 12 + (1 - t.tm_mon);
	CHECK(ui_test_open("Calendar"));
	for (int i = 0; i < (months < 0 ? -months : months); i++)
		arrow(months < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
	CHECK(month_shown(2028, 1));
	CHECK(ui_test_tap_text("29"));
	CHECK(ui_test_shows("Tuesday 29 February 2028"));
	CHECK(ui_test_shows_part(" \xc2\xb7 Week 9 \xc2\xb7 Day 60 of 366"));
	CHECK(ui_test_tap_text("28"));
	CHECK(ui_test_shows_part("Day 59 of 366"));
}

/* A year with 53 ISO weeks: 31 December 2026 is a Thursday */
TEST(week_53)
{
	struct tm t = today();
	int months = (2026 - (t.tm_year + 1900)) * 12 + (11 - t.tm_mon);
	CHECK(ui_test_open("Calendar"));
	for (int i = 0; i < (months < 0 ? -months : months); i++)
		arrow(months < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
	CHECK(month_shown(2026, 11));
	CHECK(ui_test_tap_text("31"));
	CHECK(ui_test_shows("Thursday 31 December 2026"));
	CHECK(ui_test_shows_part("Week 53 \xc2\xb7 Day 365 of 365"));
}

/* Its timer (today changes at midnight) goes with the screen */
TEST(left_and_opened_again)
{
	CHECK(ui_test_open("Calendar"));
	CHECK(arrow(LV_SYMBOL_RIGHT));
	ui_test_wait(61000);
	CHECK(ui_test_back());
	ui_test_wait(61000);
	struct tm t = today();
	CHECK(ui_test_open("Calendar"));
	CHECK(month_shown(t.tm_year + 1900, t.tm_mon));
}
