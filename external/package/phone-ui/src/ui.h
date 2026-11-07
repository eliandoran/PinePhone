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

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "lvgl.h"

#define UI_STATUS_H 56  /* status bar, on top of every screen */
#define UI_HEADER_H 96  /* title bar of app screens */
#define UI_HOME_EDGE_H 24 /* the strip along the bottom edge for the home gesture */

#define UI_COLOR_ACCENT 0x0a84ff /* buttons, highlights */
#define UI_COLOR_ACCENT_PRESSED 0x54a9ff

/* Signal colors, the same in both themes */
#define UI_COLOR_RED 0xff453a /* delete, stop, hang up */
#define UI_COLOR_RED_PRESSED 0xff7d75
#define UI_COLOR_GREEN 0x30d158 /* start, call */
#define UI_COLOR_GREEN_PRESSED 0x6ee38c
#define UI_COLOR_ORANGE 0xff9f0a /* pause, warnings */
#define UI_COLOR_ORANGE_PRESSED 0xffc566

/* Fonts: Inter with Latin-1 and the keyboard's symbols (ui_font_*.c, see gen-fonts.sh),
 * LVGL's symbols from its Montserrat of the same size. Use these, not lv_font_montserrat_*. */
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_20)
LV_FONT_DECLARE(ui_font_24)
LV_FONT_DECLARE(ui_font_26)
LV_FONT_DECLARE(ui_font_32)
LV_FONT_DECLARE(ui_font_48)
/* Monospace, for code and terminals: DejaVu Sans Mono, 12 px a character, 25 px lines
 * (ASCII, Latin-1, box drawing; the rest from ui_font_20, which does not line up) */
LV_FONT_DECLARE(ui_font_mono_20)

/* Text takes the font of its role, as iOS's and Material's text styles: the display has
 * 270 pixels per inch, 1.7 times Android's 160 dpi, so 20 px (12 sp) is the smallest
 * text read. A symbol sized to its box (a button's, the dial pad's keys, an avatar's
 * initials) keeps the font of its size, ui_font_<px>. */
#define UI_FONT_CAPTION (&ui_font_20)    /* times, counts, section headings, notes under a value */
#define UI_FONT_BODY (&ui_font_24)       /* the theme's: previews, wrapped text, errors, captions */
#define UI_FONT_BODY_LARGE (&ui_font_26) /* text read at length: a note */
#define UI_FONT_TITLE (&ui_font_26)      /* a row's or a card's title, a button's text */
#define UI_FONT_HEADLINE (&ui_font_32)   /* a screen's title, a value read at a glance */
#define UI_FONT_DISPLAY (&ui_font_48)    /* a name or number shown big (a contact, a clock) */

/* Colors of the dark or light theme. Screens take them when they are created:
 * switching themes rebuilds them (ui_set_light()). */
struct ui_colors {
	uint32_t text;           /* primary text */
	uint32_t dim;            /* secondary text */
	uint32_t card;           /* opaque surfaces (list rows, bubbles) */
	uint32_t pressed;        /* pressed rows and cards */
	uint32_t line;           /* between rows */
	uint32_t track;          /* sliders, switches, progress bars */
	uint32_t knob_edge;      /* around white knobs: shows them on white */
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
#define UI_COLOR_KNOB_EDGE (ui_colors->knob_edge)
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
/* The wallpaper, screen-sized, dimmed by `dim`: an image source that stays valid */
const lv_draw_buf_t *ui_wallpaper(lv_opa_t dim);

/* An app screen: dimmed wallpaper, a header with a back button and `title` (kept
 * as a static text: it must stay valid), and *body, a container that fills the
 * rest (flex column, no padding). *header may be NULL; actions can be added to
 * it with ui_header_button(). */
lv_obj_t *ui_app_screen_create(const char *title, lv_obj_t **header, lv_obj_t **body);

/* A button with a symbol on the right side of a header */
lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, lv_event_cb_t cb, void *user_data);

void ui_push(lv_obj_t *screen); /* slides in from the right */
void ui_push_now(lv_obj_t *screen); /* shown at once, no slide: an incoming call */
void ui_pop(void);              /* back to the previous screen, deletes this one */
void ui_home(void);             /* back to the home screen, deletes pushed screens */
void ui_open_menu(void);
/* Dragging the menu up from the home screen (`open`) or down from the menu: begin
 * (false when it cannot: a transition runs, no GPU, not on that screen), then how far
 * it went in px, then let go: `done`, or back to where it was */
