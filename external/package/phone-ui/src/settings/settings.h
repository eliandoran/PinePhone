/*
 * Settings: a list of sections, each a screen showing what the phone reports
 * (sysfs, procfs, statvfs...). settings_apply() restores the saved settings at
 * startup, before LVGL runs. Shared between its files:
 *
 * - settings.c     the list, settings_apply(), the building blocks below
 * - display.c      brightness (ui_brightness_*()), theme, screen
 * - sound.c        vibration
 * - datetime.c     the clock and the time zone
 * - battery.c      charge, details, the thermistor's temperature and the charger's cut-offs
 * - network.c      interfaces and their addresses
 * - storage.c      the root and data file systems
 * - about.c        software, hardware, temperatures
 * - diagnostics.c  the accelerometer as read and as taken
 *
 * Each section has an *_open() that pushes its screen and, for the list, a *_summary()
 * that sets the value shown next to its name.
 */

#ifndef PHONE_UI_SETTINGS_H
#define PHONE_UI_SETTINGS_H

#include <stddef.h>

#include "../ui.h"

/* Building blocks: a scrolling page of cards, cards holding rows separated by lines,
 * rows with a title on the left and a value on the right (settings.c) */
lv_obj_t *settings_page(const char *title, lv_obj_t **page); /* returns the screen */
lv_obj_t *settings_panel(lv_obj_t *parent); /* a card with its content padded, not rows */
lv_obj_t *settings_row(lv_obj_t *card);
void settings_row_tappable(lv_obj_t *row, lv_event_cb_t cb, void *user_data);
lv_obj_t *settings_value(lv_obj_t *row); /* the dim label on the right */
lv_obj_t *settings_info_row(lv_obj_t *card, const char *title); /* returns its value */
lv_obj_t *settings_title_line(lv_obj_t *panel, const char *title); /* in a panel; its value */
lv_obj_t *settings_bar(lv_obj_t *panel, int32_t max, uint32_t color);
/* A timer that lives as long as `scr` and runs once right away */
void settings_timer(lv_obj_t *scr, lv_timer_cb_t cb, uint32_t period);

/* The first entry of a sysfs directory whose real path contains `match` (any if NULL),
 * searched once: NULL when there is none */
struct settings_device {
	char path[300];
	bool searched;
};
const char *settings_device(struct settings_device *d, const char *dir, const char *match);
const char *settings_pmic_adc(void); /* the AXP803's IIO ADC */
/* An attribute of a sysfs device (false for a NULL device) */
bool settings_attr_long(const char *dev, const char *attr, long *v);
bool settings_attr_double(const char *dev, const char *attr, double *v);

void display_open(void);
void display_summary(lv_obj_t *value);
void sound_open(void);
void sound_summary(lv_obj_t *value);
void datetime_open(void);
void datetime_summary(lv_obj_t *value);
void datetime_apply(void); /* the saved time zone */
void battery_open(void);
void battery_summary(lv_obj_t *value);
void network_open(void);
void network_summary(lv_obj_t *value);
void storage_open(void);
void storage_summary(lv_obj_t *value);
void about_open(void);
void about_summary(lv_obj_t *value);
void diagnostics_open(void);

#endif
