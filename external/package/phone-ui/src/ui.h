/*
 * Shared pieces of the phone UI: screens and navigation, the status bar, labels
 * with cached text, app icons, vibration, settings.
 *
 * Drawing goes through LVGL's OpenGL ES draw unit, which renders each widget part
 * on the CPU once and caches it as a texture. What it cannot cache is rendered
 * and uploaded again on every frame, so:
 * - Labels with fixed text: lv_label_set_text_static() with a string that stays
 *   valid (literal, static data).
 * - Labels whose text changes but comes from a limited set (time, numbers,
 *   names): ui_set_text(), which keeps one copy of each distinct text.
 * - Text that changes on every frame (stopwatch) or is typed: lv_label_set_text().
 * - No LV_LABEL_LONG_MODE_DOTS with static text (it writes the dots into the
 *   text), use LV_LABEL_LONG_MODE_CLIP or wrap.
 * - No lv_list_add_*() / lv_msgbox (they copy their texts); build lists from
 *   objects and static labels.
 * - No opa, transform or blend mode styles on containers (they render the whole
 *   container into a separate layer), no shadows.
 */

#ifndef PHONE_UI_H
#define PHONE_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define UI_STATUS_H 56  /* status bar, on top of every screen */
#define UI_HEADER_H 96  /* title bar of app screens */

#define UI_COLOR_DIM 0xa0a0aa    /* secondary text */
#define UI_COLOR_ACCENT 0x0a84ff /* buttons, highlights */
#define UI_COLOR_CARD 0x2c2c34   /* opaque surfaces (list rows, bubbles) */

/* Duration of screen transitions */
extern uint32_t ui_anim_ms;

/* Screens and navigation. The home screen and the menu always exist; other screens
 * are pushed on top and deleted when they are left. */

/* A screen with the wallpaper dimmed by `dim` (LV_OPA_TRANSP to LV_OPA_COVER).
 * Returns the screen; *content is a full-screen container below the status bar. */
lv_obj_t *ui_screen_create(lv_opa_t dim, lv_obj_t **content);

/* An app screen: dimmed wallpaper, a header with a back button and `title` (kept
 * as a static text: it must stay valid), and *body, a container that fills the
 * rest (flex column, no padding). *header may be NULL; actions can be added to
 * it with ui_header_button(). */
lv_obj_t *ui_app_screen_create(const char *title, lv_obj_t **header, lv_obj_t **body);

/* A button with a symbol on the right side of a header */
lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, lv_event_cb_t cb, void *user_data);

void ui_push(lv_obj_t *screen); /* slides in from the right */
void ui_pop(void);              /* back to the previous screen, deletes this one */
void ui_home(void);             /* back to the home screen, deletes pushed screens */
void ui_open_menu(void);
lv_obj_t *ui_current(void);     /* the screen on top */

/* Labels with text from a limited set of values, see above */
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(2, 3);
const char *ui_intern(const char *text); /* the copy ui_set_text() uses, never freed */

/* Apps */
struct ui_app {
	const char *name;
	const char *symbol;
	uint32_t color;
	void (*open)(void); /* pushes the app's first screen; NULL: not done yet */
};
extern const struct ui_app ui_apps[];
extern const unsigned ui_app_count;
const struct ui_app *ui_app_find(const char *name);
void ui_app_open(const struct ui_app *app);

/* Rounded colored square with the app's symbol */
lv_obj_t *ui_app_icon_create(lv_obj_t *parent, const struct ui_app *app, int32_t size);

/* Short vibration (if enabled in the settings) */
void ui_vibrate(uint32_t ms);
extern bool ui_haptics; /* vibrate on key presses */

/* Settings, kept in /var/lib/phone-ui/settings as key=value lines */
const char *ui_setting_get(const char *key, const char *fallback);
int ui_setting_get_int(const char *key, int fallback);
void ui_setting_set(const char *key, const char *value);
void ui_setting_set_int(const char *key, int value);

/* Status bar and wallpaper (ui.c), home screen and menu (home.c) */
void ui_init(lv_display_t *disp);
void ui_status_update(void);
void home_create(void);
void ui_set_screens(lv_obj_t *home, lv_obj_t *menu); /* home.c, once */

/* Apps (one file each) */
void phone_open(void);      /* dialer: phone.c */
void phone_dial(const char *number); /* dialer with a number entered (from contacts) */
void contacts_open(void);   /* contacts.c */
void messages_open(void);   /* messages.c */
void messages_open_thread(const char *number); /* conversation with a number */
void settings_open(void);   /* settings.c */
void settings_apply(void);  /* at startup: brightness, time zone, haptics */
void calculator_open(void); /* calculator.c */
void clock_open(void);      /* clock.c: alarm, stopwatch, timer */
void camera_open(void);     /* camera.c */

#endif
