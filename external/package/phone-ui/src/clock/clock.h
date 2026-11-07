/*
 * Clock: local time with world clocks, an alarm, a stopwatch and a countdown timer, on
 * pages switched by a navigation bar. Shared between its files:
 *
 * - clock.c      the app's screen: the navigation bar, the pages, layout helpers
 * - world.c      local time and the world clocks
 * - alarm.c      the alarm, kept, set again at startup (clock_start())
 * - stopwatch.c  the stopwatch and its laps
 * - timer.c      the countdown timer and its alarm
 * - digits.c     fixed-width running digits
 *
 * Each page keeps its own state, from its *_create() until the screen is deleted
 * (its *_delete()); the alarm and the timer run on without it.
 */

#ifndef PHONE_UI_CLOCK_H
#define PHONE_UI_CLOCK_H

#include <stdint.h>

#include "../i18n.h"
#include "../ui.h"

/* clock.c */
uint64_t clock_now_ms(void); /* monotonic */
lv_obj_t *clock_page_create(lv_obj_t *parent); /* a flex column, children centered */
lv_obj_t *clock_spacer(lv_obj_t *parent, int32_t height);
lv_obj_t *clock_button_row(lv_obj_t *parent); /* full width, children spread evenly */
/* A ui_button()'s static text and colors */
void clock_button_set(lv_obj_t *button, const char *text, uint32_t color, uint32_t pressed);
/* A round button of the row at the bottom of a page (Start, Lap, Cancel) */
lv_obj_t *clock_round_button(lv_obj_t *row, const char *text, uint32_t color, uint32_t pressed, lv_event_cb_t cb);
/* The large digits of `pattern` near the top of a page, and under them a box of a fixed
 * height for what comes and goes (the timer's bar, the alarm's next ring) */
struct digits;
lv_obj_t *clock_display_create(lv_obj_t *page, struct digits *d, const char *pattern);
/* A column of rows, centered (what sets the timer, the alarm) */
lv_obj_t *clock_setup_create(lv_obj_t *page);
/* A dim `title` over - and + buttons, which call cb with user data `step` and -`step`
 * (as an intptr_t) when pressed, and repeatedly while held */
void clock_adjuster_create(lv_obj_t *parent, const char *title, lv_event_cb_t cb, int step);
/* Clock opened on a page */
enum clock_page { CLOCK_WORLD, CLOCK_ALARM, CLOCK_STOPWATCH, CLOCK_TIMER, CLOCK_PAGES };
void clock_open_page(enum clock_page page);

/* digits.c: one label per character of a pattern like "00:00.00", each set to a
 * static string. The GPU draw unit keeps a texture per digit instead of rendering
 * the whole text again on every frame, and the digits do not shift sideways as
 * they change (the font is not monospaced). */
struct digits {
	lv_obj_t *cells[8];
	unsigned count;
};
void digits_create(struct digits *d, lv_obj_t *parent, const char *pattern);
void digits_set(struct digits *d, const char *text); /* the digits of a text like the pattern */

/* world.c */
void world_create(lv_obj_t *page);
void world_delete(void);

/* alarm.c */
void alarm_page_create(lv_obj_t *page);
void alarm_page_delete(void);

/* stopwatch.c */
void stopwatch_create(lv_obj_t *page);
void stopwatch_delete(void);

/* timer.c */
void countdown_create(lv_obj_t *page); /* timer_create() is POSIX's */
void countdown_delete(void);

#endif
