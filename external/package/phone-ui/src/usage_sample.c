/*
 * Battery use's measurements (usage.h): each second the battery's current, the screen and
 * the app on it; every INTERVAL_MS what ran (/proc) and what the radios moved, one
 * interval for usage.c. Only on battery, awake (phone-ui does not run asleep: a sleep is
 * seen on waking, CLOCK_BOOTTIME ahead of CLOCK_MONOTONIC, the level it fell to usage.c).
 *
 * Who used the CPU: phone-ui's main thread is the app on screen (the one opened last whose
 * screen is still there; home, the lock screen, the screen off: System); its other threads
 * belong to the app on screen when each was first seen (plugins start theirs when opened:
 * TDLib's); other processes are an external app's when their program is in its folder
 * (WebKit's too), Music's player is Music's, the rest System.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ui.h"
#include "usage.h"

#ifndef USAGE_PROC
#define USAGE_PROC "/proc"
#endif
#define NET "/sys/class/net"
#define MOBILE "wwan0"
#define WIFI "wlan0"

#define INTERVAL_MS 10000
#define SAVE_MS 300000
/* Bytes in an interval that kept a radio active; after it, the next interval half (its
 * tail: LTE stays connected about 10 s after the last packet) */
#define RADIO_BYTES 1024
#define APPS_SHOWN 8
#define THREADS 64
#define PROCS 256

struct counter {
	int id;
	unsigned long long start; /* a process's start time: a pid used again is another */
	unsigned long long ticks;
	const char *owner;
	bool seen; /* in this scan */
};

static struct {
	bool started, plugged, screen_was_on;
	bool scanned; /* threads seen from now on are the app's on screen */
	int level;
	uint64_t slept_ms;
	/* The interval: since start_tick, each second's sums */
	uint32_t start_tick, last_tick, saved_tick;
	double seconds, ma_sum, screen_s, bright_s;
	int ma_n;
	struct usage_user users[USAGE_USERS];
	int users_n;
	/* The counters at its start */
	unsigned long long busy;
	unsigned long long bytes[2];
	bool active[2];
	struct counter threads[THREADS], procs[PROCS];
	int threads_n, procs_n;
	/* Apps opened, the last last: their first screen while it lives */
	struct {
		const char *name;
		lv_obj_t *screen;
	} shown[APPS_SHOWN];
	int shown_n;
} us;

static uint64_t clock_ms(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Apps on screen */

static void shown_deleted_cb(lv_event_t *e)
{
	lv_obj_t *screen = lv_event_get_target_obj(e);
	for (int i = 0; i < us.shown_n; i++)
		if (us.shown[i].screen == screen) {
			memmove(&us.shown[i], &us.shown[i + 1], (size_t)(us.shown_n - i - 1) * sizeof(us.shown[0]));
			us.shown_n--;
			i--;
		}
}

void usage_app_opened(const char *name)
{
	if (ui_at_home())
		return; /* nothing pushed: an external app's process, or not done yet */
	lv_obj_t *screen = ui_current();
	if (us.shown_n == APPS_SHOWN) {
		memmove(&us.shown[0], &us.shown[1], (APPS_SHOWN - 1) * sizeof(us.shown[0]));
		us.shown_n--;
	}
	us.shown[us.shown_n++] = (typeof(us.shown[0])){ name, screen };
	lv_obj_add_event_cb(screen, shown_deleted_cb, LV_EVENT_DELETE, NULL);
}

static const char *app_shown(void)
{
	return us.shown_n && !ui_at_home() && !ui_locked() ? us.shown[us.shown_n - 1].name : NULL;
}

static struct usage_user *user(const char *name)
{
	for (int i = 0; i < us.users_n; i++)
		if (strcmp(us.users[i].name, name) == 0)
			return &us.users[i];
	if (us.users_n == USAGE_USERS)
		return &us.users[0]; /* System's, the first */
	us.users[us.users_n] = (struct usage_user){ name, 0, 0 };
	return &us.users[us.users_n++];
}

/* /proc */

/* All cores' busy time (ticks) */
static bool cpu_busy(unsigned long long *busy)
{
	FILE *f = fopen(USAGE_PROC "/stat", "r");
	unsigned long long v[8];
	/* NON-NLS-BEGIN */
	bool ok = f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
	                      &v[4], &v[5], &v[6], &v[7]) == 8;
	/* NON-NLS-END */
	if (f)
		fclose(f);
	if (ok) /* user nice system (idle iowait) irq softirq steal */
		*busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
	return ok;
}

