/*
 * Clock: the alarm, at a time of day, once or on chosen days of the week. Kept in
 * /var/lib/clock/alarms.json as a list, each alarm with an id (its key in alarms.c and
 * notify.c) and a name ("Alarm" without one): one for now, the page's, without a way yet
 * to add others or to name it. Set again as phone-ui starts (clock_start()).
 * At its time an alert (alert.c) rings as a call, the phone woken for it (alarms.c, the
 * RTC): Stop, or Snooze to ring again SNOOZE_MIN minutes later. An alarm without days turns
 * off as it rings. Not stopped, a notification stays.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "../json.h"

#define ALARMS "/var/lib/clock"
#define ALARMS_FILE ALARMS "/alarms.json"
#define MAX_FILE (64 * 1024)
#define APP "Clock" /* NON-NLS: its name in ui_apps */
#define DEFAULT_MINUTE (7 * 60)
#define SNOOZE_MIN 10

struct alarm {
	unsigned id;
	char *name;     /* NULL: "Alarm" */
	int minute;     /* of the day */
	unsigned days;  /* a bit per tm_wday (0: Sunday); none: once */
	bool on;
	time_t snoozed; /* rings again then; 0: not snoozed */
};

static struct alarm *alarms;
static unsigned alarm_count;
static bool loaded;

/* Its page, while Clock is open: the first alarm's */
static struct {
	lv_obj_t *wheels[2], *status, *toggle, *days[7];
	lv_timer_t *timer;
	bool changed; /* its time, saved as the page goes */
} ap;

static struct alarm *alarm_add(unsigned id, int minute)
{
	alarms = realloc(alarms, (alarm_count + 1) * sizeof(*alarms));
	struct alarm *a = &alarms[alarm_count++];
	*a = (struct alarm){ .id = id, .minute = minute };
	return a;
}

/* The page's: the first, 07:00 and off until there is one */
static struct alarm *alarm_first(void)
{
	if (!alarm_count)
		alarm_add(1, DEFAULT_MINUTE);
	return &alarms[0];
}

/* By its key in alarms.c or notify.c, "alarm-<id>" or "snooze-<id>" */
static struct alarm *alarm_by_key(const char *key)
{
	const char *dash = strchr(key, '-');
	unsigned id = dash ? (unsigned)strtoul(dash + 1, NULL, 10) : 0;
	for (unsigned i = 0; i < alarm_count; i++)
		if (alarms[i].id == id)
			return &alarms[i];
	return NULL;
}

static void key_of(char *buf, size_t size, const char *kind, const struct alarm *a)
{
	snprintf(buf, size, "%s-%u", kind, a->id);
}

static void alarms_load(void)
{
	if (loaded)
		return;
	loaded = true;

	FILE *f = fopen(ALARMS_FILE, "r");
	if (!f)
		return;
	char *text = malloc(MAX_FILE + 1);
	size_t n = fread(text, 1, MAX_FILE, f);
	fclose(f);
	text[n] = '\0';

	struct json *root = json_parse(text);
	free(text);
	struct json *list = json_get(root, "alarms");
	if (!list || list->kind != JSON_ARRAY) {
		printf("phone-ui: clock: " ALARMS_FILE " has no list of alarms, ignored\n");
	} else {
		for (struct json *v = list->child; v; v = v->next) {
			int h, m;
			unsigned id = (unsigned)json_int(json_get(v, "id"), 0);
			if (!id || sscanf(json_str(json_get(v, "time"), ""), "%2d:%2d", &h, &m) != 2 || h < 0 || h > 23 ||
			    m < 0 || m > 59)
				continue;
			struct alarm *a = alarm_add(id, h * 60 + m);
			const char *name = json_str(json_get(v, "name"), "");
			a->name = *name ? strdup(name) : NULL;
			struct json *days = json_get(v, "days");
			for (struct json *d = days ? days->child : NULL; d; d = d->next)
				if (json_int(d, -1) >= 0 && json_int(d, -1) < 7)
					a->days |= 1u << json_int(d, 0);
			a->on = json_bool(json_get(v, "on"));
			a->snoozed = (time_t)json_int(json_get(v, "snoozed"), 0);
		}
	}
	json_free(root);
}

