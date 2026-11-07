/*
 * The battery: whether the cell is usable, the time to full while charging, the
 * history of its level and the screen time since unplugged, all sampled by
 * ui_battery_update() from ui_status_update().
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "i18n.h"
#include "ui.h"
#include "usage.h"

/*
 * The AXP803 reports a dead cell as present (and charging, at 0%): a Li-ion cell
 * never runs below ~2.5 V in use, its protection circuit cuts off around there.
 */
bool ui_battery_present(void)
{
	long present, uv;
	if (!ui_read_long(UI_BATTERY "/present", &present))
		present = 1;
	if (!ui_read_long(UI_BATTERY "/voltage_now", &uv))
		uv = -1;
	return present && (uv < 0 || uv >= 2500000);
}

/* Power on the USB input: a charger or a computer */
bool ui_usb_powered(void)
{
	long online;
	return ui_read_long("/sys/class/power_supply/axp20x-usb/online", &online) && online;
}

/*
 * Full on the charger, the AXP803 stops charging and the battery tops up the input with a
 * few mA: its driver then says "Discharging" (and "Full" only without any current). Plugged
 * in without charging is "Full" at 100%, "Not charging" below: it charges again once the
 * voltage has dropped, the level may read 99% meanwhile.
 */
void ui_battery_status(char *buf, size_t size)
{
	long capacity;
	if (!ui_read_line(UI_BATTERY "/status", buf, size))
		buf[0] = '\0';
	if (strcmp(buf, "Charging") == 0 || !ui_usb_powered())
		return;
	snprintf(buf, size, "%s", ui_read_long(UI_BATTERY "/capacity", &capacity) && capacity >= 100 ?
	         "Full" : "Not charging"); /* NON-NLS: the kernel's words, compared */
}

/* The power supply class's words for the status and health, translated */
static const char *const kernel_words[] = {
	N_("Charging"), N_("Discharging"), N_("Full"), N_("Not charging"), N_("Unknown"),
	NC_("battery health", "Good"), NC_("battery health", "Overheat"), NC_("battery health", "Dead"),
	NC_("battery health", "Cold"), NC_("battery health", "Over voltage"),
	NC_("battery health", "Unspecified failure"),
};

const char *ui_battery_word(const char *word)
{
	for (size_t i = 0; i < sizeof(kernel_words) / sizeof(kernel_words[0]); i++) {
		const char *w = strchr(kernel_words[i], '\004');
		if (strcmp(w ? w + 1 : kernel_words[i], word) == 0)
			return _(kernel_words[i]);
	}
	return word;
}

bool ui_battery_plugged(const char *status)
{
	return strcmp(status, "Charging") == 0 || strcmp(status, "Full") == 0 ||
	       strcmp(status, "Not charging") == 0;
}

/*
 * Time to full: the charge missing to 100% of the design capacity at the charging
 * current, averaged over the last minutes (exponentially, sampled by ui_battery_update()):
 * above about 80% the charger holds the voltage and the current falls, which the average
 * follows. Just plugged in, the charger ramps up from a few mA: the average starts once
 * it has charged for CHARGE_SETTLE_MS, with no estimate before (it read days).
 */
#define CHARGE_AVG_MS 120000
#define CHARGE_SETTLE_MS 20000

static double charge_ua = -1; /* the average, -1 while not charging or settling */
static uint32_t charge_tick, charge_start;
static bool charge_on;

static void charge_sample(bool charging)
{
	long ua;
	if (!charging || !ui_read_long(UI_BATTERY "/current_now", &ua)) {
		charge_ua = -1;
		charge_on = false;
		return;
	}
	uint32_t now = lv_tick_get();
	if (!charge_on) {
		charge_on = true;
		charge_start = now;
	}
	if (lv_tick_diff(now, charge_start) < CHARGE_SETTLE_MS)
		charge_ua = -1;
	else if (charge_ua < 0)
		charge_ua = ua;
	else
		charge_ua += (ua - charge_ua) * (1 - exp(-(double)lv_tick_diff(now, charge_tick) / CHARGE_AVG_MS));
	charge_tick = now;
}

void ui_battery_duration(char *buf, size_t size, long minutes)
{
	if (minutes < 60)
		snprintf(buf, size, "%ld min", minutes);
	else if (minutes % 60 == 0)
		snprintf(buf, size, "%ld h", minutes / 60);
	else
		snprintf(buf, size, "%ld h %ld min", minutes / 60, minutes % 60);
}

bool ui_battery_time_to_full(char *buf, size_t size)
{
	long capacity, full;
	if (charge_ua <= 0 || !ui_read_long(UI_BATTERY "/capacity", &capacity) || capacity >= 100 ||
	    !ui_read_long(UI_BATTERY "/charge_full_design", &full) || full <= 0)
		return false;
	long minutes = lround((100 - capacity) / 100.0 * full / charge_ua * 60);
	if (minutes < 1)
		minutes = 1;
	ui_battery_duration(buf, size, minutes);
	return true;
}