/* A stat file's name (comm), CPU time (utime + stime) and start time */
static bool stat_read(const char *path, char *comm, size_t size, unsigned long long *ticks,
                      unsigned long long *start)
{
	char buf[512];
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	char *open = strchr(buf, '('), *close = strrchr(buf, ')');
	if (!open || !close || close < open)
		return false;
	snprintf(comm, size, "%.*s", (int)(close - open - 1), open + 1);
	/* After the name: state is field 3, utime 14, stime 15, starttime 22 */
	unsigned long long utime, stime;
	if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %llu",
	           &utime, &stime, start) != 3)
		return false;
	*ticks = utime + stime;
	return true;
}

/* An app by its program's folder */
static const char *exe_owner(int pid, const char *comm)
{
	char path[64], exe[PATH_MAX];
	if (strcmp(comm, "mpg123") == 0) {
		const struct ui_app *music = ui_app_find("Music"); /* NON-NLS: its name */
		return music ? music->name : USAGE_SYSTEM;
	}
	snprintf(path, sizeof(path), USAGE_PROC "/%d/exe", pid);
	ssize_t n = readlink(path, exe, sizeof(exe) - 1);
	if (n <= 0)
		return USAGE_SYSTEM; /* the kernel's threads have none */
	exe[n] = '\0';
	for (unsigned i = 0; i < ui_app_count; i++) {
		const char *dir = ui_apps[i].dir;
		size_t len = dir ? strlen(dir) : 0;
		if (len && strncmp(exe, dir, len) == 0 && exe[len] == '/')
			return ui_apps[i].name;
	}
	return USAGE_SYSTEM;
}

/* The counter of `id` (a new one owned by owner()), its ticks since the last scan */
static unsigned long long counter_delta(struct counter *c, int *n, int max, int id, unsigned long long start,
                                        unsigned long long ticks, const char *owner, const char **who)
{
	struct counter *k = NULL;
	for (int i = 0; i < *n && !k; i++)
		if (c[i].id == id && c[i].start == start)
			k = &c[i];
	if (!k) {
		if (*n == max)
			return 0;
		k = &c[(*n)++];
		*k = (struct counter){ id, start, ticks, owner, true };
		*who = owner;
		return 0; /* first seen: from now */
	}
	unsigned long long d = ticks >= k->ticks ? ticks - k->ticks : 0;
	k->ticks = ticks;
	k->seen = true;
	*who = k->owner;
	return d;
}

/* Counters gone since the last scan */
static void counter_sweep(struct counter *c, int *n)
{
	int j = 0;
	for (int i = 0; i < *n; i++)
		if (c[i].seen) {
			c[i].seen = false;
			c[j++] = c[i];
		}
	*n = j;
}

/* What each thread and process used since the last scan, to its owner: `charge` false at
 * the interval's start (only counted from there). Returns the ticks charged */
