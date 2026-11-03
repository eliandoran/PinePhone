/*
 * Phone: recent calls and the dialer, switched by a navigation bar, and the call
 * screen. The modem is not driven yet, so a call only gets as far as "No SIM
 * card"; it is logged as an outgoing call (calls.c).
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data.h"


#define KEY_SIZE 168
#define KEY_GAP 48
#define MAX_DIGITS 40
#define ROW_H 120
#define MAX_ROWS 100 /* recents shown, after grouping */
#define NAV_H 128

enum { TAB_RECENTS, TAB_KEYPAD, TABS };

/* A round button with a symbol */
static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                              const char *symbol, lv_event_cb_t cb)
{
	lv_obj_t *button = ui_button(parent, size, size, color, pressed, symbol, cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), size >= 120 ? &ui_font_48 : &ui_font_32, 0);
	return button;
}

static void free_user_data_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* Call screen */

static void call_status_cb(lv_timer_t *timer)
{
	lv_obj_t *scr = lv_timer_get_user_data(timer);
	lv_obj_t *status = lv_obj_get_user_data(scr);
	/* The timer runs once and deletes itself: the screen must not delete it again */
	lv_obj_set_user_data(scr, NULL);
	lv_label_set_text_static(status, "No SIM card");
	lv_obj_set_style_text_color(status, lv_color_hex(UI_COLOR_ORANGE), 0);
	ui_vibrate(60);
}

static void call_delete_cb(lv_event_t *e)
{
	if (lv_obj_get_user_data(lv_event_get_current_target_obj(e)))
		lv_timer_delete(lv_event_get_user_data(e));
}

static void hang_up_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(30);
	ui_pop();
}

/* Mute, keypad and speaker: they only toggle until there is audio routing */
static void call_option(lv_obj_t *parent, const char *symbol, const char *caption)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, 160, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);

	lv_obj_t *button = round_button(col, 112, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, symbol, NULL);
	lv_obj_set_checkable(button, true);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_SHADE), LV_STATE_CHECKED);

	lv_obj_t *label = lv_label_create(col);
	lv_label_set_text_static(label, caption);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
}

