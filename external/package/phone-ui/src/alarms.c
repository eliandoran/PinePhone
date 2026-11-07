/*
 * Alarms: what an app wants done at a time (Tasks' reminders; Clock's alarm and timer to
 * come), one mechanism for all. An alarm is an app's and a key's (set again, it replaces
 * the one it had), due either at a local date and time (worked out again at each check:
 * a time zone change, DST or the clock set keep 09:00 at 09:00) or after a delay counted
 * on CLOCK_BOOTTIME (asleep too). Once due, its callback runs in the UI loop and it is
 * gone; one in the past runs at the next check. Checked each second while there are any.
 * Not kept: each app sets its alarms again at startup, from its own data.
 *
 * The phone wakes for them: a timerfd on CLOCK_REALTIME_ALARM is set to the first one at a
 * date and time, one on CLOCK_BOOTTIME_ALARM to the first delay, again whenever they change
 * and at each check (the clock or the time zone changed). As the phone suspends, the
 * kernel's alarmtimer sets the RTC's alarm to the earliest (it refuses to suspend with one
 * due within two seconds); woken, the check runs them, and power.c's lights the screen and
 * sleeps again as for a message. Nothing reads the timerfds: they are only for waking.
 * Setting them needs CAP_WAKE_ALARM (root); without it, the alarms run awake only.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<sys/timerfd.h>)
#include <sys/timerfd.h>
#define WAKE 1 /* not in a browser (wasm/): nothing to wake there */
#endif
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

#ifdef WAKE
/* The wake timers, CLOCK_REALTIME_ALARM's and CLOCK_BOOTTIME_ALARM's: -2 not made yet,
 * -1 cannot be; and when they are set for (0: not) */
static int wake_fds[2] = { -2, -2 };
static struct timespec wake_set[2];
#endif

static uint64_t boot_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static time_t local_time(const struct alarm *a)
{
	struct tm tm = a->tm;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static bool due(const struct alarm *a)
{
	if (!a->local)
		return boot_ms() >= a->boot_ms;
	return local_time(a) <= time(NULL);
}

/* One wake timer set for `when` (absolute; 0: none), if it changed */
static void wake_set_for(int i, struct timespec when)
{
#ifdef WAKE
	static const clockid_t clocks[2] = { CLOCK_REALTIME_ALARM, CLOCK_BOOTTIME_ALARM };
	if (when.tv_sec == wake_set[i].tv_sec && when.tv_nsec == wake_set[i].tv_nsec)
		return;
	if (wake_fds[i] == -2) {
		wake_fds[i] = timerfd_create(clocks[i], TFD_NONBLOCK | TFD_CLOEXEC);
		if (wake_fds[i] < 0)
			printf("phone-ui: alarms: cannot wake the phone for them (%s)\n", strerror(errno));
	}
	if (wake_fds[i] < 0)
		return;
	struct itimerspec it = { .it_value = when };
	if (timerfd_settime(wake_fds[i], TFD_TIMER_ABSTIME, &it, NULL) != 0) {
		printf("phone-ui: alarms: cannot set the wake timer (%s)\n", strerror(errno));
		return;
	}
	wake_set[i] = when;
#else
	LV_UNUSED(i);
	LV_UNUSED(when);
#endif
}

/* The phone woken for the first alarm of each kind */
static void wake_update(void)
{
	time_t first = 0;
	uint64_t first_ms = 0;
	for (unsigned i = 0; i < alarm_count; i++) {
		if (alarms[i].local) {
			time_t t = local_time(&alarms[i]);
			if (!first || t < first)
				first = t;
		} else if (!first_ms || alarms[i].boot_ms < first_ms) {
			first_ms = alarms[i].boot_ms;
		}
	}
	wake_set_for(0, (struct timespec){ .tv_sec = first });
	wake_set_for(1, (struct timespec){ .tv_sec = (time_t)(first_ms / 1000), .tv_nsec = (long)(first_ms % 1000) * 1000000 });
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
	wake_update();
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
	wake_update();
}

void ui_alarm_in(const char *app, const char *key, uint32_t ms, ui_alarm_fn fire, void *data)
{
	struct alarm *a = add(app, key, fire, data);
	if (a)
		a->boot_ms = boot_ms() + ms;
	wake_update();
}

void ui_alarm_cancel(const char *app, const char *key)
{
	for (unsigned i = 0; i < alarm_count; i++)
		if (!strcmp(alarms[i].app, app) && !strcmp(alarms[i].key, key)) {
			remove_at(i);
			wake_update();
			return;
		}
}