static unsigned long long scan(bool charge, double hz, const char *on_screen, double interval_s)
{
	unsigned long long charged = 0;
	char path[300], comm[64];
	unsigned long long ticks, start;
	const char *who;
	int self = (int)getpid();

	DIR *d = opendir(USAGE_PROC "/self/task");
	struct dirent *de;
	while (d && (de = readdir(d))) {
		int tid = atoi(de->d_name);
		snprintf(path, sizeof(path), USAGE_PROC "/self/task/%s/stat", de->d_name);
		if (tid <= 0 || !stat_read(path, comm, sizeof(comm), &ticks, &start))
			continue;
		/* Those there at the first scan (at startup): phone-ui's own */
		const char *owner = us.scanned && on_screen ? on_screen : USAGE_SYSTEM;
		unsigned long long dt = counter_delta(us.threads, &us.threads_n, THREADS, tid, start, ticks, owner, &who);
		if (!charge || !dt)
			continue;
		charged += dt;
		if (tid != self) {
			user(who)->cpu_s += dt / hz;
			continue;
		}
		/* The main thread: the apps on screen for their part of the interval */
		double s = dt / hz, left = s;
		for (int i = 0; i < us.users_n; i++)
			if (us.users[i].screen_s > 0 && interval_s > 0) {
				double part = s * us.users[i].screen_s / interval_s;
				us.users[i].cpu_s += part;
				left -= part;
			}
		user(USAGE_SYSTEM)->cpu_s += left > 0 ? left : 0;
	}
	if (d)
		closedir(d);
	counter_sweep(us.threads, &us.threads_n);

	d = opendir(USAGE_PROC);
	while (d && (de = readdir(d))) {
		int pid = atoi(de->d_name);
		if (pid <= 0 || pid == self)
			continue;
		snprintf(path, sizeof(path), USAGE_PROC "/%s/stat", de->d_name);
		if (!stat_read(path, comm, sizeof(comm), &ticks, &start))
			continue;
		struct counter *k = NULL;
		for (int i = 0; i < us.procs_n && !k; i++)
			if (us.procs[i].id == pid && us.procs[i].start == start)
				k = &us.procs[i];
		unsigned long long dt = counter_delta(us.procs, &us.procs_n, PROCS, pid, start, ticks,
		                                      k ? k->owner : exe_owner(pid, comm), &who);
		if (charge && dt) {
			charged += dt;
			user(who)->cpu_s += dt / hz;
		}
	}
	if (d)
		closedir(d);
	counter_sweep(us.procs, &us.procs_n);
	us.scanned = true;
	return charged;
}

/* A network interface's bytes both ways */
static unsigned long long net_bytes(const char *dev)
{
	char path[128];
	long rx = 0, tx = 0;
	snprintf(path, sizeof(path), NET "/%s/statistics/rx_bytes", dev);
	ui_read_long(path, &rx);
	snprintf(path, sizeof(path), NET "/%s/statistics/tx_bytes", dev);
	ui_read_long(path, &tx);
	return (unsigned long long)rx + (unsigned long long)tx;
}

/* The interval */

/* `fresh`: the counters from now (after a sleep, unplugged), else they are the last
 * interval's end */
static void interval_start(uint32_t tick, bool fresh)
{
	us.start_tick = us.last_tick = tick;
	us.seconds = us.ma_sum = us.screen_s = us.bright_s = 0;
	us.ma_n = 0;
	us.users_n = 0;
	user(USAGE_SYSTEM); /* the first */
	if (!fresh)
		return;
	if (!cpu_busy(&us.busy))
		us.busy = 0;
	scan(false, 1, app_shown(), 0);
	us.bytes[0] = net_bytes(MOBILE);
	us.bytes[1] = net_bytes(WIFI);
	us.active[0] = us.active[1] = false;
}

static double radio(int i, unsigned long long bytes)
{
	bool moved = bytes - us.bytes[i] >= RADIO_BYTES && bytes >= us.bytes[i];
	double x = moved ? 1 : us.active[i] ? 0.5 : 0;
	us.active[i] = moved;
	us.bytes[i] = bytes;
	return x;
}