void phone_call(const char *number)
{
	call_log_add(number, CALL_OUTGOING, time(NULL), 0);
	const struct contact *contact = contact_find(number);
	lv_obj_t *content;
	lv_obj_t *scr = ui_screen_create(LV_OPA_80, &content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(content, 32, 0);
	lv_obj_set_style_pad_bottom(content, 96, 0);
	lv_obj_set_scrollable(content, false);

	lv_obj_t *space = lv_obj_create(content);
	lv_obj_remove_style_all(space);
	lv_obj_set_size(space, 1, 120);

	avatar_create(content, contact, 224);

	lv_obj_t *name = lv_label_create(content);
	lv_label_set_text_static(name, contact ? contact->name : ui_intern(number));
	lv_obj_set_width(name, LV_PCT(100));
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(name, &ui_font_48, 0);
	lv_obj_set_style_margin_top(name, 40, 0);

	if (contact) {
		lv_obj_t *label = lv_label_create(content);
		lv_label_set_text_static(label, contact->number);
		lv_obj_set_style_text_font(label, &ui_font_26, 0);
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(label, 12, 0);
	}

	lv_obj_t *status = lv_label_create(content);
	lv_label_set_text_static(status, "Calling...");
	lv_obj_set_style_text_font(status, &ui_font_26, 0);
	lv_obj_set_style_text_color(status, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(status, 24, 0);

	space = lv_obj_create(content);
	lv_obj_remove_style_all(space);
	lv_obj_set_width(space, 1);
	lv_obj_set_flex_grow(space, 1);

	lv_obj_t *options = ui_box(content);
	lv_obj_set_size(options, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(options, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(options, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	call_option(options, LV_SYMBOL_MUTE, "Mute");
	call_option(options, LV_SYMBOL_KEYBOARD, "Keypad");
	call_option(options, LV_SYMBOL_VOLUME_MAX, "Speaker");

	lv_obj_t *hang_up = round_button(content, 144, UI_COLOR_RED, UI_COLOR_RED_PRESSED, LV_SYMBOL_CALL, hang_up_cb);
	lv_obj_set_style_margin_top(hang_up, 72, 0);

	/* The status changes after a moment, as if the modem had answered */
	lv_timer_t *timer = lv_timer_create(call_status_cb, 2000, scr);
	lv_timer_set_repeat_count(timer, 1);
	lv_obj_set_user_data(scr, status);
	lv_obj_add_event_cb(scr, call_delete_cb, LV_EVENT_DELETE, timer);

	ui_push(scr);
}

/* Dialer */

/* The Phone screen */
struct phone {
	char digits[MAX_DIGITS + 1];
	lv_obj_t *display, *number, *name, *backspace;
	lv_obj_t *tabs[TABS], *pages[TABS], *recents, *trash, *confirm;
	unsigned tab, version; /* version: data_version the recents show */
};

static const struct key {
	char digit;
	const char *text, *letters;
} keys[] = {
	{ '1', "1", " " },   { '2', "2", "ABC" }, { '3', "3", "DEF" },
	{ '4', "4", "GHI" }, { '5', "5", "JKL" }, { '6', "6", "MNO" },
	{ '7', "7", "PQRS" }, { '8', "8", "TUV" }, { '9', "9", "WXYZ" },
	{ '*', "*", " " },   { '0', "0", "+" },   { '#', "#", " " },
};

static void dialer_update(struct phone *d)
{
	size_t n = strlen(d->digits);

	/* Typed text: not worth caching */
	lv_label_set_text(d->number, d->digits);
	lv_obj_set_style_text_font(d->number, n <= 18 ? &ui_font_48 : &ui_font_32, 0);
	/* Too long even then: show the end, where the typing happens */
	lv_obj_align(d->number, n > 28 ? LV_ALIGN_RIGHT_MID : LV_ALIGN_CENTER, 0, -24);

	const struct contact *c = contact_find(d->digits);
	lv_label_set_text_static(d->name, c ? c->name : "");
	lv_obj_set_hidden(d->backspace, n == 0);
}

static void dialer_add(struct phone *d, char c)
{
	size_t n = strlen(d->digits);
	if (n == MAX_DIGITS)
		return;
	d->digits[n] = c;
	d->digits[n + 1] = '\0';
	dialer_update(d);
}

static void key_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	const struct key *key = lv_obj_get_user_data(lv_event_get_current_target_obj(e));

	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		ui_vibrate(15);
		break;
	case LV_EVENT_SHORT_CLICKED:
		dialer_add(d, key->digit);
		break;
	case LV_EVENT_LONG_PRESSED:
		/* Long press on 0 gives '+', as on every phone */
		if (key->digit == '0') {
			dialer_add(d, '+');
			ui_vibrate(30);
		}
		break;
	default:
		break;
	}
}

static void backspace_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	size_t n = strlen(d->digits);

	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		ui_vibrate(15);
		return;
	}
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
		n = 0;
	else if (n)
		n--;
	d->digits[n] = '\0';
	dialer_update(d);
}

static void call_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	ui_vibrate(15);
	if (d->digits[0])
		phone_call(d->digits);
}

static void contacts_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_open();
}

static lv_obj_t *key_create(lv_obj_t *parent, const struct key *key, struct phone *d)
{
	lv_obj_t *button = lv_obj_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, KEY_SIZE, KEY_SIZE);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_10, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(button, 4, 0);
	lv_obj_set_scrollable(button, false);
	lv_obj_set_clickable(button, true);
	lv_obj_set_user_data(button, (void *)key);
	lv_obj_add_event_cb(button, key_cb, LV_EVENT_PRESSED, d);
	lv_obj_add_event_cb(button, key_cb, LV_EVENT_SHORT_CLICKED, d);
	lv_obj_add_event_cb(button, key_cb, LV_EVENT_LONG_PRESSED, d);

	lv_obj_t *digit = lv_label_create(button);
	lv_label_set_text_static(digit, key->text);
	lv_obj_set_style_text_font(digit, &ui_font_48, 0);

	lv_obj_t *letters = lv_label_create(button);
	lv_label_set_text_static(letters, key->letters);
	lv_obj_set_style_text_font(letters, &ui_font_16, 0);
	lv_obj_set_style_text_letter_space(letters, 3, 0);
	lv_obj_set_style_text_color(letters, lv_color_hex(UI_COLOR_DIM), 0);
	return button;
}

/* Recents */

static const char *const kind_names[] = { "Outgoing", "Incoming", "Missed" };

static void recent_cb(lv_event_t *e)
{
	ui_vibrate(15);
	phone_call(lv_event_get_user_data(e));
}

static void info_cb(lv_event_t *e)
{
	contacts_show(lv_event_get_user_data(e));
}