bool ui_menu_drag_begin(bool open);
void ui_menu_drag(int32_t pos);
void ui_menu_drag_end(bool done);
lv_obj_t *ui_current(void);     /* the screen on top */

/* Labels with text from a limited set of values, see above */
void ui_set_text(lv_obj_t *label, const char *text);
void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(2, 3);
const char *ui_intern(const char *text); /* the copy ui_set_text() uses, never freed */

/* Apps: the built-in ones (home.c), then the external ones (external.c) */
/* The menu's sections, in their order; hidden apps have no tile (--open finds them) */
enum ui_category { UI_ESSENTIALS, UI_TOOLS, UI_MEDIA, UI_GAMES, UI_CATEGORIES, UI_HIDDEN = UI_CATEGORIES };

struct ui_app {
	const char *name;
	const char *symbol;
	uint32_t color;
	void (*open)(void); /* pushes the app's first screen; NULL: not done yet */
	enum ui_category category;
	const char *dir;    /* external app: its folder (open is NULL unless a plugin) */
	unsigned icon;      /* external app: its icon.png as a texture, 0: none */
};
extern const struct ui_app *ui_apps;
extern unsigned ui_app_count;
const struct ui_app *ui_app_find(const char *name);
void ui_app_open(const struct ui_app *app);

/* A plugin app's notification (notify.c), with Messages' and Phone's: listed on the lock
 * screen and in the quick settings (the app's icon, `title`, `text`, `time`, `count` over
 * one), counted on the app's icon, a bell in the status bar. One per `key` of the app (a
 * chat's id): posted again, it changes; `count` 0 withdraws it. `alert`: something new came,
 * a banner for a few seconds and the vibrator (unless the quick settings are open). Tapped,
 * the phone unlocked first, `open(key)` is called. `app` is its name, as in app.json.
 * Cleared from the quick settings they go, until posted again. */
void ui_notify(const char *app, const char *key, const char *title, const char *text,
               unsigned count, time_t time, bool alert, void (*open)(const char *key));

/* Alarms (alarms.c): fire(key, data) in the UI loop once a time comes, one per `app` and
 * `key` (set again, it replaces it). At a local date and time (`local`'s year to minute,
 * worked out again at each check: the clock or the time zone changed, still that time),
 * or `ms` from now, counted asleep too. Due already, it fires at the next check (a second).
 * Not kept across restarts: set them again at startup. Asleep, the phone wakes for them. */
typedef void (*ui_alarm_fn)(const char *key, void *data);
void ui_alarm_at(const char *app, const char *key, const struct tm *local, ui_alarm_fn fire, void *data);
void ui_alarm_in(const char *app, const char *key, uint32_t ms, ui_alarm_fn fire, void *data);
void ui_alarm_cancel(const char *app, const char *key);

/* Built-in apps' icons, PNGs at the sizes they are drawn: icons.c, generated by
 * gen-icons.sh from icons/<app>.svg */
struct ui_icon {
	const char *name; /* the app's, in lower case */
	int32_t size;
	const uint8_t *png;
	unsigned png_size;
};
extern const struct ui_icon ui_icons[];
extern const unsigned ui_icon_count;

/* Rounded square: the app's icon (icons.c, or an external app's icon.png), or
 * its symbol on its color */
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
/* A slider taking the rest of a flex row: accent indicator, round white knob */
lv_obj_t *ui_slider_create(lv_obj_t *parent);
/* An on/off switch: green when on, white knob */
lv_obj_t *ui_switch_create(lv_obj_t *parent, bool on);

/* A container that only lays out its children: no style, not clickable or scrollable */
lv_obj_t *ui_box(lv_obj_t *parent);

/* Grids of tiles (Books' covers, Doom's games): cards with a picture from edge to
 * edge at their top, their text below */
#define UI_TILE_GAP 16 /* around the grid and between its tiles */
#define UI_TILE_RADIUS 20
/* The grid, taking the rest of a flex column and scrolling */
lv_obj_t *ui_tile_grid_create(lv_obj_t *parent);
/* How wide tiles are, `columns` of them across the display */
int32_t ui_tile_width(int columns);
/* A tile `w` wide in the grid, its picture `pic_h` high: *pic is an lv_3dtexture
 * (its source a ui_tile_texture()), *text a column below it for the labels. With a
 * `cb`, clicking it calls cb with `user_data` and it is highlighted while pressed. */