static void interval_end(int level)
{
	struct usage_interval iv = { .seconds = us.seconds };
	if (iv.seconds <= 0)
		return;
	double hz = (double)sysconf(_SC_CLK_TCK);
	unsigned long long busy;
	iv.ma = us.ma_n ? us.ma_sum / us.ma_n : -1;
	iv.x[USAGE_BASE] = 1;
	iv.x[USAGE_SCREEN] = us.screen_s / iv.seconds;
	iv.x[USAGE_BRIGHT] = us.bright_s / iv.seconds;
	double all = 0;
	if (cpu_busy(&busy)) {
		all = busy >= us.busy ? (busy - us.busy) / hz : 0;
		us.busy = busy;
	}
	double counted = scan(true, hz, app_shown(), iv.seconds) / hz;
	/* What no thread or process says it used (the kernel's interrupts, those gone) */
	if (all > counted)
		user(USAGE_SYSTEM)->cpu_s += all - counted;
	iv.x[USAGE_CPU] = (all > counted ? all : counted) / iv.seconds;
	iv.x[USAGE_MOBILE] = radio(0, net_bytes(MOBILE));
	iv.x[USAGE_WIFI] = radio(1, net_bytes(WIFI));
	memcpy(iv.users, us.users, sizeof(iv.users));
	iv.users_n = us.users_n;
	usage_add(&iv);
	usage_level(time(NULL), level);
}

static double design_mah(void)
{
	long uah;
	return ui_read_long(UI_BATTERY "/charge_full_design", &uah) && uah > 0 ? uah / 1000.0 : 3000;
}

void usage_tick(int level, bool plugged, bool screen_on)
{
	uint32_t tick = lv_tick_get();
	uint64_t mono = clock_ms(CLOCK_MONOTONIC), slept = clock_ms(CLOCK_BOOTTIME) - mono;
	time_t now = time(NULL);
	if (!us.started) {
		usage_load();
		time_t t;
		int l;
		double a, s;
		/* The first time, and after charging while phone-ui was not running */
		if (!plugged && !usage_period(&t, &l, &t, &l, &a, &s))
			usage_reset(now, level);
		us.plugged = plugged;
		us.level = level;
		us.slept_ms = slept;
		us.saved_tick = tick;
		us.started = true;
		interval_start(tick, true);
		return;
	}

	/* Woken: the sleep, then a new interval */
	if (slept > us.slept_ms) {
		if (!us.plugged) {
			double fell = level >= 0 && us.level >= 0 && level <= us.level ?
			              (us.level - level) * design_mah() / 100 : -1;
			usage_asleep((slept - us.slept_ms) / 1000.0, fell);
			usage_level(now, level);
		}
		us.slept_ms = slept;
		us.level = level;
		interval_start(tick, true);
		return;
	}
	us.level = level;

	if (plugged != us.plugged) {
		if (!plugged)
			usage_reset(now, level);
		else
			interval_end(level);
		us.plugged = plugged;
		usage_save();
		us.saved_tick = tick;
		interval_start(tick, true);
		return;
	}
	if (plugged) {
		us.last_tick = tick;
		return;
	}

	/* This second */
	double s = lv_tick_diff(tick, us.last_tick) / 1000.0;
	us.last_tick = tick;
	us.seconds += s;
	long ua;
	if (ui_read_long(UI_BATTERY "/current_now", &ua)) {
		us.ma_sum += ua < 0 ? -ua / 1000.0 : 0; /* below 0: discharging */
		us.ma_n++;
	}
	if (screen_on) {
		us.screen_s += s;
		int b = ui_brightness_get();
		us.bright_s += s * (b >= 0 ? b / 100.0 : 0.5);
		const char *app = app_shown();
		if (app)
			user(app)->screen_s += s;
	}

	bool off = us.screen_was_on && !screen_on;
	us.screen_was_on = screen_on;
	if (lv_tick_diff(tick, us.start_tick) >= INTERVAL_MS || off) {
		interval_end(level);
		if (off || lv_tick_diff(tick, us.saved_tick) >= SAVE_MS) {
			usage_save();
			us.saved_tick = tick;
		}
		interval_start(tick, false);
	}
}
