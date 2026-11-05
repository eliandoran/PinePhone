/*
 * The battery: whether the cell is usable, the time to full while charging and the
 * history of its level, all sampled by ui_battery_update() from ui_status_update().
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "ui.h"

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
	         "Full" : "Not charging");
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

bool ui_battery_time_to_full(char *buf, size_t size)
{
	long capacity, full;
	if (charge_ua <= 0 || !ui_read_long(UI_BATTERY "/capacity", &capacity) || capacity >= 100 ||
	    !ui_read_long(UI_BATTERY "/charge_full_design", &full) || full <= 0)
		return false;
	long minutes = lround((100 - capacity) / 100.0 * full / charge_ua * 60);
	if (minutes < 1)
		minutes = 1;
	if (minutes < 60)
		snprintf(buf, size, "%ld min", minutes);
	else if (minutes % 60 == 0)
		snprintf(buf, size, "%ld h", minutes / 60);
	else
		snprintf(buf, size, "%ld h %ld min", minutes / 60, minutes % 60);
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

void ui_battery_update(int level, const char *status)
{
	bool plugged = ui_battery_plugged(status);
	charge_sample(strcmp(status, "Charging") == 0);
	if (level >= 0 && ui_battery_present())
		log_sample(level, plugged);
}