lv_obj_t *ui_tile_create(lv_obj_t *grid, int32_t w, int32_t pic_h, lv_event_cb_t cb, void *user_data,
                         lv_obj_t **pic, lv_obj_t **text);
/* Its size again (the display turned) */
void ui_tile_set_size(lv_obj_t *tile, int32_t w, int32_t pic_h);

/* A row of a card, `height` high: a flex row with the items centered vertically,
 * with a line below it if `divider` (all but the last row). With a `cb`, clicking
 * it calls cb with `user_data` and it is highlighted while pressed. */
lv_obj_t *ui_row_create(lv_obj_t *card, int32_t height, bool divider, lv_event_cb_t cb, void *user_data);
void ui_row_set_divider(lv_obj_t *row, bool divider); /* rows reused for other items */
/* `row` (clickable, in a list scrolled up and down) swiped to the left: it follows the
 * finger once the drag goes sideways, uncovering `symbol` on `color`; let go past a
 * third of its width or flicked, it slides out and closes up, then `done(row,
 * user_data)` is called, which deletes it (or the list again without it); else it
 * comes back. A swipe is no click, and up and down the list scrolls as before. */
void ui_swipe_away(lv_obj_t *row, const char *symbol, uint32_t color, void (*done)(lv_obj_t *row, void *user_data),
                   void *user_data);
/* `list` (scrolled up and down) pulled down from its top, even when it has too little
 * to scroll: a sign comes down with it, turning to the accent color once far enough;
 * let go there, `refresh(user_data)` is called (once the release is over: it may
 * fill the list again at once) and the sign stays at the top until
 * ui_pull_refresh_done(list) (or 15 s). Pulled again meanwhile, nothing more. */
void ui_pull_refresh(lv_obj_t *list, void (*refresh)(void *user_data), void *user_data);
void ui_pull_refresh_done(lv_obj_t *list);
/* A column taking the rest of the row, for its lines of text (6 px apart) */
lv_obj_t *ui_row_text(lv_obj_t *row);
/* A line of that column, for a label with flex grow and others after it (time, badge) */
lv_obj_t *ui_row_line(lv_obj_t *text);
/* Secondary text: static, dim (times, previews, values) */
lv_obj_t *ui_dim_label(lv_obj_t *parent, const char *text, const lv_font_t *font);
/* A row's text (Messages' conversations, notifications, recent calls): `title` and its
 * `time` (dim, smaller) on one line, `preview` (dim, one line) under them; static
 * texts. The preview's line takes more after it (lv_obj_get_parent(): a badge). */
struct ui_row_lines {
	lv_obj_t *title, *time, *preview;
};
struct ui_row_lines ui_row_lines(lv_obj_t *row, const char *title, const char *time, const char *preview);
/* How many (unread, missed) in a blue pill after a row's preview */
lv_obj_t *ui_count_badge(lv_obj_t *parent, unsigned count);

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
/* A circle `size` across with a symbol (larger from 120 px) */
lv_obj_t *ui_round_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                          const char *symbol, lv_event_cb_t cb, void *user_data);
/* That with a dim `caption` (static text) under it, in a column 160 wide (call
 * screen's options, a contact's actions): returns the button */
lv_obj_t *ui_captioned_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                              const char *symbol, const char *caption, lv_event_cb_t cb, void *user_data);
/* A list screen's main action (a new message, a new contact): a round button in the
 * accent color, the send button's larger, floating at the bottom right in reach of
 * the thumb over `body` (an app screen's). A scrolling body's end scrolls clear of it;
 * in one that does not scroll, what does (a tile grid) takes UI_ACTION_CLEAR at its end. */
#define UI_ACTION_CLEAR (UI_HOME_EDGE_H + 32 + 104 + 16)
lv_obj_t *ui_action_button(lv_obj_t *body, const char *symbol, lv_event_cb_t cb, void *user_data);

/* A bar below the content, 112 px high: a flex row of buttons or a text field */
lv_obj_t *ui_bar_create(lv_obj_t *parent);
/* A tab of a navigation bar: its caption (translated, static text) and its icon, a
 * symbol (LV_SYMBOL_*, UI_SYMBOL_*) */
struct ui_nav_tab {
	const char *name;
	const char *symbol;
};
/* The navigation bar at the bottom of an app (Phone, Telegram), 128 px high: an icon
 * `icon_size` px (32 or 48) and a caption per tab, the selected one in the accent color.
 * cb gets each tab's clicks with user_data, ui_nav_bar_index(e) which one it was. */