/* Through a temporary file, synced: the battery can cut power at any time */
static void alarms_save(void)
{
	mkdir(ALARMS, 0755);
	FILE *f = fopen(ALARMS_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: clock");
		return;
	}
	fputs("{\"alarms\": [", f);
	for (unsigned i = 0; i < alarm_count; i++) {
		const struct alarm *a = &alarms[i];
		fprintf(f, "%s\n\t{\"id\": %u", i ? "," : "", a->id);
		if (a->name) {
			size_t size = 6 * strlen(a->name) + 1;
			char *name = malloc(size);
			json_escape(name, size, a->name);
			fprintf(f, ", \"name\": \"%s\"", name);
			free(name);
		}
		fprintf(f, ", \"time\": \"%02d:%02d\", \"days\": [", a->minute / 60, a->minute % 60);
		for (int d = 0, n = 0; d < 7; d++)
			if (a->days & 1u << d)
				fprintf(f, "%s%d", n++ ? ", " : "", d);
		fprintf(f, "], \"on\": %s", a->on ? "true" : "false");
		if (a->snoozed)
			fprintf(f, ", \"snoozed\": %lld", (long long)a->snoozed);
		fputs("}", f);
	}
	fputs("\n]}\n", f);
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(ALARMS_FILE ".new", ALARMS_FILE) != 0) {
		perror("phone-ui: clock");
		unlink(ALARMS_FILE ".new");
	}
}

/* When it rings next after `now` (0: never), and that as a local time for alarms.c */
static time_t next_ring(const struct alarm *a, time_t now, struct tm *tm)
{
	struct tm today;
	localtime_r(&now, &today);
	for (int d = 0; d <= 7; d++) {
		struct tm t = today;
		t.tm_mday += d;
		t.tm_hour = a->minute / 60;
		t.tm_min = a->minute % 60;
		t.tm_sec = 0;
		t.tm_isdst = -1;
		time_t when = mktime(&t); /* normalized: its tm_wday */
		if (when > now && (!a->days || a->days & 1u << t.tm_wday)) {
			t.tm_isdst = -1;
			*tm = t;
			return when;
		}
	}
	return 0;
}

static void ring(const char *key, void *data);

/* Its ring and its snooze set in alarms.c, as it is now */
static void arm(const struct alarm *a)
{
	char key[24];
	struct tm tm;
	key_of(key, sizeof(key), "alarm", a);
	ui_alarm_cancel(APP, key);
	if (a->on && next_ring(a, time(NULL), &tm))
		ui_alarm_at(APP, key, &tm, ring, NULL);
	key_of(key, sizeof(key), "snooze", a);
	ui_alarm_cancel(APP, key);
	if (a->snoozed) {
		localtime_r(&a->snoozed, &tm);
		tm.tm_isdst = -1;
		ui_alarm_at(APP, key, &tm, ring, NULL);
	}
}

static void page_update(void);

/* Tapped: the alarm's page; seen, the notification goes */
static void notification_open(const char *key)
{
	ui_notify(APP, key, NULL, NULL, 0, 0, false, NULL);
	clock_open_page(CLOCK_ALARM);
}

static const char *time_text(const struct alarm *a)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%02d:%02d", a->minute / 60 % 24, a->minute % 60);
	return ui_intern(buf);
}

static void alert_done(enum ui_alert_end how, void *data)
{
	unsigned id = (unsigned)(uintptr_t)data;
	struct alarm *a = NULL;
	for (unsigned i = 0; i < alarm_count; i++)
		if (alarms[i].id == id)
			a = &alarms[i];
	if (!a)
		return;
	if (how == UI_ALERT_OTHER) {
		a->snoozed = time(NULL) + SNOOZE_MIN * 60;
		arm(a);
		alarms_save();
	} else if (how == UI_ALERT_MISSED) {
		char key[24], text[160];
		key_of(key, sizeof(key), "alarm", a);
		if (a->name)
			snprintf(text, sizeof(text), "%s, %s", a->name, time_text(a));
		else
			snprintf(text, sizeof(text), "%s", time_text(a));
		ui_notify(APP, key, _("Missed alarm"), text, 1, time(NULL), false, notification_open);
		printf("phone-ui: clock: alarm %u not stopped\n", a->id);
	}
	page_update();
}

/* Its time (alarms.c), or its snooze's: it rings */
static void ring(const char *key, void *data)
{
	LV_UNUSED(data);
	struct alarm *a = alarm_by_key(key);
	if (!a)
		return;
	if (!strncmp(key, "snooze", 6))
		a->snoozed = 0;
	else if (!a->days)
		a->on = false;
	arm(a); /* the next day it rings on */
	alarms_save();
	page_update();
	ui_alert(LV_SYMBOL_BELL, a->name ? ui_intern(a->name) : _("Alarm"), time_text(a), _("Snooze"), alert_done,
	         (void *)(uintptr_t)a->id);
}

void clock_start(void)
{
	alarms_load();
	for (unsigned i = 0; i < alarm_count; i++)
		arm(&alarms[i]);
}

/* The page */

