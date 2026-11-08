/* usage.c: the model of the current drawn, and where the charge went */

#include "test.h"

#include "system/usage.h"

/* The phone as the tests make it draw: each term's current at 1 (mA) */
static const double truth[USAGE_TERMS] = { 110, 90, 240, 170, 210, 60 };

static double drawn(const double *x)
{
	double ma = 0;
	for (int i = 0; i < USAGE_TERMS; i++)
		ma += truth[i] * x[i];
	return ma;
}

static const struct usage_entry *entry(const char *name)
{
	const struct usage_entry *e;
	int n = usage_entries(&e);
	for (int i = 0; i < n; i++)
		if (strcmp(e[i].name, name) == 0)
			return &e[i];
	return NULL;
}

static double mah(const char *name)
{
	const struct usage_entry *e = entry(name);
	return e ? e->mah : 0;
}

/* Deterministic pseudo-random in [0, 1) */
static double rnd(void)
{
	static unsigned s = 12345;
	s = s * 1103515245 + 12345;
	return (s >> 8 & 0xffff) / 65536.0;
}

static struct usage_interval interval(double screen, double bright, double cpu, double mobile, double wifi)
{
	struct usage_interval iv = { .seconds = 10 };
	iv.x[USAGE_BASE] = 1;
	iv.x[USAGE_SCREEN] = screen;
	iv.x[USAGE_BRIGHT] = screen * bright;
	iv.x[USAGE_CPU] = cpu;
	iv.x[USAGE_MOBILE] = mobile;
	iv.x[USAGE_WIFI] = wifi;
	iv.ma = drawn(iv.x);
	iv.users[iv.users_n++] = (struct usage_user){ USAGE_SYSTEM, cpu * iv.seconds, 0 };
	return iv;
}

TEST_SETUP
{
	CHECK(system("rm -rf " USAGE_DIR) == 0);
}

/* Before any measurement the model is its prior: a PinePhone's, roughly */
TEST(prior)
{
	usage_load();
	for (int i = 0; i < USAGE_TERMS; i++)
		CHECK(usage_term_ma(i) > 0);
	CHECK(usage_asleep_ma() > 10 && usage_asleep_ma() < 100);
	time_t t;
	int l;
	double a, s;
	CHECK(!usage_period(&t, &l, &t, &l, &a, &s));
}

/* From enough intervals of mixed use, each term's current is found (within 10%: the prior
 * still pulls on the screen and its brightness, which go together) */
TEST(learns_the_phone)
{
	usage_load();
	usage_reset(1000, 90);
	for (int i = 0; i < 3000; i++) {
		bool on = rnd() < 0.5;
		struct usage_interval iv = interval(on, on ? rnd() : 0, rnd() < 0.3 ? 4 * rnd() : 0.1 * rnd(),
		                                    rnd() < 0.2, rnd() < 0.1);
		usage_add(&iv);
	}
	for (int i = 0; i < USAGE_TERMS; i++) {
		if (fabs(usage_term_ma(i) - truth[i]) > truth[i] * 0.1)
			TEST_FAIL("term %d: %.1f mA, %.1f expected", i, usage_term_ma(i), truth[i]);
	}
}

/* An interval's charge goes to its parts by the model, scaled to what was measured;
 * the CPU to who used it, the radio to the apps whose CPU went with it */
TEST(charge_split)
{
	usage_load();
	usage_reset(1000, 90);
	struct usage_interval iv = interval(1, 0.5, 1.5, 1, 0);
	iv.users_n = 0;
	iv.users[iv.users_n++] = (struct usage_user){ "Telegram", 10, 10 };
	iv.users[iv.users_n++] = (struct usage_user){ USAGE_SYSTEM, 5, 0 };
	iv.seconds = 3600;
	usage_add(&iv);

	/* The model then is the prior's, the total is the measured one */
	double total = iv.ma, model = 0;
	for (int i = 0; i < USAGE_TERMS; i++)
		model += usage_term_ma(i) * iv.x[i];
	double k = total / model;
	CHECK_DBL(usage_total_mah(), total);
	CHECK(fabs(mah(USAGE_IDLE) - usage_term_ma(USAGE_BASE) * k) < 0.5);
	CHECK(fabs(mah(USAGE_SCREEN_ON) - (usage_term_ma(USAGE_SCREEN) + usage_term_ma(USAGE_BRIGHT) * 0.5) * k) < 0.5);
	double cpu = usage_term_ma(USAGE_CPU) * 1.5 * k, radio = usage_term_ma(USAGE_MOBILE) * k;
	CHECK(fabs(mah("Telegram") - (cpu * 2 / 3 + radio)) < 0.5);
	CHECK(fabs(mah(USAGE_SYSTEM) - cpu / 3) < 0.5);
	CHECK(!entry(USAGE_MOBILE_NET));
	CHECK_DBL(entry("Telegram")->seconds, 10);
	CHECK_DBL(entry(USAGE_SCREEN_ON)->seconds, 3600);
	CHECK_DBL(entry(USAGE_IDLE)->seconds, 3600);

	/* Most first */
	const struct usage_entry *e;
	int n = usage_entries(&e);
	for (int i = 1; i < n; i++)
		CHECK(e[i - 1].mah >= e[i].mah);
}

/* Data moved without an app's CPU: the network's own entry */
TEST(radio_without_app)
{
	usage_load();
	usage_reset(1000, 90);
	struct usage_interval iv = interval(0, 0, 0.1, 1, 1);
	usage_add(&iv);
	CHECK(mah(USAGE_MOBILE_NET) > 0);
	CHECK(mah(USAGE_WIFI_NET) > 0);
}