lv_obj_t *ui_nav_bar_create(lv_obj_t *parent, const struct ui_nav_tab *tabs, unsigned n,
                            int32_t icon_size, lv_event_cb_t cb, void *user_data);
void ui_nav_bar_select(lv_obj_t *bar, unsigned index);
unsigned ui_nav_bar_index(lv_event_t *e);
/* A text area without the theme's border, outline and shadow; cb gets CLICKED
 * (show the keyboard), READY and CANCEL (from the keyboard) */
lv_obj_t *ui_textarea_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data);
/* A one-line text area `height` high, rounded on a card color */
lv_obj_t *ui_text_field(lv_obj_t *parent, int32_t height, uint32_t max_length, lv_event_cb_t cb,
                        void *user_data);
/* Prose (a note, a message): the keyboard offers corrections of the word being typed,
 * one of which a space or punctuation applies. Not for names, addresses, codes. */
void ui_textarea_set_prose(lv_obj_t *ta);
/* Enter sends (READY) in a text area of several lines too, as in one of one line */
void ui_textarea_set_enter_sends(lv_obj_t *ta);
/* For phone numbers: the keyboard is a dial pad, and only what numbers are written
 * with can be typed */
void ui_textarea_set_phone(lv_obj_t *ta);
/* A bar with a "Message" text field and a send button: returns the text field,
 * for ui_keyboard_create() under the bar. The text wraps, the field growing up to
 * COMPOSE_LINES lines, then scrolling; Enter sends. */
#define COMPOSE_LINES 5
lv_obj_t *ui_compose_bar(lv_obj_t *parent, uint32_t max_length, lv_event_cb_t ta_cb,
                         lv_event_cb_t send_cb, void *user_data);
/* A conversation (Messages, Telegram, Assistant) in a screen's body: `list`, a
 * scrolling column of message groups, above a compose bar and its keyboard. The
 * struct must outlive the screen. Sending hides the keyboard and clears the field,
 * then calls send() with the text, trimmed (not called if empty). */
struct ui_chat {
	lv_obj_t *list, *ta, *kb;
	void (*send)(struct ui_chat *chat, const char *text);
	void *user_data;
	/* Replies: a message swiped left (ui_chat_replyable()) calls reply() with its id,
	 * which answers with ui_chat_reply(); send() then finds the message answered in
	 * reply_to (0: none), cleared once it returns */
	void (*reply)(struct ui_chat *chat, long long id);
	long long reply_to;
	lv_obj_t *reply_bar, *reply_name, *reply_text;
};
void ui_chat_create(struct ui_chat *chat, lv_obj_t *body, uint32_t max_length);
void ui_chat_scroll_end(struct ui_chat *chat, lv_anim_enable_t anim);
/* Messages going one way, on that side of the list, ending with *time: a dim label
 * (empty static text) for their time and state */
lv_obj_t *ui_chat_group_create(struct ui_chat *chat, bool outgoing, lv_obj_t **time);
/* A bubble at the end of a group (before its time): returns its label, empty
 * static text, wrapping at 75% of the screen */
lv_obj_t *ui_chat_bubble_create(lv_obj_t *group, bool outgoing);
/* A message's object (its bubble, a photo) swiped left calls chat->reply(chat, id);
 * a swipe is not a click */
void ui_chat_replyable(struct ui_chat *chat, lv_obj_t *obj, long long id);
/* Replying to message `id` (0: not replying): a bar above the field with who wrote it
 * and its text on one line, a button to cancel; the keyboard shown */
void ui_chat_reply(struct ui_chat *chat, long long id, const char *name, const char *text);
/* At the top of the bubble of `label`, the message it answers: who wrote it and its
 * text on one line, after a bar. Returns it, to make it tappable (to that message). */
lv_obj_t *ui_chat_quote_create(lv_obj_t *label, bool outgoing, const char *name, const char *text);

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
/* The same for a JPEG or binary PPM file (told apart by content), with every
 * n-th pixel of a PPM; logs when it cannot be read */
uint8_t *ui_image_read(const char *path, int bw, int bh, bool cover, int *w, int *h);
/* BGRX pixels into f, row y at px + y * stride (negative: bottom row first);
 * full_color: chroma not subsampled (sharp colored text); false on error */
