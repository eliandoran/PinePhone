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
 * - Large shapes must be opaque fills (rounded or not) or borders without radius:
 *   those are drawn as clears (LVGL patch 0010). Anything else is a texture as large
 *   as the object, blended over all of it on every frame. List rows get their radius
 *   only in LV_STATE_PRESSED, so that their divider (a border) has none.
 */

#ifndef PHONE_UI_H
#define PHONE_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "lvgl.h"

#define UI_STATUS_H 56  /* status bar, on top of every screen */
#define UI_HEADER_H 96  /* title bar of app screens */

#define UI_COLOR_ACCENT 0x0a84ff /* buttons, highlights */
#define UI_COLOR_ACCENT_PRESSED 0x54a9ff

/* Signal colors, the same in both themes */
#define UI_COLOR_RED 0xff453a /* delete, stop, hang up */
#define UI_COLOR_RED_PRESSED 0xff7d75
#define UI_COLOR_GREEN 0x30d158 /* start, call */
#define UI_COLOR_GREEN_PRESSED 0x6ee38c
#define UI_COLOR_ORANGE 0xff9f0a /* pause, warnings */
#define UI_COLOR_ORANGE_PRESSED 0xffc566

/* Fonts: Latin-1 and the keyboard's symbols (ui_font_*.c, see gen-fonts.sh), the
 * rest from LVGL's Montserrat of the same size. Use these, not lv_font_montserrat_*. */
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_20)
LV_FONT_DECLARE(ui_font_24)
LV_FONT_DECLARE(ui_font_26)
LV_FONT_DECLARE(ui_font_32)
LV_FONT_DECLARE(ui_font_48)

/* Colors of the dark or light theme. Screens take them when they are created:
 * switching themes rebuilds them (ui_set_light()). */
struct ui_colors {
	uint32_t text;           /* primary text */
	uint32_t dim;            /* secondary text */
	uint32_t card;           /* opaque surfaces (list rows, bubbles) */
	uint32_t pressed;        /* pressed rows and cards */
	uint32_t line;           /* between rows */
	uint32_t track;          /* sliders, switches, progress bars */
	uint32_t button;         /* gray buttons */
	uint32_t button_pressed;
	uint32_t bar;            /* bars below the content (text field) */
	uint32_t shade;          /* what screens dim the wallpaper towards */
};
extern const struct ui_colors *ui_colors;
extern bool ui_light;

#define UI_COLOR_TEXT (ui_colors->text)
#define UI_COLOR_DIM (ui_colors->dim)
#define UI_COLOR_CARD (ui_colors->card)
#define UI_COLOR_PRESSED (ui_colors->pressed)
#define UI_COLOR_LINE (ui_colors->line)
#define UI_COLOR_TRACK (ui_colors->track)
#define UI_COLOR_BUTTON (ui_colors->button)
#define UI_COLOR_BUTTON_PRESSED (ui_colors->button_pressed)
#define UI_COLOR_BAR (ui_colors->bar)
#define UI_COLOR_SHADE (ui_colors->shade)

/* Switches to the light or dark theme (saved) and rebuilds every screen in its
 * colors: the home screen and the menu (if it was open), then `reopen` pushes the
 * screens to show on top of them again. Done after the current event: it can be
 * called from a screen it deletes. */
void ui_set_light(bool light, void (*reopen)(void));

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

/* On-screen keyboard for `ta`, hidden: put it under the content, which shrinks
 * above it when shown. Its hide key sends LV_EVENT_CANCEL to `ta`, Enter sends
 * LV_EVENT_READY to a one-line `ta` (a new line otherwise). */
lv_obj_t *ui_keyboard_create(lv_obj_t *parent, lv_obj_t *ta);
void ui_keyboard_show(lv_obj_t *kb, bool show); /* and focuses its text area */

/* Widgets shared by the apps: widgets.c */

/* A label with static text (must stay valid) clipped to one line; set its width
 * or flex grow */
lv_obj_t *ui_one_line_label(lv_obj_t *parent, const char *text, const lv_font_t *font);
/* Dim centered text in place of an empty list ("No notes"), static text */
lv_obj_t *ui_empty_label(lv_obj_t *parent, const char *text);
/* Small dim heading above a card (static text), 16 px below what precedes it */
lv_obj_t *ui_section_label(lv_obj_t *parent, const char *text);
/* Rounded opaque surface for list rows, full width, flex column */
lv_obj_t *ui_card_create(lv_obj_t *parent);

/* A container that only lays out its children: no style, not clickable or scrollable */
lv_obj_t *ui_box(lv_obj_t *parent);

/* A row of a card, `height` high: a flex row with the items centered vertically,
 * with a line below it if `divider` (all but the last row). With a `cb`, clicking
 * it calls cb with `user_data` and it is highlighted while pressed. */
lv_obj_t *ui_row_create(lv_obj_t *card, int32_t height, bool divider, lv_event_cb_t cb, void *user_data);
void ui_row_set_divider(lv_obj_t *row, bool divider); /* rows reused for other items */
/* A column taking the rest of the row, for its lines of text (6 px apart) */
lv_obj_t *ui_row_text(lv_obj_t *row);
/* A line of that column, for a label with flex grow and others after it (time, badge) */
lv_obj_t *ui_row_line(lv_obj_t *text);
/* Secondary text: static, dim (times, previews, values) */
lv_obj_t *ui_dim_label(lv_obj_t *parent, const char *text, const lv_font_t *font);

