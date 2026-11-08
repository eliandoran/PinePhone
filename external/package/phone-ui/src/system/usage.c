/*
 * Battery use: where the charge went since unplugged, as Android's battery usage.
 *
 * Android multiplies how long each part of the phone was in each state by a current the
 * maker measured for it (power_profile.xml). Here the battery's own current is measured
 * (the AXP803's current_now), so the profile is learned instead: usage_sample.c sums each
 * interval awake on battery (10 s) into its terms (usage.h: the phone awake, the screen,
 * its brightness, cores busy, the modem's and Wi-Fi's radios active) and the current drawn,
 * and the current is fitted as a sum of the terms, each times its own current: least
 * squares, held towards a PinePhone's rough figures (PRIOR) until there is data to say
 * otherwise (ridge regression), old intervals fading (DECAY), no term below 0.
 *
 * An interval's charge, the measured current times its length, then goes to its terms as
 * the model splits it: the screen's to Screen, the phone awake's to Idle, the CPU's to who
 * used it (apps, or System: phone-ui itself, the kernel, daemons), and the radios' to the
 * apps whose CPU went with the data (an app receiving is busy too), Mobile network and
 * Wi-Fi when none was. Asleep nothing runs to measure: its current is learned from what
 * the level fell over the sleeps, from a prior too.
 *
 * Kept on the data partition: the model (MODEL) and the totals since unplugged (TOTALS).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "i18n.h"
#include "usage.h"

#ifndef USAGE_DIR
#define USAGE_DIR "/var/lib/phone-ui"
#endif
#define MODEL USAGE_DIR "/power-model"
#define TOTALS USAGE_DIR "/battery-usage"

/* The entries' names, translated where shown */
static const char *const names[] __attribute__((unused)) = {
	N_("Idle"), N_("Screen"), N_("System"), N_("Mobile network"), N_("Wi-Fi"), N_("Asleep"),
};

/* A PinePhone's currents at 3.8 V, roughly (mA): awake and idle with the modem on, the
 * panel on at its lowest, its backlight's more at full, a core busy at its top frequency,
 * the LTE radio connected, Wi-Fi moving data */
static const double PRIOR[USAGE_TERMS] = { 130, 100, 180, 150, 150, 80 };
/* The prior weighs as that many intervals of 10 s on each term */
#define PRIOR_WEIGHT 20.0
/* Each interval weighs that much less (10 s ones: half in about 10 h awake) */
#define DECAY 0.9998
#define INTERVAL_S 10.0
/* Asleep: 40 mA (1.3% of 3000 mAh an hour), worth 4 hours asleep; halved past 48 h */
#define ASLEEP_PRIOR_MA 40.0
#define ASLEEP_PRIOR_H 4.0
#define ASLEEP_KEEP_H 48.0

#define ENTRIES 48

static struct {
	/* The fit: sum of w x x', of w x y */
	double xx[USAGE_TERMS][USAGE_TERMS], xy[USAGE_TERMS];
	double beta[USAGE_TERMS];
	double asleep_h, asleep_mah; /* the sleeps measured */
	bool solved;

	bool started;
	time_t start, end;
	int start_level, end_level;
	double awake_s, asleep_s;
	struct usage_entry entry[ENTRIES];
	int n;
} u;

/* (xx + PRIOR_WEIGHT I) beta = xy + PRIOR_WEIGHT PRIOR, the terms in `zero` held at 0 */
static void solve_with(bool zero[USAGE_TERMS])
{
	double a[USAGE_TERMS][USAGE_TERMS + 1];
	for (int i = 0; i < USAGE_TERMS; i++) {
		for (int j = 0; j < USAGE_TERMS; j++)
			a[i][j] = zero[i] || zero[j] ? (i == j) : u.xx[i][j] + (i == j) * PRIOR_WEIGHT;
		a[i][USAGE_TERMS] = zero[i] ? 0 : u.xy[i] + PRIOR_WEIGHT * PRIOR[i];
	}
	/* Gaussian elimination, partial pivoting: positive definite, it never meets 0 */
	for (int c = 0; c < USAGE_TERMS; c++) {
		int p = c;
		for (int r = c + 1; r < USAGE_TERMS; r++)
			if (fabs(a[r][c]) > fabs(a[p][c]))
				p = r;
		for (int k = 0; k <= USAGE_TERMS; k++) {
			double t = a[c][k];
			a[c][k] = a[p][k];
			a[p][k] = t;
		}
		for (int r = 0; r < USAGE_TERMS; r++) {
			if (r == c || a[r][c] == 0)
				continue;
			double f = a[r][c] / a[c][c];
			for (int k = c; k <= USAGE_TERMS; k++)
				a[r][k] -= f * a[c][k];
		}
	}
	for (int i = 0; i < USAGE_TERMS; i++)
		u.beta[i] = a[i][USAGE_TERMS] / a[i][i];
}