bool ui_jpeg_write(FILE *f, const uint8_t *px, int w, int h, long stride, int quality, bool full_color);
/* 0 if there is no GL (no lv_3dtexture either) */
unsigned ui_texture_create(const void *pixels, int w, int h);
void ui_texture_delete(unsigned tex); /* 0: nothing */
/* A tile's picture (ui_tile_create()): w x h BGRA pixels, or all `color` (0xRRGGBB)
 * if px is NULL, shown `shown_w` wide, with the tile's rounded corners cut out of
 * its top (transparent: a texture has no radius). px is changed. */
unsigned ui_tile_texture(uint8_t *px, int w, int h, int32_t shown_w, uint32_t color);

/* The Gallery's photos (JPEG or PPM) as a grid titled `title` (static text) to
 * choose one from: once one is selected and confirmed, the grid goes back to the
 * screen below and `picked` gets the photo's path. Nothing if none is chosen. */
void ui_photo_pick(const char *title, void (*picked)(const char *path, void *user_data), void *user_data);

/* Photo viewer (viewer.c): the photos at `paths` (copied), JPEG or PPM, from
 * `index`, swiped through, with "n of N" under them when there are several.
 * `title` is static text. ops may be NULL: caption gives a photo's text under it;
 * with remove, a trash button deletes the photo shown after a confirmation
 * (false: it could not), and the viewer goes back once none are left. */
struct ui_photo_view {
	void (*caption)(int index, char *buf, size_t size, void *user_data);
	bool (*remove)(int index, void *user_data);
	void *user_data;
};
void ui_photo_view(const char *title, const char *const *paths, int count, int index,
                   const struct ui_photo_view *ops);
/* The open viewer's photo at old_path from new_path instead (a larger copy) */
void ui_photo_view_replace(const char *old_path, const char *new_path);

/* Dates as day numbers, days since 1970-01-01 in no time zone (month.c) */
long ui_days_from_civil(int year, int month, int day); /* month 0-11 */
void ui_civil_from_days(long days, struct tm *tm);       /* its tm_year, tm_mon, tm_mday, tm_wday */
int ui_weekday(long days);                               /* 0 for Monday */
long ui_today(void);                                     /* in the local time zone */
/* A month grid, full width: the month between arrows, then its days from Monday, today
 * in the accent color, `selected` on a gray (-1: none, today's month); swiped sideways
 * or the arrows browse. Tapping a day selects it (a day of another month shows that
 * month) and calls selected_cb(day, user_data). */
lv_obj_t *ui_month_create(lv_obj_t *parent, long selected, void (*selected_cb)(long day, void *user_data),
                          void *user_data);
void ui_month_select(lv_obj_t *month, long day); /* and shows its month */
void ui_month_set_today(lv_obj_t *month, long today); /* after midnight */

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
/* Looks again for the emoji images that were missing, and lays out the labels again: the
 * browser fetches them once drawn (emoji.c) */
void ui_emoji_reload(void);
/* prefix then text on one line (whitespace runs as one space, emoji composed), at
 * most max bytes of whole characters, malloc'd (widgets.c): list previews */
char *ui_one_line(const char *prefix, const char *text, size_t max);

/* Short vibration (if enabled in the settings) */
#define UI_VIBRATE_TAP 60 /* ms: a tap or a key, barely felt (shorter is not felt) */
void ui_vibrate(uint32_t ms);
extern bool ui_haptics; /* vibrate on key presses */

/* HTTP requests (fetch.c), one at a time per struct (zeroed: idle), polled by a
 * timer. done() gets what came (NUL-terminated, NULL if nothing; its size in
 * `size` while done() runs: binary data) and the HTTP status, 0 without a
 * connection; the struct is idle again by then. */
struct ui_fetch {
	void *multi, *curl, *headers; /* libcurl's; curl NULL: idle */
	char *data;                   /* received so far */
	size_t size;
	lv_timer_t *timer;
	void (*progress)(const char *data, size_t size);
	void (*done)(const char *data, long status);
};
/* GET, at most 30 s */
void ui_fetch_start(struct ui_fetch *f, const char *url, void (*done)(const char *data, long status));
/* POST of body with headers (NULL-terminated, may be NULL), no time limit while
 * data keeps coming; progress (may be NULL) gets everything received so far
 * whenever more comes, for streamed answers */
void ui_fetch_post(struct ui_fetch *f, const char *url, const char *const *headers, const char *body,
                   void (*progress)(const char *data, size_t size),
                   void (*done)(const char *data, long status));