/* Not measured (no current): the model's guess, and nothing learned */
TEST(unmeasured)
{
	usage_load();
	usage_reset(1000, 90);
	double before = usage_term_ma(USAGE_SCREEN);
	struct usage_interval iv = interval(1, 1, 0, 0, 0);
	iv.ma = -1;
	iv.seconds = 3600;
	usage_add(&iv);
	CHECK_DBL(usage_term_ma(USAGE_SCREEN), before);
	CHECK(fabs(usage_total_mah() - (usage_term_ma(USAGE_BASE) + usage_term_ma(USAGE_SCREEN) +
	                                usage_term_ma(USAGE_BRIGHT))) < 0.01);
}

/* Asleep: the current learned from what the level fell over the sleeps */
TEST(asleep)
{
	usage_load();
	usage_reset(1000, 90);
	for (int i = 0; i < 100; i++)
		usage_asleep(3600, 25); /* 25 mA */
	CHECK(fabs(usage_asleep_ma() - 25) < 3);
	usage_asleep(7200, -1);
	CHECK(fabs(mah(USAGE_ASLEEP) - usage_total_mah()) < 1e-9);
	CHECK_DBL(entry(USAGE_ASLEEP)->seconds, 102 * 3600);
	time_t start, end;
	int l0, l1;
	double awake, asleep;
	CHECK(usage_period(&start, &l0, &end, &l1, &awake, &asleep));
	CHECK_DBL(asleep, 102 * 3600);
	CHECK_DBL(awake, 0);
}

/* Saved and loaded again: a restart goes on from there; unplugged again, from 0 */
TEST(kept)
{
	usage_load();
	usage_reset(1000, 95);
	struct usage_interval iv = interval(1, 0.5, 1, 0, 0);
	iv.users[0].name = "Notes";
	iv.users[0].screen_s = 10;
	for (int i = 0; i < 500; i++)
		usage_add(&iv);
	usage_level(6000, 80);
	usage_save();
	double total = usage_total_mah(), notes = mah("Notes"), screen = usage_term_ma(USAGE_SCREEN);

	usage_reset(0, -1); /* forgotten here, as in a new process */
	usage_load();
	CHECK(fabs(usage_total_mah() - total) < 1e-3);
	CHECK(fabs(mah("Notes") - notes) < 1e-3);
	CHECK(fabs(usage_term_ma(USAGE_SCREEN) - screen) < 1e-3);
	time_t start, end;
	int l0, l1;
	double awake, asleep;
	CHECK(usage_period(&start, &l0, &end, &l1, &awake, &asleep));
	CHECK_INT(start, 1000);
	CHECK_INT(l0, 95);
	CHECK_INT(end, 6000);
	CHECK_INT(l1, 80);
	CHECK_DBL(awake, 5000);

	usage_reset(7000, 100);
	CHECK_DBL(usage_total_mah(), 0);
	CHECK(fabs(usage_term_ma(USAGE_SCREEN) - screen) < 1e-3); /* the model stays */
}

/* A term that seems to save current (Wi-Fi on while something else idled): held at 0,
 * the others fitted without it */
TEST(no_term_below_zero)
{
	usage_load();
	usage_reset(1000, 90);
	for (int i = 0; i < 3000; i++) {
		bool on = rnd() < 0.5, wifi = rnd() < 0.3;
		struct usage_interval iv = interval(on, on ? rnd() : 0, rnd() < 0.3 ? 4 * rnd() : 0.1 * rnd(),
		                                    rnd() < 0.2, wifi);
		iv.ma -= wifi ? 2 * truth[USAGE_WIFI] : 0;
		usage_add(&iv);
	}
	CHECK_DBL(usage_term_ma(USAGE_WIFI), 0);
	for (int i = 0; i < USAGE_TERMS; i++)
		CHECK(usage_term_ma(i) >= 0);
}

/* More apps than are kept: the ones that used least make room */
TEST(many_apps)
{
	usage_load();
	usage_reset(1000, 90);
	char names[60][16];
	for (int i = 0; i < 60; i++) {
		struct usage_interval iv = interval(0, 0, 1, 0, 0);
		snprintf(names[i], sizeof(names[i]), "App %d", i);
		iv.users[0] = (struct usage_user){ names[i], 0.1 * (i + 1), 0 };
		usage_add(&iv);
	}
	const struct usage_entry *e;
	int n = usage_entries(&e);
	CHECK(n <= 48);
	CHECK(entry("App 59") != NULL); /* the most */
	CHECK(entry("App 30") != NULL);
	int apps = 0;
	for (int i = 0; i < n; i++)
		apps += strncmp(e[i].name, "App ", 4) == 0;
	CHECK(apps < 60);
}

/* An interval of no time adds nothing */
TEST(empty_interval)
{
	usage_load();
	usage_reset(1000, 90);
	struct usage_interval iv = interval(1, 0.5, 1, 0, 0);
	iv.seconds = 0;
	usage_add(&iv);
	CHECK_DBL(usage_total_mah(), 0);
}

/* Saved before any time on battery was counted: the model only */
TEST(saved_before_counting)
{
	usage_load();
	usage_save();
	FILE *f = fopen(USAGE_DIR "/battery-usage", "r");
	CHECK(f == NULL);
	if (f)
		fclose(f);
	f = fopen(USAGE_DIR "/power-model", "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
}

/* A file that is not ours, or cut short: ignored */
TEST(bad_file)
{
	CHECK(system("mkdir -p " USAGE_DIR " && echo garbage > " USAGE_DIR "/battery-usage && "
	             "printf 'model 1\\n1 2' > " USAGE_DIR "/power-model") == 0);
	usage_load();
	CHECK_DBL(usage_total_mah(), 0);
	CHECK(usage_term_ma(USAGE_BASE) > 0);
}
