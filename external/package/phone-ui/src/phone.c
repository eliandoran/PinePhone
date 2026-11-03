/*
 * Phone: dialer and the call screen. The modem is not driven yet, so a call
 * only gets as far as "No SIM card".
 */

#include <stdlib.h>
#include <string.h>

#include "data.h"

#define COLOR_GREEN 0x34c759
#define COLOR_RED 0xff3b30
#define COLOR_ORANGE 0xff9f0a

#define KEY_SIZE 168
#define KEY_GAP 48
#define MAX_DIGITS 40

/* A round button with a symbol */
static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, uint32_t color, const char *symbol)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_size(button, size, size);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_text_color(button, lv_color_white(), 0); /* inherited by the icon */
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, size >= 120 ? &ui_font_48 : &ui_font_32, 0);
	lv_obj_center(icon);
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
	lv_obj_set_style_text_color(status, lv_color_hex(COLOR_ORANGE), 0);
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
	lv_obj_t *col = lv_obj_create(parent);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, 160, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	lv_obj_set_clickable(col, false);
	lv_obj_set_scrollable(col, false);

	lv_obj_t *button = round_button(col, 112, UI_COLOR_BUTTON, symbol);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_checkable(button, true);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_SHADE), LV_STATE_CHECKED);

	lv_obj_t *label = lv_label_create(col);
	lv_label_set_text_static(label, caption);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
}

static void call_open(const char *number)
{
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

	lv_obj_t *options = lv_obj_create(content);
	lv_obj_remove_style_all(options);
	lv_obj_set_size(options, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(options, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(options, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_clickable(options, false);
	lv_obj_set_scrollable(options, false);
	call_option(options, LV_SYMBOL_MUTE, "Mute");
	call_option(options, LV_SYMBOL_KEYBOARD, "Keypad");
	call_option(options, LV_SYMBOL_VOLUME_MAX, "Speaker");

	lv_obj_t *hang_up = round_button(content, 144, COLOR_RED, LV_SYMBOL_CALL);
	lv_obj_set_style_margin_top(hang_up, 72, 0);
	lv_obj_add_event_cb(hang_up, hang_up_cb, LV_EVENT_CLICKED, NULL);

	/* The status changes after a moment, as if the modem had answered */
	lv_timer_t *timer = lv_timer_create(call_status_cb, 2000, scr);
	lv_timer_set_repeat_count(timer, 1);
	lv_obj_set_user_data(scr, status);
	lv_obj_add_event_cb(scr, call_delete_cb, LV_EVENT_DELETE, timer);

	ui_push(scr);
}

/* Dialer */

struct dialer {
	char digits[MAX_DIGITS + 1];
	lv_obj_t *display, *number, *name, *backspace;
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

static void dialer_update(struct dialer *d)
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

static void dialer_add(struct dialer *d, char c)
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
	struct dialer *d = lv_event_get_user_data(e);
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
	struct dialer *d = lv_event_get_user_data(e);
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
	struct dialer *d = lv_event_get_user_data(e);
	ui_vibrate(15);
	if (d->digits[0])
		call_open(d->digits);
}

static void contacts_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_open();
}

static lv_obj_t *key_create(lv_obj_t *parent, const struct key *key, struct dialer *d)
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

void phone_dial(const char *number)
{
	struct dialer *d = calloc(1, sizeof(*d));
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Phone", &header, &body);
	lv_obj_add_event_cb(scr, free_user_data_cb, LV_EVENT_DELETE, d);
	ui_header_button(header, LV_SYMBOL_LIST, contacts_cb, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_bottom(body, 48, 0);
	lv_obj_set_scrollable(body, false);

	/* Number and the contact's name, children are clipped to it */
	d->display = lv_obj_create(body);
	lv_obj_remove_style_all(d->display);
	lv_obj_set_width(d->display, LV_PCT(100));
	lv_obj_set_flex_grow(d->display, 1);
	lv_obj_set_style_pad_hor(d->display, 24, 0);
	lv_obj_set_scrollable(d->display, false);
	lv_obj_set_clickable(d->display, false);

	d->number = lv_label_create(d->display);
	lv_label_set_text_static(d->number, "");

	d->name = lv_label_create(d->display);
	lv_label_set_text_static(d->name, "");
	lv_obj_set_style_text_font(d->name, &ui_font_26, 0);
	lv_obj_set_style_text_color(d->name, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_align(d->name, LV_ALIGN_CENTER, 0, 40);

	lv_obj_t *keypad = lv_obj_create(body);
	lv_obj_remove_style_all(keypad);
	lv_obj_set_size(keypad, 3 * KEY_SIZE + 2 * KEY_GAP, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(keypad, KEY_GAP, 0);
	lv_obj_set_style_pad_row(keypad, 24, 0);
	lv_obj_set_scrollable(keypad, false);
	lv_obj_set_clickable(keypad, false);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		key_create(keypad, &keys[i], d);

	/* Call button under the middle column, backspace under the right one */
	lv_obj_t *row = lv_obj_create(body);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), 176);
	lv_obj_set_style_margin_top(row, 24, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, false);

	lv_obj_t *call = round_button(row, 144, COLOR_GREEN, LV_SYMBOL_CALL);
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

	/* Only what can be dialed: "+40 744-555" is entered as "+40744555" */
	for (const char *c = number ? number : ""; *c; c++) {
		size_t n = strlen(d->digits);
		if (n < MAX_DIGITS && ((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#'))
			d->digits[n] = *c;
	}
	dialer_update(d);
	ui_push(scr);
}

void phone_open(void)
{
	phone_dial(NULL);
}