/* Any method ("GET", "PATCH", "PUT"...) with headers (NULL-terminated, may be NULL)
 * and a body (NULL: none), at most 30 s: web APIs */
void ui_fetch_request(struct ui_fetch *f, const char *method, const char *url, const char *const *headers,
                      const char *body, void (*done)(const char *data, long status));
void ui_fetch_stop(struct ui_fetch *f); /* cancels it, done() is not called */
/* A command on a mail server (Email): libcurl's IMAP. `url` is imaps://host:port/ (a
 * mailbox and more after it; imap:// takes STARTTLS: never without TLS), logged in as
 * `user` with `password`, kept out of the URL (errors log it); `command` is sent in
 * place of what libcurl sends for the URL (NULL: that, LIST for a bare server). done()
 * gets the answer's untagged lines ("" when none) and 0, IMAP having no status codes,
 * or NULL and the CURLcode negated when it failed: -CURLE_LOGIN_DENIED a refused
 * password, -CURLE_COULDNT_RESOLVE_HOST or -CURLE_COULDNT_CONNECT no server there.
 * At most 30 s. */
void ui_fetch_mail(struct ui_fetch *f, const char *url, const char *user, const char *password,
                   const char *command, void (*done)(const char *data, long status));

/* HTML (browser/html.c, with Web Lite's litehtml): `html`, a fragment (a body's
 * content), laid out `w` px wide (CSS px: 1.5 of them, as in Web Lite), in the UI's
 * fonts, `css` (may be NULL) after the defaults (no margins, images no wider than
 * it); white unless it sets a background. An object w wide and as high as the
 * document, at most max_h (cut there), scrolled sideways when its content is wider
 * (a table that cannot fit); its images (PNG, JPEG) are fetched and
 * shown as they come, which changes its height. Drawn on the CPU into textures:
 * for what does not change. Relative links and images go from base_url. */
lv_obj_t *ui_html_create(lv_obj_t *parent, const char *html, const char *base_url, int32_t w, int32_t max_h,
                         const char *css);

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

/* LVGL's frame rate and CPU counter in a corner (Settings › Diagnostics, `fps_counter`,
 * off by default; ui_init() applies it) */
void ui_fps_counter_set(bool on);

/* Turning sideways (turn.c): a screen that turns with the phone. While it is on top,
 * the display is rotated as the phone is held: LVGL lays the screens out at 1440 x
 * 720 and sends LV_EVENT_RESOLUTION_CHANGED to the display (lay the screen out again
 * then). `fullscreen`: sideways, without the status bar and the back gesture (give
 * a way back). Other screens are upright: the display turns back before one is
 * shown, or call ui_upright() before making one (sizes taken from the display). */
void ui_screen_turns(lv_obj_t *scr, bool fullscreen);
bool ui_sideways(void);
void ui_upright(void);
/* Rotation lock (the shade's toggle, `rotation_lock` in the settings): screens that turn
 * stay at the rotation the display had when it was locked (lv_display_rotation_t), -1
 * while unlocked */
int ui_rotation_lock(void);
void ui_rotation_lock_set(bool on);

/* The accelerometer as turn.c reads it, for diagnostics */
struct ui_accel {
	char dev[32], name[32]; /* "iio:device1", "mpu6050" */
	float m[3][3];          /* mount matrix, rows: the phone's x, y, z */
	float scale;            /* m/s² per raw unit, 0 if unknown */
	float raw[3];
	int rotation;           /* what the display would turn to (lv_display_rotation_t), -1: as it is */
};
/* Reads it now (opened if it was not); false if there is none */
bool ui_accel_read(struct ui_accel *a);
bool ui_screen_fullscreen(lv_obj_t *scr); /* turns, full screen sideways */
void ui_turn_for(lv_obj_t *to);  /* ui.c: before a transition to `to` */
void ui_chrome_update(void);     /* ui.c: the status bar and back gesture, for the screen on top */

/* Theme, status bar and wallpaper (ui.c), home screen and menu (home.c) */
void ui_init(lv_display_t *disp);
void ui_status_update(void);
void ui_status_bar_foreground(void); /* over what was added to the top layer since */
/* A battery drawn at the object's size (about 2:1 wide, the nub on the right): filled as
 * far as the level, in the text color, red below 15%, green while charging with a bolt
 * over it; crossed out (absent), or a "?" (unknown). In the text color of the object. */