/* A call, or `n` of the same kind in a row with the same number */
static void recent_create(lv_obj_t *card, const struct call *c, unsigned n, bool divider)
{
	const struct contact *contact = contact_find(c->number);
	const char *who = contact ? contact->name : c->number;
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, recent_cb, (void *)c->number);
	avatar_create(row, contact, 72);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *top = ui_row_line(text);
	lv_obj_t *name = ui_one_line_label(top, "", &ui_font_26);
	lv_obj_set_flex_grow(name, 1);
	if (n > 1)
		ui_set_text_fmt(name, "%s (%u)", who, n);
	else
		lv_label_set_text_static(name, who);
	if (c->kind == CALL_MISSED)
		lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_RED), 0);
	char when[32];
	ui_when_format(when, sizeof(when), c->time);
	ui_set_text(ui_dim_label(top, "", &ui_font_20), when);

	lv_obj_t *kind = ui_dim_label(text, kind_names[c->kind], &ui_font_20);
	if (c->duration)
		ui_set_text_fmt(kind, "%s, %u:%02u", kind_names[c->kind], c->duration / 60, c->duration % 60);
	else if (c->kind == CALL_OUTGOING)
		lv_label_set_text_static(kind, "Outgoing, not connected");

	/* The contact, or a new one with this number */
	lv_obj_t *info = ui_box(row);
	lv_obj_set_size(info, 88, 88);
	lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(info, lv_color_hex(UI_COLOR_BUTTON), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(info, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_text_color(info, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_clickable(info, true);
	lv_obj_add_event_cb(info, info_cb, LV_EVENT_CLICKED, (void *)c->number);
	lv_obj_center(ui_glyph_create(info, 48, UI_GLYPH_INFO));
}

static void recents_fill(struct phone *d)
{
	lv_obj_clean(d->recents);
	d->version = data_version;

	unsigned n = call_count();
	if (!n) {
		ui_empty_label(d->recents, "No recent calls");
		return;
	}

	lv_obj_t *card = ui_card_create(d->recents);
	unsigned rows = 0;
	for (unsigned i = 0; i < n && rows < MAX_ROWS; rows++) {
		const struct call *c = call_get(i), *next;
		unsigned same = 1;
		while ((next = call_get(i + same)) && next->kind == c->kind && number_equal(next->number, c->number))
			same++;
		i += same;
		recent_create(card, c, same, i < n && rows + 1 < MAX_ROWS);
	}
}

/* The Phone screen: Recents and Keypad, switched by a navigation bar */

static void tab_select(struct phone *d, unsigned sel)
{
	for (unsigned i = 0; i < TABS; i++) {
		lv_obj_set_state(d->tabs[i], LV_STATE_CHECKED, i == sel);
		lv_obj_set_hidden(d->pages[i], i != sel);
	}
	d->tab = sel;
	/* Clearing the log is offered on its page */
	lv_obj_set_hidden(d->trash, sel != TAB_RECENTS || !call_count());
	lv_obj_set_hidden(d->confirm, true);
}

static void tab_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	lv_obj_t *tab = lv_event_get_current_target_obj(e);
	tab_select(d, tab == d->tabs[TAB_RECENTS] ? TAB_RECENTS : TAB_KEYPAD);
}

/* Navigation bar at the bottom: an icon and a caption per page, the current one
 * in the accent color */
static void nav_create(lv_obj_t *parent, struct phone *d)
{
	static const char *const names[TABS] = { "Recents", "Keypad" };
	static const enum ui_glyph glyphs[TABS] = { UI_GLYPH_CLOCK, UI_GLYPH_DIALPAD };

	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), NAV_H);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);

	for (unsigned i = 0; i < TABS; i++) {
		lv_obj_t *tab = ui_box(bar);
		lv_obj_set_height(tab, LV_PCT(100));
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(tab, 8, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, d);

		ui_glyph_create(tab, 48, glyphs[i]);
		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, names[i]);
		lv_obj_set_style_text_font(label, &ui_font_20, 0);
		d->tabs[i] = tab;
	}
}

static void trash_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	lv_obj_set_hidden(d->confirm, !lv_obj_is_hidden(d->confirm));
}

static void cancel_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	lv_obj_set_hidden(d->confirm, true);
}

static void clear_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	call_log_clear();
	ui_vibrate(15);
	recents_fill(d);
	tab_select(d, TAB_RECENTS);
}

/* Back from a call or a contact: the log or the names may have changed */
static void phone_load_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	if (d->version == data_version)
		return;
	recents_fill(d);
	tab_select(d, d->tab);
	dialer_update(d);
}