static void status_update(const struct alarm *a)
{
	char hm[8];
	struct tm tm;
	time_t now = time(NULL);
	if (a->snoozed) {
		localtime_r(&a->snoozed, &tm);
		strftime(hm, sizeof(hm), "%H:%M", &tm);
		ui_set_text_fmt(ap.status, _("Snoozed until %s"), hm);
		return;
	}
	time_t when = a->on ? next_ring(a, now, &tm) : 0;
	if (!when) {
		lv_label_set_text_static(ap.status, C_("alarm", "Off"));
		return;
	}
	long min = (long)((when - now + 59) / 60); /* rounded up: "1 min" in its last minute */
	if (min >= 24 * 60)
		ui_set_text_fmt(ap.status, _("Rings on %s"), ui_day_name(tm.tm_wday, false));
	else if (min >= 60)
		ui_set_text_fmt(ap.status, _("Rings in %ld h %ld min"), min / 60, min % 60);
	else
		ui_set_text_fmt(ap.status, _("Rings in %ld min"), min);
}

static void page_update(void)
{
	if (!ap.status)
		return;
	const struct alarm *a = alarm_first();
	clock_wheel_set(ap.wheels[0], (unsigned)a->minute / 60);
	clock_wheel_set(ap.wheels[1], (unsigned)a->minute % 60);
	for (int d = 0; d < 7; d++) {
		bool set = a->days & 1u << d;
		clock_button_set(ap.days[d], lv_label_get_text(lv_obj_get_child(ap.days[d], 0)),
		                 set ? UI_COLOR_ACCENT : UI_COLOR_CARD, set ? UI_COLOR_ACCENT_PRESSED : UI_COLOR_BUTTON_PRESSED);
	}
	if (a->on || a->snoozed)
		clock_button_set(ap.toggle, _("Turn off"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED);
	else
		clock_button_set(ap.toggle, _("Turn on"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED);
	status_update(a);
}

/* Changed by hand: a missed one's notification goes */
static void changed(struct alarm *a)
{
	char key[24];
	key_of(key, sizeof(key), "alarm", a);
	ui_notify(APP, key, NULL, NULL, 0, 0, false, NULL);
	arm(a);
	page_update();
}

static void wheel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	struct alarm *a = alarm_first();
	int minute = (int)(clock_wheel_get(ap.wheels[0]) * 60 + clock_wheel_get(ap.wheels[1]));
	if (minute == a->minute)
		return;
	a->minute = minute;
	ap.changed = true;
	changed(a);
}

static void day_cb(lv_event_t *e)
{
	struct alarm *a = alarm_first();
	a->days ^= 1u << (int)(intptr_t)lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	changed(a);
	alarms_save();
}

static void toggle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	struct alarm *a = alarm_first();
	if (a->on || a->snoozed) {
		a->on = false;
		a->snoozed = 0;
	} else {
		a->on = true;
	}
	ui_vibrate(UI_VIBRATE_TAP);
	changed(a);
	alarms_save();
}

static void tick(lv_timer_t *t)
{
	LV_UNUSED(t);
	status_update(alarm_first());
}

void alarm_page_create(lv_obj_t *page)
{
	alarms_load();

	clock_spacer(page, 40);
	lv_obj_t *setup = clock_setup_create(page);
	clock_wheels_create(setup, _("Hours"), 24, _("Minutes"), 60, wheel_cb, ap.wheels);
	ap.status = ui_dim_label(setup, "", UI_FONT_BODY);
	lv_obj_set_style_margin_top(ap.status, -16, 0);

	/* The days it repeats on, from Monday */
	lv_obj_t *repeat = ui_box(setup);
	lv_obj_set_size(repeat, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(repeat, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(repeat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(repeat, 12, 0);
	ui_dim_label(repeat, _("Repeat"), UI_FONT_CAPTION);
	lv_obj_t *days = clock_button_row(repeat);
	for (int i = 0; i < 7; i++) {
		int d = (i + 1) % 7;
		ap.days[d] = ui_button(days, 84, 84, UI_COLOR_CARD, UI_COLOR_BUTTON_PRESSED, ui_day_name(d, true), day_cb,
		                       (void *)(intptr_t)d);
		lv_obj_set_style_text_font(lv_obj_get_child(ap.days[d], 0), UI_FONT_CAPTION, 0);
	}

	/* Pushes the button to the bottom */
	lv_obj_set_flex_grow(clock_spacer(page, 0), 1);
	lv_obj_t *row = clock_button_row(page);
	ap.toggle = clock_round_button(row, _("Turn on"), UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, toggle_cb);

	ap.timer = lv_timer_create(tick, 1000, NULL);
	page_update();
}

/* The page goes; the alarm stays */
void alarm_page_delete(void)
{
	if (ap.changed)
		alarms_save();
	lv_timer_delete(ap.timer);
	memset(&ap, 0, sizeof(ap));
}
