/*
 * Phone: recent calls and the dialer, switched by a navigation bar, and the call
 * screen, for calls dialed here and coming in (modem.h, which logs them: calls.c).
 * A USSD code dialed opens ussd.c instead.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data.h"
#include "i18n.h"
#include "log.h"
#include "modem.h"


#define KEY_SIZE 168
#define CALL_DIM LV_OPA_80 /* the call screen's wallpaper */
#define KEY_GAP 48
#define MAX_DIGITS 40
#define ROW_H 120
#define MAX_ROWS 100 /* recents shown, after grouping */
#define NAV_H 128

enum { TAB_RECENTS, TAB_CONTACTS, TAB_KEYPAD, TABS };

/* A round button with a symbol */
static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                              const char *symbol, lv_event_cb_t cb)
{
	lv_obj_t *button = ui_button(parent, size, size, color, pressed, symbol, cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), size >= 120 ? &ui_font_48 : &ui_font_32, 0);
	return button;
}

/* The keypad's keys, the dialer's and the call screen's */
static const struct key {
	char digit;
	const char *text, *letters;
} keys[] = {
	{ '1', "1", " " },   { '2', "2", "ABC" }, { '3', "3", "DEF" },
	{ '4', "4", "GHI" }, { '5', "5", "JKL" }, { '6', "6", "MNO" },
	{ '7', "7", "PQRS" }, { '8', "8", "TUV" }, { '9', "9", "WXYZ" },
	{ '*', "*", " " },   { '0', "0", "+" },   { '#', "#", " " },
};

/* A key of `size`: `cb` gets its pressed, short clicked and long pressed events, the
 * key as the button's user data */
static lv_obj_t *key_create(lv_obj_t *parent, const struct key *key, int32_t size, lv_event_cb_t cb,
                            void *user_data)
{
	lv_obj_t *button = lv_obj_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, size, size);
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
	lv_obj_add_event_cb(button, cb, LV_EVENT_PRESSED, user_data);
	lv_obj_add_event_cb(button, cb, LV_EVENT_SHORT_CLICKED, user_data);
	lv_obj_add_event_cb(button, cb, LV_EVENT_LONG_PRESSED, user_data);

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

static void free_user_data_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* Call screen: the call as the modem has it (modem.h), dialed here or coming in. While
 * one rings here, Decline and Answer; on, Mute, Keypad and Speaker, and the time it
 * has lasted; ended, why, then back to where it was. Keypad shows keys instead of who
 * it is with, each sending its tone (menus: "press 1"), the keys pressed above them */

#define END_SHOW_MS 2000 /* how it ended, before going back */
#define RING_VIBRATE_MS 1500
#define TONES_SHOWN 20   /* the last keys pressed */

static struct call_screen {
	lv_obj_t *scr, *space, *who, *typed, *status, *incoming, *options, *keypad, *bottom, *hide;
	const char *who_number; /* the one `who` shows (ui_intern()) */
	const char *who_name;   /* its name, number or "Unknown number" (ui_intern()) */
	char tones[TONES_SHOWN + 1];
	bool keypad_open;
	lv_timer_t *timer;   /* the time shown each second; ended: going back */
	lv_timer_t *ring;    /* vibrating while it rings here */
	bool ended;
	bool lock_hidden;    /* came in over the lock screen, which comes back after the call */
} *cs;

static void call_screen_back_cb(lv_timer_t *timer)
{
	struct call_screen *s = lv_timer_get_user_data(timer);
	s->timer = NULL; /* deleted after this, running once */
	if (ui_current() == s->scr)
		ui_pop(); /* under another screen: it goes with a tap on hang up */
}

static void call_screen_delete_cb(lv_event_t *e)
{
	struct call_screen *s = lv_event_get_user_data(e);
	if (s->timer)
		lv_timer_delete(s->timer);
	if (s->ring)
		lv_timer_delete(s->ring);
	if (cs == s)
		cs = NULL;
	/* Left during a call (back to home): it goes on, without a screen, and the phone
	 * stays unlocked */
	if (s->lock_hidden && s->ended)
		ui_lock_hide(false);
	free(s);
}

/* No longer the call's: its timers stopped, the screen left for hang up to close */
static void call_screen_detach(void)
{
	if (cs->timer)
		lv_timer_delete(cs->timer);
	if (cs->ring)
		lv_timer_delete(cs->ring);
	cs->timer = cs->ring = NULL;
	cs = NULL;
}

