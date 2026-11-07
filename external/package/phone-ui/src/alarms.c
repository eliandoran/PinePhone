/*
 * Alarms: what an app wants done at a time (Tasks' reminders; Clock's alarm and timer to
 * come), one mechanism for all. An alarm is an app's and a key's (set again, it replaces
 * the one it had), due either at a local date and time (worked out again at each check:
 * a time zone change, DST or the clock set keep 09:00 at 09:00) or after a delay counted
 * on CLOCK_BOOTTIME (asleep too). Once due, its callback runs in the UI loop and it is
 * gone; one in the past runs at the next check. Checked each second while there are any.
 * Not kept: each app sets its alarms again at startup, from its own data.
 *
 * Only while the phone is awake for now: an alarm due while it sleeps runs once it wakes.
 * To wake it, they would arm a timerfd on CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM with
 * the first due, which the kernel's alarmtimer sets in the RTC as it suspends.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

#define CHECK_MS 1000

struct alarm {
	char *app, *key;
	bool local;       /* at tm; else at boot_ms */
	struct tm tm;
	uint64_t boot_ms;
	ui_alarm_fn fire;
	void *data;
};

static struct alarm *alarms;
static unsigned alarm_count;
static lv_timer_t *timer;

static uint64_t boot_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool due(const struct alarm *a)
{
	if (!a->local)
		return boot_ms() >= a->boot_ms;
	struct tm tm = a->tm;
	tm.tm_isdst = -1;
	return mktime(&tm) <= time(NULL);
}

static void remove_at(unsigned i)
{
	free(alarms[i].app);
	free(alarms[i].key);
	alarms[i] = alarms[--alarm_count];
}

static void check_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	/* From the start again after each: a callback may set or cancel alarms */
	for (unsigned i = 0; i < alarm_count;) {
		if (!due(&alarms[i])) {
			i++;
			continue;
		}
		struct alarm a = alarms[i];
		alarms[i].app = alarms[i].key = NULL;
		remove_at(i);
		a.fire(a.key, a.data);
		free(a.app);
		free(a.key);
		i = 0;
	}
	if (!alarm_count)
		lv_timer_pause(timer);
}

static struct alarm *add(const char *app, const char *key, ui_alarm_fn fire, void *data)
{
	ui_alarm_cancel(app, key);
	struct alarm *grown = realloc(alarms, (alarm_count + 1) * sizeof(*alarms));
	if (!grown)
		return NULL;
	alarms = grown;
	struct alarm *a = &alarms[alarm_count++];
	*a = (struct alarm){ .app = strdup(app), .key = strdup(key), .fire = fire, .data = data };
	if (!timer)
		timer = lv_timer_create(check_cb, CHECK_MS, NULL);
	lv_timer_resume(timer);
	return a;
}

void ui_alarm_at(const char *app, const char *key, const struct tm *local, ui_alarm_fn fire, void *data)
{
	struct alarm *a = add(app, key, fire, data);
	if (a) {
		a->local = true;
		a->tm = *local;
	}
}

void ui_alarm_in(const char *app, const char *key, uint32_t ms, ui_alarm_fn fire, void *data)
{
	struct alarm *a = add(app, key, fire, data);
	if (a)
		a->boot_ms = boot_ms() + ms;
}

void ui_alarm_cancel(const char *app, const char *key)
{
	for (unsigned i = 0; i < alarm_count; i++)
		if (!strcmp(alarms[i].app, app) && !strcmp(alarms[i].key, key)) {
			remove_at(i);
			return;
		}
}