#define UI_BATTERY_UNKNOWN (-1) /* no driver */
#define UI_BATTERY_ABSENT (-2)  /* a dead or absent cell */
lv_obj_t *ui_battery_icon_create(lv_obj_t *parent, int32_t w, int32_t h);
void ui_battery_icon_set(lv_obj_t *icon, int level, bool charging); /* level 0-100 or the above */
void ui_battery_icon_update(lv_obj_t *icon); /* from the battery's sysfs now */
/* False for an object that is not one; level and charging may be NULL */
bool ui_battery_icon_get(lv_obj_t *obj, int *level, bool *charging);
/* The battery (battery.c), sampled by ui_status_update(): level -1 unknown, status as below */
void ui_battery_update(int level, const char *status);
/* "Charging", "Discharging" or the driver's other words; plugged in without charging "Full"
 * at 100%, "Not charging" below (the AXP803 then says "Discharging"); "" without a driver */
void ui_battery_status(char *buf, size_t size);
bool ui_battery_plugged(const char *status); /* "Charging", "Full" or "Not charging" */
bool ui_battery_present(void); /* false for a dead or absent cell, even if the PMIC says present */
bool ui_usb_powered(void); /* a charger or a computer on USB */
/* While charging, "1 h 20 min" until full (ui_battery_update() averages the current) */
bool ui_battery_time_to_full(char *buf, size_t size);
void ui_battery_duration(char *buf, size_t size, long minutes); /* "1 h 20 min" */
/* Since unplugged, in minutes: false while plugged in */
bool ui_battery_since_unplugged(long *screen_on, long *asleep);
void ui_battery_screen(bool on); /* the backlight on or off (ui_backlight_on()) */
/* Its history, read oldest first */
struct battery_sample {
	time_t time;
	int level; /* percent */
	bool charging;
};
int battery_log_read(struct battery_sample *s, int max); /* the newest max */
/* When the screen was on (wall clock), oldest first, up to now if it is */
struct screen_period {
	time_t start, end;
};
int battery_screen_read(struct screen_period *p, int max); /* the newest max */
int ui_usb_net(void); /* as of the last status update: 0 no USB network, 1 no internet, 2 internet */
/* As of the last status update: -1 no Wi-Fi connection, 0 connecting or without an address,
 * 1 to 4 bars of signal (settings/wifi.c reads it) */
int ui_wifi_bars(void);
int wifi_level(void); /* the same, read now from wpa_supplicant (ui_status_update()) */
void wifi_open(void); /* Settings, Wi-Fi */
void wifi_summary(lv_obj_t *value); /* the network's name, "Not connected", "Not available" */
/* 1 connected (its SSID in ssid), 0 not connected (or off), -1 not available */
int wifi_network(char *ssid, size_t size);
/* On (default) or off: wlan0 down, remembered (wifi=0 in the settings) */
bool wifi_enabled(void);
void wifi_set_enabled(bool on);
bool ui_at_home(void); /* the home screen or the menu on top, no app open */
/* Quick settings (shade.c): pulled down from the status bar, or opened by tapping it */
void ui_shade_init(lv_obj_t *status_bar);
/* Pulled down from obj too (the home screen: within the thumb's reach); up is left to
 * obj's own drags, the first way the finger goes decides */
void ui_shade_pull_from(lv_obj_t *obj);
bool ui_shade_shown(void); /* open, opening or pulled */
void ui_shade_close(void); /* at once, e.g. when locking: not left over a call */
void home_create(void);
/* Clock and date as on the home screen, 12% down `parent` (home.c); update it each second */
lv_obj_t *ui_clock_create(lv_obj_t *parent);
void ui_clock_update(lv_obj_t *clock);
void ui_set_screens(lv_obj_t *home, lv_obj_t *menu); /* home.c, once */
/* main.c: watches for input devices and opens them (again after external_run()) */
void ui_input_start(void);
void ui_cpu_boost(bool on); /* cpu.c: the CPU pinned at its highest frequency, or its lowest */
/* cpu.c: its lowest while `thread` (a game's engine) keeps up, its highest once it is busy
 * most of a second, until ui_cpu_boost() (false when the game is left) */
void ui_cpu_boost_when_busy(pthread_t thread);
/* Boot splash (splash.c): the kernel's boot logo, fading into the home screen */
void ui_splash(void);