/* A round avatar `size` across, not clickable: text (initials, a symbol) on a
 * color, or a photo (a round texture, image.c) once there is one. The text's
 * font follows the size; set another on the avatar. */
lv_obj_t *ui_avatar_create(lv_obj_t *parent, int32_t size);
void ui_avatar_set(lv_obj_t *avatar, uint32_t color, const char *text); /* a limited set of texts */
void ui_avatar_set_photo(lv_obj_t *avatar, unsigned tex); /* 0: the text again */

/* A button w x h with fully rounded ends (a circle if square) in `color`, `pressed`
 * while pressed, with a centered label (static text, lv_obj_get_child(button, 0)):
 * white on colors, the text color on grays. cb (if any) gets clicks. */
lv_obj_t *ui_button(lv_obj_t *parent, int32_t w, int32_t h, uint32_t color, uint32_t pressed,
                    const char *text, lv_event_cb_t cb, void *user_data);
void ui_button_set_color(lv_obj_t *button, uint32_t color, uint32_t pressed);

/* A bar below the content, 112 px high: a flex row of buttons or a text field */
lv_obj_t *ui_bar_create(lv_obj_t *parent);
/* A text area without the theme's border, outline and shadow; cb gets CLICKED
 * (show the keyboard), READY and CANCEL (from the keyboard) */
lv_obj_t *ui_textarea_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data);
/* A one-line text area `height` high, rounded on a card color */
lv_obj_t *ui_text_field(lv_obj_t *parent, int32_t height, uint32_t max_length, lv_event_cb_t cb,
                        void *user_data);
/* A bar with a "Message" text field and a send button: returns the text field,
 * for ui_keyboard_create() under the bar */
lv_obj_t *ui_compose_bar(lv_obj_t *parent, uint32_t max_length, lv_event_cb_t ta_cb,
                         lv_event_cb_t send_cb, void *user_data);
/* A hidden bar with Cancel and a red `action` button (static text), e.g. shown
 * by a trash button in the header */
lv_obj_t *ui_confirm_bar(lv_obj_t *parent, const char *action, lv_event_cb_t confirm_cb,
                         lv_event_cb_t cancel_cb, void *user_data);

/* Images: image.c. Decoding gives BGRA pixels (LVGL's texture shader swaps red
 * and blue back), uploaded as GL textures shown by lv_3dtexture. */

/* The scale of a w x h image fitted into bw x bh (cover: filling it) */
float ui_fit_scale(int w, int h, int bw, int bh, bool cover);
/* A JPEG decoded at the smallest scale (n/8) still as large as fitted into
 * (cover: filling) bw x bh, malloc'd; NULL if it cannot be read */
uint8_t *ui_jpeg_read(FILE *f, int bw, int bh, bool cover, int *w, int *h);
/* 0 if there is no GL (no lv_3dtexture either) */
unsigned ui_texture_create(const void *pixels, int w, int h);
void ui_texture_delete(unsigned tex); /* 0: nothing */

/* Today: the time; this week: the day; otherwise the date */
void ui_when_format(char *buf, size_t size, time_t t);
/* In decimal units, as storage is sold: "512 B", "1.5 kB", "29 GB" */
void ui_size_format(char *buf, size_t size, unsigned long long bytes);
/* "UTC", "UTC+2", "UTC-3:30" */
void ui_offset_format(char *buf, size_t size, long seconds);
/* The local time in `tz` (a POSIX TZ string) without leaving it set */
void ui_tz_localtime(const char *tz, time_t t, struct tm *tm);

/* A copy of text (malloc) for labels, with emoji sequences (flags, keycaps, joined
 * emoji) as the single characters the fonts draw them with: emoji.c */
char *ui_emoji_compose(const char *text);

/* Short vibration (if enabled in the settings) */
void ui_vibrate(uint32_t ms);
extern bool ui_haptics; /* vibrate on key presses */

/* Small files (sysfs attributes): the first line without its newline, a number */
bool ui_read_line(const char *path, char *buf, size_t size);
bool ui_read_long(const char *path, long *v);
void ui_write_long(const char *path, long v);

#define UI_BATTERY "/sys/class/power_supply/axp20x-battery"

/* Settings, kept in /var/lib/phone-ui/settings as key=value lines */
const char *ui_setting_get(const char *key, const char *fallback);
int ui_setting_get_int(const char *key, int fallback);
void ui_setting_set(const char *key, const char *value);
void ui_setting_set_int(const char *key, int value);

/* Theme, status bar and wallpaper (ui.c), home screen and menu (home.c) */
void ui_init(lv_display_t *disp);
void ui_status_update(void);
bool ui_battery_present(void); /* false for a dead or absent cell, even if the PMIC says present */
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
void calendar_open(void);   /* calendar.c: month view */
void camera_open(void);     /* camera.c */
void gallery_open(void);    /* gallery.c: photos from the camera */
void notes_open(void);      /* notes.c */
void files_open(void);      /* files.c: file system browser */
void music_open(void);      /* music.c: made-up library, nothing is played */
void maps_open(void);       /* maps.c: OpenStreetMap */
void weather_open(void);    /* weather.c: Open-Meteo forecast */
void telegram_open(void);   /* telegram/: one file per part, see telegram.h */
void snake_open(void);      /* snake.c */

#endif
