/*
 * Settings: a list of sections, each a screen showing what the phone reports
 * (sysfs, procfs, statvfs...). settings_apply() restores the saved settings at
 * startup, before LVGL runs. Shared between its files:
 *
 * - settings.c     the list, settings_apply(), the building blocks below
 * - display.c      brightness (ui_brightness_*()), theme, tap to wake, screen
 * - sound.c        vibration
 * - datetime.c     the clock and the time zone
 * - language.c     the language (i18n.h: the texts of Settings are translated)
 * - battery.c      charge, the last day, details with the thermistor's temperature
 * - wifi.c         networks around, connecting (wpa_supplicant's control interface)
 * - bluetooth.c    on and off, devices paired and nearby, pairing (BlueZ: ../bluetooth.c)
 * - network.c      interfaces and their addresses
 * - storage.c      the root and data file systems
 * - emmc_install.c the system on a microSD card installed on the eMMC, with its data
 * - about.c        the phone number and IMEI, software, hardware, temperatures
 * - mobile.c       their Mobile section, also on Network with the network's name
 * - developer.c    Developer mode: the frame rate counter, the computers allowed over USB
 *                  (pairing.c), the speed test, the accelerometer as read and
 *                  as taken, what a game controller presses (ui_gamepad_*())
 *
 * Each section has an *_open() that pushes its screen and, for the list, a *_summary()
 * that sets the value shown next to its name.
 */

#ifndef PHONE_UI_SETTINGS_H
#define PHONE_UI_SETTINGS_H

#include <stddef.h>

#include "i18n.h"
#include "ui.h"

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

/* The Mobile section (mobile.c): with `network` the network's name ("Not available"
 * without a modem) and the mobile data switch (when the board has it), the phone
 * number and IMEI with a modem; none of it, NULL rows. Update it from the page's
 * timer, as the modem answers (and the shade changes data) */
struct settings_mobile {
	lv_obj_t *network, *data, *number, *imei;
	lv_obj_t *apn, *kb; /* the APN's field (Network, with mobile data) */
};
void settings_mobile_create(lv_obj_t *page, struct settings_mobile *m, bool network);
void settings_mobile_update(const struct settings_mobile *m);

void display_open(void);
void display_summary(lv_obj_t *value);
void backlight_dark(void); /* brightness 0, the backlight off while asleep (power.c) */
void tap_wake_apply(void); /* the touchscreen wakes the phone or not, as set */
void sound_open(void);
void sound_summary(lv_obj_t *value);
void datetime_open(void);
void datetime_summary(lv_obj_t *value);
void datetime_apply(void); /* the saved time zone */
void language_open(void);
void language_summary(lv_obj_t *value);
void wifi_apply(void); /* Wi-Fi off if it was turned off */
void wifi_sleep(bool asleep); /* wlan0 down while the phone sleeps (power.c), if on */
void bluetooth_summary(lv_obj_t *value);
void battery_open(void);
void battery_summary(lv_obj_t *value);
bool battery_temperature(long *c); /* °C, from its thermistor (About phone) */
void network_open(void);
void network_summary(lv_obj_t *value);
void storage_open(void);
void storage_summary(lv_obj_t *value);
/* Running from a microSD card: installing it on the eMMC (emmc_install.c) */
bool emmc_install_offered(void);
bool emmc_install_running(void); /* Power does not sleep meanwhile (power.c) */
void emmc_install_open(void);
void about_open(void);
void about_summary(lv_obj_t *value);
void developer_open(void);

#endif