/* A term found below 0 is held at 0 and the others fitted again */
static void solve(void)
{
	bool zero[USAGE_TERMS] = { false };
	for (int round = 0; round < USAGE_TERMS; round++) {
		solve_with(zero);
		bool again = false;
		for (int i = 0; i < USAGE_TERMS; i++)
			if (!zero[i] && u.beta[i] < 0)
				zero[i] = again = true;
		if (!again)
			break;
	}
	for (int i = 0; i < USAGE_TERMS; i++)
		if (u.beta[i] < 0)
			u.beta[i] = 0;
	u.solved = true;
}

double usage_term_ma(enum usage_term term)
{
	if (!u.solved)
		solve();
	return u.beta[term];
}

double usage_asleep_ma(void)
{
	return (u.asleep_mah + ASLEEP_PRIOR_MA * ASLEEP_PRIOR_H) / (u.asleep_h + ASLEEP_PRIOR_H);
}

static int entry_cmp(const void *a, const void *b)
{
	const struct usage_entry *x = a, *y = b;
	return (x->mah < y->mah) - (x->mah > y->mah);
}

static struct usage_entry *entry(const char *name)
{
	for (int i = 0; i < u.n; i++)
		if (strcmp(u.entry[i].name, name) == 0)
			return &u.entry[i];
	/* Full: the smallest one goes */
	if (u.n == ENTRIES) {
		qsort(u.entry, (size_t)u.n, sizeof(u.entry[0]), entry_cmp);
		u.n--;
	}
	struct usage_entry *e = &u.entry[u.n++];
	memset(e, 0, sizeof(*e));
	snprintf(e->name, sizeof(e->name), "%s", name);
	return e;
}

static void charge(const char *name, double mah, double seconds)
{
	if (mah <= 0 && seconds <= 0)
		return;
	struct usage_entry *e = entry(name);
	e->mah += mah;
	e->seconds += seconds;
}

void usage_reset(time_t now, int level)
{
	u.started = true;
	u.start = u.end = now;
	u.start_level = u.end_level = level;
	u.awake_s = u.asleep_s = 0;
	u.n = 0;
}

static void learn(const struct usage_interval *iv)
{
	double w = iv->seconds / INTERVAL_S;
	for (int i = 0; i < USAGE_TERMS; i++) {
		for (int j = 0; j < USAGE_TERMS; j++)
			u.xx[i][j] = u.xx[i][j] * DECAY + w * iv->x[i] * iv->x[j];
		u.xy[i] = u.xy[i] * DECAY + w * iv->x[i] * iv->ma;
	}
	u.solved = false;
}

/* `mah` to the users other than System by their CPU, `fallback` without any */
static void to_apps(const struct usage_interval *iv, double mah, const char *fallback)
{
	double cpu = 0;
	for (int i = 0; i < iv->users_n; i++)
		if (strcmp(iv->users[i].name, USAGE_SYSTEM) != 0)
			cpu += iv->users[i].cpu_s;
	if (cpu <= 0) {
		charge(fallback, mah, 0);
		return;
	}
	for (int i = 0; i < iv->users_n; i++)
		if (strcmp(iv->users[i].name, USAGE_SYSTEM) != 0)
			charge(iv->users[i].name, mah * iv->users[i].cpu_s / cpu, 0);
}

void usage_add(const struct usage_interval *iv)
{
	if (iv->seconds <= 0)
		return;
	if (iv->ma >= 0)
		learn(iv);
	double part[USAGE_TERMS], model = 0;
	for (int i = 0; i < USAGE_TERMS; i++)
		model += part[i] = usage_term_ma(i) * iv->x[i];
	/* mAh per mA of the model */
	double k = iv->seconds / 3600 * (iv->ma >= 0 && model > 0 ? iv->ma / model : 1);

	charge(USAGE_IDLE, part[USAGE_BASE] * k, iv->seconds);
	charge(USAGE_SCREEN_ON, (part[USAGE_SCREEN] + part[USAGE_BRIGHT]) * k, iv->x[USAGE_SCREEN] * iv->seconds);
	double cpu = 0;
	for (int i = 0; i < iv->users_n; i++)
		cpu += iv->users[i].cpu_s;
	for (int i = 0; i < iv->users_n; i++)
		charge(iv->users[i].name, cpu > 0 ? part[USAGE_CPU] * k * iv->users[i].cpu_s / cpu : 0,
		       iv->users[i].screen_s);
	if (cpu <= 0)
		charge(USAGE_SYSTEM, part[USAGE_CPU] * k, 0);
	to_apps(iv, part[USAGE_MOBILE] * k, USAGE_MOBILE_NET);
	to_apps(iv, part[USAGE_WIFI] * k, USAGE_WIFI_NET);
	u.awake_s += iv->seconds;
}