/*
 * Battery history: the level and whether plugged in, sampled by ui_battery_update()
 * when phone-ui starts, when plugged in or unplugged, then every LOG_EVERY_MS. Lines of
 * "time level C|D" in LOG (on the data partition), the last LOG_KEEP seconds kept.
 * Settings, Battery draws it.
 */

#define LOG_DIR "/var/lib/phone-ui"
#define LOG LOG_DIR "/battery-history"
#define LOG_EVERY_MS 300000
#define LOG_KEEP (2 * 24 * 3600)
#define CLOCK_SET 1600000000 /* before it, the clock was not set yet */

static bool logged, logged_charging;
static uint32_t logged_tick;

/* Rewrites the file without the samples older than LOG_KEEP */
static void log_trim(time_t now)
{
	static struct battery_sample s[LOG_KEEP / (LOG_EVERY_MS / 1000) * 4];
	int n = battery_log_read(s, sizeof(s) / sizeof(s[0]));
	FILE *f = fopen(LOG ".tmp", "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++)
		if (s[i].time >= now - LOG_KEEP)
			fprintf(f, "%lld %d %c\n", (long long)s[i].time, s[i].level, s[i].charging ? 'C' : 'D');
	if (fclose(f) == 0)
		rename(LOG ".tmp", LOG);
}

static void log_sample(int level, bool charging)
{
	uint32_t tick = lv_tick_get();
	if (logged && charging == logged_charging && lv_tick_diff(tick, logged_tick) < LOG_EVERY_MS)
		return;
	time_t now = time(NULL);
	if (now < CLOCK_SET)
		return;
	mkdir(LOG_DIR, 0755);
	/* Trimmed once a run and then every few hours */
	static int count;
	if (count++ % 64 == 0)
		log_trim(now);
	FILE *f = fopen(LOG, "a");
	if (!f)
		return;
	fprintf(f, "%lld %d %c\n", (long long)now, level, charging ? 'C' : 'D');
	fclose(f);
	logged = true;
	logged_charging = charging;
	logged_tick = tick;
}

int battery_log_read(struct battery_sample *s, int max)
{
	FILE *f = fopen(LOG, "r");
	if (!f)
		return 0;
	int n = 0;
	char line[64], state;
	long long t;
	int level;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lld %d %c", &t, &level, &state) != 3 || level < 0 || level > 100)
			continue;
		/* The newest max: the oldest goes */
		if (n == max) {
			for (int i = 1; i < max; i++)
				s[i - 1] = s[i];
			n--;
		}
		s[n++] = (struct battery_sample){ (time_t)t, level, state == 'C' };
	}
	fclose(f);
	return n;
}

/*
 * Since unplugged, as Android counts it: how long the screen was on (ui_battery_screen(),
 * from ui_backlight_on()) and the phone asleep (CLOCK_BOOTTIME goes on asleep,
 * CLOCK_MONOTONIC stops), both from 0 when unplugged (ui_battery_plugged()). Kept in
 * SCREEN_TIME as "screen_ms asleep_ms plugged on_since saved", saved when the screen goes
 * off, WOKE_SAVE_MS after waking and every LOG_EVERY_MS: a restart goes on from there,
 * unless it was plugged then. The screen's periods go to SCREEN_HISTORY as "start end"
 * (wall clock) when it goes off; one still open when phone-ui stopped (on_since) ends at
 * its last save.
 */
#define SCREEN_TIME LOG_DIR "/screen-time"
#define SCREEN_HISTORY LOG_DIR "/screen-history"
/* Not at once: the first write after a resume waits for the eMMC (60 ms on the PinePhone),
 * which would hold back what woke the phone (a call's screen) */
#define WOKE_SAVE_MS 2000

static struct {
	bool started, plugged, on;
	uint64_t screen_ms, asleep_ms; /* since unplugged, without the screen's current period */
	uint64_t on_ms;                /* CLOCK_MONOTONIC when the screen went on */
	uint64_t slept_ms;             /* CLOCK_BOOTTIME - CLOCK_MONOTONIC, counted up to there */
	time_t on_since;               /* the wall clock then, 0 if it was not set */
	uint32_t saved_tick;
	bool woke;                     /* to save WOKE_SAVE_MS after woke_tick */
	uint32_t woke_tick;
} st;