static void duration_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	unsigned s = modem_call_seconds();
	if (s >= 3600)
		lv_label_set_text_fmt(cs->status, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		lv_label_set_text_fmt(cs->status, "%u:%02u", s / 60, s % 60);
}

static void ring_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	ui_vibrate(600);
}

static void hang_up_cb(lv_event_t *e)
{
	struct call_screen *s = lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	if (s == cs && modem_call_state() != MODEM_CALL_NONE && !s->ended)
		modem_call_hang_up(); /* the screen goes once it ended */
	else if (ui_current() == s->scr)
		ui_pop();
}

static void answer_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	modem_call_answer();
}

static void mute_cb(lv_event_t *e)
{
	modem_call_mute(lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED));
}

static void speaker_cb(lv_event_t *e)
{
	modem_call_speaker(lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED));
}

/* A round button with its caption under it; checkable buttons toggle */
static lv_obj_t *call_option(lv_obj_t *parent, const char *symbol, const char *caption, int32_t size,
                             uint32_t color, uint32_t pressed, bool checkable, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, 160, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);

	lv_obj_t *button = round_button(col, size, color, pressed, symbol, NULL);
	if (checkable) {
		lv_obj_set_checkable(button, true);
		lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_TEXT), LV_STATE_CHECKED);
		lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_SHADE), LV_STATE_CHECKED);
	}
	if (cb)
		lv_obj_add_event_cb(button, cb, checkable ? LV_EVENT_VALUE_CHANGED : LV_EVENT_CLICKED, user_data);

	lv_obj_t *label = lv_label_create(col);
	lv_label_set_text_static(label, caption);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return col;
}

/* Who the call is with: the contact's photo or initials, name and number, or the number
 * ("" hidden); again when an incoming call's number comes after its ring */
static void call_screen_who(const char *number)
{
	const struct contact *contact = number[0] ? contact_find(number) : NULL;
	lv_obj_clean(cs->who);
	avatar_create(cs->who, contact, 224);

	lv_obj_t *name = lv_label_create(cs->who);
	cs->who_name = ui_intern(contact ? contact->name : number[0] ? number : _("Unknown number"));
	lv_label_set_text_static(name, cs->who_name);
	lv_obj_set_width(name, LV_PCT(100));
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(name, &ui_font_48, 0);
	lv_obj_set_style_margin_top(name, 40, 0);

	if (contact) {
		lv_obj_t *label = lv_label_create(cs->who);
		lv_label_set_text_static(label, contact->number);
		lv_obj_set_style_text_font(label, &ui_font_26, 0);
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(label, 12, 0);
	}
	cs->who_number = ui_intern(number);
	if (cs->typed && !cs->tones[0])
		lv_label_set_text_static(cs->typed, cs->who_name);
}

/* The keypad instead of who the call is with and the options, or back */
static void call_keypad_show(bool open)
{
	cs->keypad_open = open;
	lv_obj_set_hidden(cs->space, open);
	lv_obj_set_hidden(cs->who, open);
	lv_obj_set_hidden(cs->typed, !open);
	lv_obj_set_hidden(cs->options, open);
	lv_obj_set_hidden(cs->keypad, !open);
	lv_obj_set_hidden(cs->hide, !open);
}

static void keypad_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	call_keypad_show(true);
}

static void hide_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	call_keypad_show(false);
}

static void tone_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) != LV_EVENT_PRESSED)
		return;
	const struct key *key = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
	ui_vibrate(UI_VIBRATE_TAP);
	modem_call_tone(key->digit);
	size_t n = strlen(cs->tones);
	if (n == TONES_SHOWN)
		memmove(cs->tones, cs->tones + 1, n--);
	cs->tones[n] = key->digit;
	cs->tones[n + 1] = '\0';
	lv_label_set_text_static(cs->typed, cs->tones);
}

/* Its wallpaper, darkened at this level the first time it is used: 80 ms on the phone */
void call_screen_prepare(void)
{
	ui_wallpaper(CALL_DIM);
}

/* The screen for a call with `number` ("" hidden), pushed: at once for one coming in
 * (`now`: what woke the phone, perhaps), else sliding in as screens do */