static void phone_screen(const char *number, unsigned tab)
{
	struct phone *d = calloc(1, sizeof(*d));
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Phone", &header, &body);
	lv_obj_add_event_cb(scr, free_user_data_cb, LV_EVENT_DELETE, d);
	lv_obj_add_event_cb(scr, phone_load_cb, LV_EVENT_SCREEN_LOAD_START, d);
	ui_header_button(header, LV_SYMBOL_LIST, contacts_cb, NULL);
	d->trash = ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, d);
	lv_obj_set_scrollable(body, false);

	d->recents = d->pages[TAB_RECENTS] = ui_box(body);
	lv_obj_set_width(d->recents, LV_PCT(100));
	lv_obj_set_flex_grow(d->recents, 1);
	lv_obj_set_flex_flow(d->recents, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(d->recents, 16, 0);
	lv_obj_set_style_pad_top(d->recents, 8, 0);
	lv_obj_set_style_pad_bottom(d->recents, 32, 0);
	lv_obj_set_scrollable(d->recents, true);
	lv_obj_set_clickable(d->recents, true);
	lv_obj_set_scroll_dir(d->recents, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(d->recents, LV_SCROLLBAR_MODE_OFF);

	lv_obj_t *keypad_page = d->pages[TAB_KEYPAD] = ui_box(body);
	lv_obj_set_width(keypad_page, LV_PCT(100));
	lv_obj_set_flex_grow(keypad_page, 1);
	lv_obj_set_flex_flow(keypad_page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(keypad_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_bottom(keypad_page, 24, 0);

	/* Number and the contact's name, children are clipped to it */
	d->display = ui_box(keypad_page);
	lv_obj_set_width(d->display, LV_PCT(100));
	lv_obj_set_flex_grow(d->display, 1);
	lv_obj_set_style_pad_hor(d->display, 24, 0);

	d->number = lv_label_create(d->display);
	lv_label_set_text_static(d->number, "");

	d->name = lv_label_create(d->display);
	lv_label_set_text_static(d->name, "");
	lv_obj_set_style_text_font(d->name, &ui_font_26, 0);
	lv_obj_set_style_text_color(d->name, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_align(d->name, LV_ALIGN_CENTER, 0, 40);

	lv_obj_t *keypad = ui_box(keypad_page);
	lv_obj_set_size(keypad, 3 * KEY_SIZE + 2 * KEY_GAP, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(keypad, KEY_GAP, 0);
	lv_obj_set_style_pad_row(keypad, 24, 0);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		key_create(keypad, &keys[i], d);

	/* Call button under the middle column, backspace under the right one */
	lv_obj_t *row = ui_box(keypad_page);
	lv_obj_set_size(row, LV_PCT(100), 176);
	lv_obj_set_style_margin_top(row, 24, 0);

	lv_obj_t *call = round_button(row, 144, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, LV_SYMBOL_CALL, NULL);
	lv_obj_center(call);
	lv_obj_add_event_cb(call, call_cb, LV_EVENT_CLICKED, d);

	d->backspace = lv_button_create(row);
	lv_obj_set_size(d->backspace, KEY_SIZE, 120);
	lv_obj_align(d->backspace, LV_ALIGN_CENTER, KEY_SIZE + KEY_GAP, 0);
	lv_obj_set_style_bg_opa(d->backspace, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(d->backspace, 0, 0);
	lv_obj_add_event_cb(d->backspace, backspace_cb, LV_EVENT_PRESSED, d);
	lv_obj_add_event_cb(d->backspace, backspace_cb, LV_EVENT_SHORT_CLICKED, d);
	lv_obj_add_event_cb(d->backspace, backspace_cb, LV_EVENT_LONG_PRESSED, d);
	lv_obj_t *icon = lv_label_create(d->backspace);
	lv_label_set_text_static(icon, LV_SYMBOL_BACKSPACE);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_center(icon);

	/* Clearing the log, shown by the trash button */
	d->confirm = ui_confirm_bar(body, LV_SYMBOL_TRASH "  Clear recent calls", clear_cb, cancel_cb, d);
	nav_create(body, d);

	/* Only what can be dialed: "+40 744-555" is entered as "+40744555" */
	for (const char *c = number ? number : ""; *c; c++) {
		size_t n = strlen(d->digits);
		if (n < MAX_DIGITS && ((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#'))
			d->digits[n] = *c;
	}
	dialer_update(d);
	recents_fill(d);
	tab_select(d, tab);
	ui_push(scr);
}

void phone_dial(const char *number)
{
	phone_screen(number, TAB_KEYPAD);
}

/* Recents, unless there are none yet */
void phone_open(void)
{
	phone_screen(NULL, call_count() ? TAB_RECENTS : TAB_KEYPAD);
}
