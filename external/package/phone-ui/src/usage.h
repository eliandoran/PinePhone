/*
 * Battery use (usage.c): what the charge went to since unplugged, by app and by part
 * of the phone, as Android's battery usage. usage_sample.c measures, usage.c models
 * and adds up, Settings › Battery shows it.
 */

#ifndef PHONE_UI_USAGE_H
#define PHONE_UI_USAGE_H

#include <stdbool.h>
#include <time.h>

/* What the current drawn awake is made of: its model's terms, each a value per interval */
enum usage_term {
	USAGE_BASE,   /* 1: the phone awake, screen off, nothing running */
	USAGE_SCREEN, /* the fraction of the interval the screen was on */
	USAGE_BRIGHT, /* the same times its brightness (0 to 1) */
	USAGE_CPU,    /* cores busy on average (0 to 4) */
	USAGE_MOBILE, /* the fraction the modem's radio was active (data moved) */
	USAGE_WIFI,   /* the same for Wi-Fi */
	USAGE_TERMS
};

/* The entries that are not apps (English, N_() in usage.c: translated where shown) */
#define USAGE_IDLE "Idle"                /* USAGE_BASE */
#define USAGE_SCREEN_ON "Screen"         /* USAGE_SCREEN, USAGE_BRIGHT */
#define USAGE_SYSTEM "System"            /* CPU of phone-ui itself, the kernel, daemons */
#define USAGE_MOBILE_NET "Mobile network" /* the radio's cost no app's CPU went with */
#define USAGE_WIFI_NET "Wi-Fi"
#define USAGE_ASLEEP "Asleep"

#define USAGE_USERS 16
#define USAGE_NAME 64

/* Who ran in an interval: an app (its name, as ui_app's) or USAGE_SYSTEM */
struct usage_user {
	const char *name;
	double cpu_s;    /* CPU time, seconds of one core */
	double screen_s; /* on screen, seconds */
};

/* An interval awake on battery */
struct usage_interval {
	double seconds;
	double ma;                /* the current drawn on average, mA; < 0: not measured */
	double x[USAGE_TERMS];    /* x[USAGE_BASE] is 1 */
	struct usage_user users[USAGE_USERS];
	int users_n;
};

struct usage_entry {
	char name[USAGE_NAME];
	double mah;
	double seconds; /* apps: on screen; Screen: on; Asleep: asleep; Idle: awake */
};

/* usage_sample.c: each second from ui_battery_update() (level -1 unknown; the screen on,
 * as ui_battery_screen() was told), and after an app was opened (ui_app_open()) */
void usage_tick(int level, bool plugged, bool screen_on);
void usage_app_opened(const char *name);

/* The model and the totals as saved (once, at startup) */
void usage_load(void);
void usage_save(void);
/* Unplugged: the totals from 0, at that level (-1 unknown) */
void usage_reset(time_t now, int level);
/* An interval awake, its charge to the entries; the model learns from it when measured */
void usage_add(const struct usage_interval *iv);
/* Asleep `seconds`, the charge the level fell meanwhile (mAh, < 0: unknown) for the
 * model of the current asleep */
void usage_asleep(double seconds, double fell_mah);
/* The level now, the period's end */
void usage_level(time_t now, int level);

/* The entries, the most first; valid until the next change */
int usage_entries(const struct usage_entry **entries);
double usage_total_mah(void);
/* The period added up: when it started and the level then, the last level, how long awake
 * and asleep. False before any unplug */
bool usage_period(time_t *start, int *start_level, time_t *end, int *end_level, double *awake_s,
                  double *asleep_s);
/* The model: each term's current at 1 (mA), and asleep's */
double usage_term_ma(enum usage_term term);
double usage_asleep_ma(void);

#endif