static uint64_t clock_ms(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static time_t wall_now(void)
{
	time_t now = time(NULL);
	return now < CLOCK_SET ? 0 : now;
}

static uint64_t screen_total_ms(void)
{
	return st.screen_ms + (st.on ? clock_ms(CLOCK_MONOTONIC) - st.on_ms : 0);
}

static void screen_save(void)
{
	mkdir(LOG_DIR, 0755);
	FILE *f = fopen(SCREEN_TIME ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%llu %llu %d %lld %lld\n", (unsigned long long)screen_total_ms(),
	        (unsigned long long)st.asleep_ms, st.plugged, (long long)(st.on ? st.on_since : 0),
	        (long long)wall_now());
	if (fclose(f) == 0)
		rename(SCREEN_TIME ".tmp", SCREEN_TIME);
	st.saved_tick = lv_tick_get();
	st.woke = false;
}

int battery_screen_read(struct screen_period *p, int max)
{
	FILE *f = fopen(SCREEN_HISTORY, "r");
	int n = 0;
	char line[64];
	long long start, end;
	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lld %lld", &start, &end) != 2 || end < start)
			continue;
		if (n == max) {
			for (int i = 1; i < max; i++)
				p[i - 1] = p[i];
			n--;
		}
		p[n++] = (struct screen_period){ (time_t)start, (time_t)end };
	}
	if (f)
		fclose(f);
	time_t now = wall_now();
	if (st.on && st.on_since && now && max > 0) {
		if (n == max) {
			for (int i = 1; i < max; i++)
				p[i - 1] = p[i];
			n--;
		}
		p[n++] = (struct screen_period){ st.on_since, now };
	}
	return n;
}

/* Appended, the file trimmed to LOG_KEEP once a run and then every 64 periods */
static void screen_log(time_t start, time_t end)
{
	if (!start || end <= start)
		return;
	mkdir(LOG_DIR, 0755);
	static int count;
	if (count++ % 64 == 0) {
		static struct screen_period p[LOG_KEEP / 60];
		bool on = st.on;
		st.on = false; /* not the current one */
		int n = battery_screen_read(p, sizeof(p) / sizeof(p[0]));
		st.on = on;
		FILE *f = fopen(SCREEN_HISTORY ".tmp", "w");
		if (f) {
			for (int i = 0; i < n; i++)
				if (p[i].end >= end - LOG_KEEP)
					fprintf(f, "%lld %lld\n", (long long)p[i].start, (long long)p[i].end);
			if (fclose(f) == 0)
				rename(SCREEN_HISTORY ".tmp", SCREEN_HISTORY);
		}
	}
	FILE *f = fopen(SCREEN_HISTORY, "a");
	if (!f)
		return;
	fprintf(f, "%lld %lld\n", (long long)start, (long long)end);
	fclose(f);
}

/* When phone-ui starts, its screen on */
static void screen_start(bool plugged)
{
	unsigned long long screen, asleep;
	int was_plugged;
	long long on_since, saved;
	FILE *f = fopen(SCREEN_TIME, "r");
	if (f && fscanf(f, "%llu %llu %d %lld %lld", &screen, &asleep, &was_plugged, &on_since, &saved) == 5) {
		screen_log((time_t)on_since, (time_t)saved);
		if (!was_plugged || plugged) {
			st.screen_ms = screen;
			st.asleep_ms = asleep;
		}
	}
	if (f)
		fclose(f);
	st.started = true;
	st.plugged = plugged;
	st.on = true;
	st.on_ms = clock_ms(CLOCK_MONOTONIC);
	st.on_since = wall_now();
	st.slept_ms = clock_ms(CLOCK_BOOTTIME) - st.on_ms;
	screen_save();
}

void ui_battery_screen(bool on)
{
	if (!st.started || on == st.on)
		return;
	time_t now = wall_now();
	if (on) {
		st.on = true;
		st.on_ms = clock_ms(CLOCK_MONOTONIC);
		st.on_since = now;
		return;
	}
	st.screen_ms = screen_total_ms();
	st.on = false;
	if (now)
		screen_log(st.on_since, now);
	screen_save();
}

static void screen_sample(bool plugged)
{
	if (!st.started) {
		screen_start(plugged);
		return;
	}
	uint64_t mono = clock_ms(CLOCK_MONOTONIC), slept = clock_ms(CLOCK_BOOTTIME) - mono;
	bool woke = slept > st.slept_ms;
	if (woke) {
		st.woke = true;
		st.woke_tick = lv_tick_get();
		st.asleep_ms += slept - st.slept_ms;
		/* The screen's period from the time awake, whatever the wall clock said then */
		time_t now = wall_now();
		if (st.on && now)
			st.on_since = now - (time_t)((mono - st.on_ms) / 1000);
	}
	st.slept_ms = slept;
	bool unplugged = st.plugged && !plugged;
	st.plugged = plugged;
	if (unplugged) {
		st.screen_ms = st.asleep_ms = 0;
		st.on_ms = mono;
	}
	if (unplugged || lv_tick_elaps(st.saved_tick) >= LOG_EVERY_MS ||
	    (st.woke && lv_tick_elaps(st.woke_tick) >= WOKE_SAVE_MS))
		screen_save();
}

bool ui_battery_since_unplugged(long *screen_min, long *asleep_min)
{
	if (!st.started || st.plugged)
		return false;
	*screen_min = (long)(screen_total_ms() / 60000);
	*asleep_min = (long)(st.asleep_ms / 60000);
	return true;
}

void ui_battery_update(int level, const char *status)
{
	bool plugged = ui_battery_plugged(status);
	charge_sample(strcmp(status, "Charging") == 0);
	screen_sample(plugged);
	if (level >= 0 && ui_battery_present())
		log_sample(level, plugged);
	usage_tick(level, plugged, st.on);
}
