/*
 * Clock: local time with world clocks, a stopwatch and a countdown timer, on
 * pages switched by a segmented control. Shared between its files:
 *
 * - clock.c      the app's screen: the segmented control, the pages, layout helpers
 * - world.c      local time and the world clocks
 * - stopwatch.c  the stopwatch and its laps
 * - timer.c      the countdown timer and its alarm
 * - digits.c     fixed-width running digits
 *
 * Each page keeps its own state, from its *_create() until the screen is deleted
 * (its *_delete()).
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

/* stopwatch.c */
void stopwatch_create(lv_obj_t *page);
void stopwatch_delete(void);

/* timer.c */
void countdown_create(lv_obj_t *page); /* timer_create() is POSIX's */
void countdown_delete(void);

#endif
