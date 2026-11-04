/*
 * Battery history: the level and whether it charges, sampled by ui_status_update()
 * when phone-ui starts, when charging starts or stops, then every LOG_EVERY_MS. Lines of
 * "time level C|D" in LOG (on the data partition), the last LOG_KEEP seconds kept.
 * Settings, Battery draws it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "ui.h"

#define LOG_DIR "/var/lib/phone-ui"
#define LOG LOG_DIR "/battery-history"
#define LOG_EVERY_MS 300000
#define LOG_KEEP (2 * 24 * 3600)
#define CLOCK_SET 1600000000 /* before it, the clock was not set yet */

static bool logged, logged_charging;
static uint32_t logged_tick;

/* Rewrites the file without the samples older than LOG_KEEP */
static void battery_log_trim(time_t now)
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

void battery_log_sample(int level, bool charging)
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
		battery_log_trim(now);
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