/* Volume (volume.c): the saved level at startup; the volume keys of a keypad step it
 * and show a slider */
void ui_volume_init(void);
void ui_volume_keys(lv_indev_t *indev);
#define UI_VOLUME_LEVELS 16
int ui_volume_get(void);
void ui_volume_set(int level); /* 0 (muted) to UI_VOLUME_LEVELS, saved */
/* While a call has sound (modem.c, on with its board's audio script): the keys step
 * the call's level instead, 1 to UI_VOLUME_LEVELS, saved; the music's is set again
 * once off */
void ui_volume_call(bool on);
int ui_volume_call_level(void);

/* Screenshots (screenshot.c) of what the display shows, as JPEG: into `path` ("-":
 * stdout, NULL: /var/lib/photos), works without LVGL; ui_screenshot() takes one
 * shortly after (once the volume slider is gone) and flashes the screen */
bool ui_screenshot_save(const char *path);
void ui_screenshot(void);

/* Power menu (power.c): holding Power opens it (Power off, Restart); volume.c passes
 * the key's presses and releases on, and cancels the hold for a screenshot */
void ui_power_key(bool pressed);
void ui_power_cancel(void);

/* Lock screen (lock.c): put up by going to sleep, swiped up to unlock */
void ui_lock(void);
bool ui_locked(void);
void ui_unlock(void); /* slid away, as swiped (a notification opened) */
/* Hidden, if locked, while something must show over it (an incoming call), counting as
 * unlocked; shown again, locked */
void ui_lock_hide(bool hide);

/* Screen brightness in percent (settings.c), -1 without a backlight; setting it does
 * not save it (the "brightness" setting, applied at startup) */
#define UI_BRIGHTNESS_MIN 5 /* a black screen cannot be turned back up */
int ui_brightness_get(void);
/* The backlight off or on again at its brightness, which is kept (bl_power) */
void ui_backlight_on(bool on);
void ui_brightness_set(int percent);

/* External apps (external.c): programs in /var/lib/apps/<id>/, found at startup */
void external_scan(void);                    /* adds them to ui_apps, before the menu is made */
void external_open(const struct ui_app *app); /* runs it on the display until it exits */

/* Apps (one file each) */
void phone_open(void);      /* dialer: phone.c */
void phone_dial(const char *number); /* dialer with a number entered, to change it */
void phone_call(const char *number); /* the call screen, calling number */
/* A USSD code (*111#, *#06#: *, # first, # last) rather than a number: ussd.c */
bool ussd_is_code(const char *number);
void ussd_open(const char *code); /* sent, the network's replies as a conversation */
void contacts_open(void);   /* contacts.c */
void contacts_show(const char *number); /* its contact, or a new one with that number */
void contacts_new(void);    /* the editor for a new contact */
/* The contact list in `list`, a scrolling column (Phone's Contacts page), up to
 * date whenever its screen is shown again */
void contacts_list_create(lv_obj_t *list);
void messages_open(void);   /* messages.c */
void messages_open_thread(const char *number); /* conversation with a number */
bool messages_thread_shown(const char *number); /* its thread is the screen on top */
void settings_open(void);   /* settings.c */
void settings_apply(void);  /* at startup: brightness, time zone, haptics */
void calculator_open(void); /* calculator.c */
void clock_open(void);      /* clock.c: alarm, stopwatch, timer */
void calendar_open(void);   /* calendar.c: month view */
void camera_open(void);     /* camera.c */
void gallery_open(void);    /* gallery.c: photos from the camera */
void notes_open(void);      /* notes.c */
void tasks_open(void);      /* tasks.c: things to do, ticked off */
void tasks_start(void);     /* its reminders set, at startup */
void files_open(void);      /* files.c: file system browser */
void music_open(void);      /* music.c: MP3s in /var/lib/music, played by mpg123 */
void maps_open(void);       /* maps.c: OpenStreetMap */
void weather_open(void);    /* weather.c: Open-Meteo forecast */
void compass_open(void);    /* compass.c: magnetometer, tilt-compensated */
void browser_open(void);    /* browser/: Web Lite, web pages with litehtml, see browser.h */
void books_open(void);      /* books/: ePub reader, books in /var/lib/books, see books.h */
void snake_open(void);      /* snake.c */
void recorder_open(void);   /* recorder.c: voice memos in /var/lib/memos */
void assistant_open(void);  /* assistant.c: chats with a language model (DeepSeek) */

#endif