void usage_asleep(double seconds, double fell_mah)
{
	if (seconds <= 0)
		return;
	if (fell_mah >= 0) {
		u.asleep_h += seconds / 3600;
		u.asleep_mah += fell_mah;
		if (u.asleep_h > ASLEEP_KEEP_H) {
			u.asleep_h /= 2;
			u.asleep_mah /= 2;
		}
	}
	charge(USAGE_ASLEEP, usage_asleep_ma() * seconds / 3600, seconds);
	u.asleep_s += seconds;
}

void usage_level(time_t now, int level)
{
	u.end = now;
	u.end_level = level;
}

int usage_entries(const struct usage_entry **entries)
{
	qsort(u.entry, (size_t)u.n, sizeof(u.entry[0]), entry_cmp);
	*entries = u.entry;
	return u.n;
}

double usage_total_mah(void)
{
	double total = 0;
	for (int i = 0; i < u.n; i++)
		total += u.entry[i].mah;
	return total;
}

bool usage_period(time_t *start, int *start_level, time_t *end, int *end_level, double *awake_s,
                  double *asleep_s)
{
	if (!u.started)
		return false;
	*start = u.start;
	*start_level = u.start_level;
	*end = u.end;
	*end_level = u.end_level;
	*awake_s = u.awake_s;
	*asleep_s = u.asleep_s;
	return true;
}

/* Written beside and renamed: a cut write leaves the last one */
static FILE *save_open(const char *path, char *tmp, size_t size)
{
	mkdir(USAGE_DIR, 0755);
	snprintf(tmp, size, "%s.tmp", path);
	return fopen(tmp, "w");
}

static void save_close(FILE *f, const char *tmp, const char *path)
{
	if (fclose(f) == 0)
		rename(tmp, path);
}

void usage_save(void)
{
	char tmp[256];
	FILE *f = save_open(MODEL, tmp, sizeof(tmp));
	if (f) {
		fprintf(f, "model 1\n");
		for (int i = 0; i < USAGE_TERMS; i++) {
			for (int j = 0; j < USAGE_TERMS; j++)
				fprintf(f, "%.9g ", u.xx[i][j]);
			fprintf(f, "%.9g\n", u.xy[i]);
		}
		fprintf(f, "%.9g %.9g\n", u.asleep_h, u.asleep_mah);
		save_close(f, tmp, MODEL);
	}
	if (!u.started)
		return;
	f = save_open(TOTALS, tmp, sizeof(tmp));
	if (!f)
		return;
	fprintf(f, "usage 1\n%lld %d %lld %d %.3f %.3f\n", (long long)u.start, u.start_level,
	        (long long)u.end, u.end_level, u.awake_s, u.asleep_s);
	for (int i = 0; i < u.n; i++)
		fprintf(f, "%.6f %.3f %s\n", u.entry[i].mah, u.entry[i].seconds, u.entry[i].name);
	save_close(f, tmp, TOTALS);
}

static void load_model(void)
{
	FILE *f = fopen(MODEL, "r");
	if (!f)
		return;
	double xx[USAGE_TERMS][USAGE_TERMS], xy[USAGE_TERMS], h, mah;
	bool ok = fscanf(f, "model 1 ") == 0; /* NON-NLS */
	for (int i = 0; ok && i < USAGE_TERMS; i++) {
		for (int j = 0; ok && j < USAGE_TERMS; j++)
			ok = fscanf(f, "%lf", &xx[i][j]) == 1;
		ok = ok && fscanf(f, "%lf", &xy[i]) == 1;
	}
	ok = ok && fscanf(f, "%lf %lf", &h, &mah) == 2 && h >= 0 && mah >= 0;
	fclose(f);
	if (!ok)
		return;
	memcpy(u.xx, xx, sizeof(xx));
	memcpy(u.xy, xy, sizeof(xy));
	u.asleep_h = h;
	u.asleep_mah = mah;
	u.solved = false;
}

static void load_totals(void)
{
	FILE *f = fopen(TOTALS, "r");
	if (!f)
		return;
	char line[160];
	long long start, end;
	int l0, l1;
	double awake, asleep;
	if (!fgets(line, sizeof(line), f) || strcmp(line, "usage 1\n") != 0 || !fgets(line, sizeof(line), f) ||
	    sscanf(line, "%lld %d %lld %d %lf %lf", &start, &l0, &end, &l1, &awake, &asleep) != 6) {
		fclose(f);
		return;
	}
	usage_reset((time_t)start, l0);
	u.end = (time_t)end;
	u.end_level = l1;
	u.awake_s = awake;
	u.asleep_s = asleep;
	double mah, seconds;
	int at;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (sscanf(line, "%lf %lf %n", &mah, &seconds, &at) == 2 && line[at])
			charge(line + at, mah, seconds);
	}
	fclose(f);
}

void usage_load(void)
{
	load_model();
	load_totals();
}