static void call_screen(const char *number, bool now)
{
	bool lock_hidden = false;
	if (cs) {
		lock_hidden = cs->lock_hidden; /* this one brings it back */
		cs->lock_hidden = false;
		call_screen_detach(); /* an ended one still showing */
	}
	cs = calloc(1, sizeof(*cs));
	cs->lock_hidden = lock_hidden;
	lv_obj_t *content;
	cs->scr = ui_screen_create(CALL_DIM, &content);
	lv_obj_add_event_cb(cs->scr, call_screen_delete_cb, LV_EVENT_DELETE, cs);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(content, 32, 0);
	lv_obj_set_style_pad_bottom(content, 96, 0);
	lv_obj_set_scrollable(content, false);

	cs->space = lv_obj_create(content);
	lv_obj_remove_style_all(cs->space);
	lv_obj_set_size(cs->space, 1, 120);

	cs->who = ui_box(content);
	lv_obj_set_size(cs->who, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cs->who, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cs->who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	call_screen_who(number);

	/* With the keypad: the keys pressed, who it is with before */
	cs->typed = lv_label_create(content);
	lv_label_set_text_static(cs->typed, cs->who_name);
	lv_label_set_long_mode(cs->typed, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(cs->typed, LV_PCT(100));
	lv_obj_set_style_text_align(cs->typed, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(cs->typed, &ui_font_48, 0);
	lv_obj_set_style_margin_top(cs->typed, 48, 0);
	lv_obj_set_hidden(cs->typed, true);

	cs->status = lv_label_create(content);
	lv_label_set_text_static(cs->status, "");
	lv_obj_set_style_text_font(cs->status, &ui_font_26, 0);
	lv_obj_set_style_text_color(cs->status, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(cs->status, 24, 0);

	lv_obj_t *space = lv_obj_create(content);
	lv_obj_remove_style_all(space);
	lv_obj_set_width(space, 1);
	lv_obj_set_flex_grow(space, 1);

	cs->options = ui_box(content);
	lv_obj_set_size(cs->options, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cs->options, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cs->options, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	call_option(cs->options, LV_SYMBOL_MUTE, _("Mute"), 112, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, true,
	            mute_cb, NULL);
	call_option(cs->options, LV_SYMBOL_KEYBOARD, _("Keypad"), 112, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, false,
	            keypad_cb, NULL);
	call_option(cs->options, LV_SYMBOL_VOLUME_MAX, _("Speaker"), 112, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	            true, speaker_cb, NULL);

	cs->keypad = ui_box(content);
	lv_obj_set_size(cs->keypad, 3 * KEY_SIZE + 2 * KEY_GAP, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cs->keypad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(cs->keypad, KEY_GAP, 0);
	lv_obj_set_style_pad_row(cs->keypad, 24, 0);
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		key_create(cs->keypad, &keys[i], KEY_SIZE, tone_cb, NULL);
	lv_obj_set_hidden(cs->keypad, true);

	/* Hang up, Hide on its right with the keypad */
	cs->bottom = ui_box(content);
	lv_obj_set_size(cs->bottom, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cs->bottom, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cs->bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(cs->bottom, 72, 0);
	lv_obj_t *left = ui_box(cs->bottom);
	lv_obj_set_size(left, 160, 1);
	lv_obj_t *hang_up = round_button(cs->bottom, 144, UI_COLOR_RED, UI_COLOR_RED_PRESSED, LV_SYMBOL_CALL, NULL);
	lv_obj_add_event_cb(hang_up, hang_up_cb, LV_EVENT_CLICKED, cs);
	lv_obj_t *right = ui_box(cs->bottom);
	lv_obj_set_size(right, 160, 144);
	cs->hide = lv_label_create(right);
	lv_label_set_text_static(cs->hide, _("Hide"));
	lv_obj_set_style_text_font(cs->hide, &ui_font_32, 0);
	lv_obj_center(cs->hide);
	lv_obj_set_clickable(cs->hide, true);
	lv_obj_set_ext_click_area(cs->hide, 40);
	lv_obj_add_event_cb(cs->hide, hide_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(cs->hide, true);

	/* Ringing here: Decline and Answer instead */
	cs->incoming = ui_box(content);
	lv_obj_set_size(cs->incoming, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cs->incoming, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cs->incoming, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	call_option(cs->incoming, LV_SYMBOL_CALL, _("Decline"), 144, UI_COLOR_RED, UI_COLOR_RED_PRESSED, false,
	            hang_up_cb, cs);
	call_option(cs->incoming, LV_SYMBOL_CALL, _("Answer"), 144, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, false,
	            answer_cb, NULL);
	lv_obj_set_hidden(cs->incoming, true);

	if (now)
		ui_push_now(cs->scr);
	else
		ui_push(cs->scr);
}

/* Not made: why (in English, as modem.c says it; translated here), until hung up */
static void call_screen_unavailable(const char *why)
{
	lv_label_set_text_static(cs->status, _(why));
	lv_obj_set_style_text_color(cs->status, lv_color_hex(UI_COLOR_ORANGE), 0);
	cs->ended = true;
	ui_vibrate(60);
}

void call_changed(enum modem_call state, enum modem_call_end end)
{
	if (!cs && state != MODEM_CALL_INCOMING)
		return; /* its screen was left: nothing to show */
	if (!cs || cs->ended)
		call_screen(modem_call_number(), state == MODEM_CALL_INCOMING);
	/* Coming in over the lock screen: shown over it */
	if (state == MODEM_CALL_INCOMING && ui_locked()) {
		ui_lock_hide(true);
		cs->lock_hidden = true;
		printf("phone-ui: [%.3f] call screen over the lock screen\n", log_time());
	}
	if (cs->timer)
		lv_timer_delete(cs->timer);
	cs->timer = NULL;
	if (cs->ring && state != MODEM_CALL_INCOMING) {
		lv_timer_delete(cs->ring);
		cs->ring = NULL;
	}
	/* The number as it came with RING's +CLIP */
	if (state != MODEM_CALL_NONE && strcmp(cs->who_number, modem_call_number()) != 0)
		call_screen_who(modem_call_number());

	bool incoming = state == MODEM_CALL_INCOMING;
	if (cs->keypad_open && (incoming || state == MODEM_CALL_NONE))
		call_keypad_show(false); /* ended: why, with who it was */
	lv_obj_set_hidden(cs->incoming, !incoming);
	lv_obj_set_hidden(cs->bottom, incoming || state == MODEM_CALL_NONE);
	lv_obj_set_hidden(cs->options, incoming || state == MODEM_CALL_NONE || cs->keypad_open);
	switch (state) {
	case MODEM_CALL_DIALING:
		lv_label_set_text_static(cs->status, _("Calling..."));
		break;
	case MODEM_CALL_ALERTING:
		lv_label_set_text_static(cs->status, _("Ringing..."));
		break;
	case MODEM_CALL_INCOMING:
		lv_label_set_text_static(cs->status, _("Incoming call"));
		if (!cs->ring)
			cs->ring = lv_timer_create(ring_cb, RING_VIBRATE_MS, NULL);
		break;
	case MODEM_CALL_ACTIVE:
		duration_cb(NULL);
		cs->timer = lv_timer_create(duration_cb, 1000, NULL);
		break;
	case MODEM_CALL_NONE: {
		static const char *const ends[] = { N_("Call ended"), N_("Busy"), N_("No answer"), N_("Call failed") };
		lv_label_set_text_static(cs->status, _(ends[end]));
		cs->ended = true;
		cs->timer = lv_timer_create(call_screen_back_cb, END_SHOW_MS, cs);
		lv_timer_set_repeat_count(cs->timer, 1);
		ui_vibrate(60);
		break;
	}
	}
}

void phone_call(const char *number)
{
	call_screen(number, false);
	const char *why = modem_call_unavailable();
	if (!why && !modem_call_dial(number))
		why = N_("Call failed");
	if (why) {
		call_log_add(number, CALL_OUTGOING, time(NULL), 0);
		call_screen_unavailable(why);
	}
}

/* Dialer */

/* The Phone screen */
struct phone {
	char digits[MAX_DIGITS + 1];
	lv_obj_t *display, *number, *name, *backspace;
	lv_obj_t *tabs[TABS], *pages[TABS], *recents, *trash, *add, *confirm;
	unsigned tab, version; /* version: data_version the recents show */
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
		ui_vibrate(UI_VIBRATE_TAP);
		break;
	case LV_EVENT_SHORT_CLICKED:
		dialer_add(d, key->digit);
		break;
	case LV_EVENT_LONG_PRESSED:
		/* Long press on 0 gives '+', as on every phone */
		if (key->digit == '0') {
			dialer_add(d, '+');
			ui_vibrate(UI_VIBRATE_TAP);
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
		ui_vibrate(UI_VIBRATE_TAP);
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
	ui_vibrate(UI_VIBRATE_TAP);
	if (ussd_is_code(d->digits))
		ussd_open(d->digits); /* not a call: not logged */
	else if (d->digits[0])
		phone_call(d->digits);
}

static void add_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_new();
}

/* Recents */

static const char *const kind_names[] = { N_("Outgoing"), N_("Incoming"), N_("Missed") };

static void recent_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
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

	lv_obj_t *kind = ui_dim_label(text, _(kind_names[c->kind]), &ui_font_20);
	if (c->duration)
		ui_set_text_fmt(kind, "%s, %u:%02u", _(kind_names[c->kind]), c->duration / 60, c->duration % 60);
	else if (c->kind == CALL_OUTGOING)
		lv_label_set_text_static(kind, _("Outgoing, not connected"));

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
		ui_empty_label(d->recents, _("No recent calls"));
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

/* The Phone screen: Recents, Contacts and Keypad, switched by a navigation bar */

static void tab_select(struct phone *d, unsigned sel)
{
	for (unsigned i = 0; i < TABS; i++) {
		lv_obj_set_state(d->tabs[i], LV_STATE_CHECKED, i == sel);
		lv_obj_set_hidden(d->pages[i], i != sel);
	}
	d->tab = sel;
	/* Clearing the log and adding a contact are offered on their pages */
	lv_obj_set_hidden(d->trash, sel != TAB_RECENTS || !call_count());
	lv_obj_set_hidden(d->add, sel != TAB_CONTACTS);
	lv_obj_set_hidden(d->confirm, true);
}

static void tab_cb(lv_event_t *e)
{
	struct phone *d = lv_event_get_user_data(e);
	lv_obj_t *tab = lv_event_get_current_target_obj(e);
	unsigned i = 0;
	while (i + 1 < TABS && d->tabs[i] != tab)
		i++;
	tab_select(d, i);
	if (i == TAB_RECENTS)
		call_missed_seen();
}

/* Navigation bar at the bottom: an icon and a caption per page, the current one
 * in the accent color */
static void nav_create(lv_obj_t *parent, struct phone *d)
{
	static const char *const names[TABS] = { N_("Recents"), N_("Contacts"), N_("Keypad") };
	static const enum ui_glyph glyphs[TABS] = { UI_GLYPH_CLOCK, UI_GLYPH_PERSON, UI_GLYPH_DIALPAD };

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
		lv_label_set_text_static(label, _(names[i]));
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
	ui_vibrate(UI_VIBRATE_TAP);
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
	lv_obj_t *scr = ui_app_screen_create(_("Phone"), &header, &body);
	lv_obj_add_event_cb(scr, free_user_data_cb, LV_EVENT_DELETE, d);
	lv_obj_add_event_cb(scr, phone_load_cb, LV_EVENT_SCREEN_LOAD_START, d);
	d->trash = ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, d);
	/* Never shown together: in the same place */
	d->add = ui_header_button(header, LV_SYMBOL_PLUS, add_cb, NULL);
	lv_obj_align(d->add, LV_ALIGN_RIGHT_MID, -12, 0); /* as the first one (ui.c) */
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

	lv_obj_t *contacts = d->pages[TAB_CONTACTS] = ui_box(body);
	lv_obj_set_width(contacts, LV_PCT(100));
	lv_obj_set_flex_grow(contacts, 1);
	lv_obj_set_flex_flow(contacts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(contacts, true);
	lv_obj_set_clickable(contacts, true);
	contacts_list_create(contacts);

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
		key_create(keypad, &keys[i], KEY_SIZE, key_cb, d);

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
	char text[96];
	snprintf(text, sizeof(text), LV_SYMBOL_TRASH "  %s", _("Clear recent calls"));
	d->confirm = ui_confirm_bar(body, ui_intern(text), clear_cb, cancel_cb, d);
	nav_create(body, d);

	/* Only what can be dialed: "+40 744-555" is entered as "+40744555" */
	for (const char *c = number ? number : ""; *c; c++) {
		size_t n = strlen(d->digits);
		if (n < MAX_DIGITS && ((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#'))
			d->digits[n] = *c;
	}
	dialer_update(d);
	/* Opened on Recents, the missed calls are seen (not when back on it from a call: it
	 * may have rung unheard, with Recents under it) */
	if (tab == TAB_RECENTS)
		call_missed_seen();
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
