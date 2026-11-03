/*
 * Telegram's login: one form following TDLib's authorization states (phone
 * number, code, password), then the account with a log out button.
 */

#include <stdio.h>
#include <string.h>

#include "telegram.h"

/* Outlive the screen */
static struct {
	bool busy;        /* a login request is on its way */
	char name[128];   /* account, once ready */
	char detail[192]; /* where the code went, the password hint */
	char error[128];
} login;

/* The form, when the screen is open */
static struct {
	lv_obj_t *form;
	lv_obj_t *title;
	lv_obj_t *hint;
	lv_obj_t *ta;
	lv_obj_t *error;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *kb;
} form;

void login_show(bool shown)
{
	if (!form.form)
		return;
	lv_obj_set_hidden(form.form, !shown);
	if (!shown) {
		ui_keyboard_show(form.kb, false);
		return;
	}

	const char *title = "", *hint = "", *placeholder = "", *button = "Next";
	const char *accept = NULL;
	bool input = false, has_button = true, password = false;
	switch (tg.state) {
	case ST_NO_LIB:
		title = "Telegram is not installed";
		hint = "This firmware has no TDLib (" LIB ").";
		has_button = false;
		break;
	case ST_NO_API:
		title = "No API credentials";
		hint = "Create an app on my.telegram.org and put its api_id and api_hash "
			"in " API_FILE ", separated by a space.";
		button = "Try again";
		break;
	case ST_STARTING:
		title = "Connecting...";
		has_button = false;
		break;
	case ST_PHONE:
		title = "Your phone number";
		hint = "With the country code, e.g. +33 6 12 34 56 78.";
		placeholder = "+";
		accept = "+0123456789 ";
		input = true;
		break;
	case ST_CODE:
		title = "Enter the code";
		hint = login.detail;
		placeholder = "Code";
		accept = "0123456789";
		input = true;
		break;
	case ST_PASSWORD:
		title = "Two-step verification";
		hint = login.detail;
		placeholder = "Password";
		input = true;
		password = true;
		break;
	case ST_READY:
		title = "Logged in";
		hint = login.name;
		button = "Log out";
		break;
	case ST_UNSUPPORTED:
		title = "Not supported yet";
		hint = login.detail;
		button = "Log out";
		break;
	case ST_CLOSING:
		title = "Logging out...";
		has_button = false;
		break;
	}

	lv_label_set_text_static(form.title, title);
	lv_obj_set_style_text_color(form.title, lv_color_hex(tg.state == ST_READY ? UI_COLOR_GREEN : UI_COLOR_TEXT), 0);
	ui_set_text(form.hint, hint);
	ui_set_text(form.error, login.error);
	lv_obj_set_hidden(form.error, !login.error[0]);

	if (input) {
		lv_textarea_set_placeholder_text(form.ta, placeholder);
		lv_textarea_set_accepted_chars(form.ta, accept);
		lv_textarea_set_password_mode(form.ta, password);
	} else {
		ui_keyboard_show(form.kb, false);
	}
	lv_obj_set_hidden(form.ta, !input);

	bool logout = tg.state == ST_READY || tg.state == ST_UNSUPPORTED;
	lv_obj_set_hidden(form.button, !has_button);
	lv_obj_set_state(form.button, LV_STATE_DISABLED, login.busy);
	lv_label_set_text_static(form.button_label, login.busy ? "Wait..." : button);
	lv_obj_set_style_bg_color(form.button, lv_color_hex(logout ? UI_COLOR_BUTTON : UI_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(form.button_label, logout ? lv_color_hex(UI_COLOR_RED) : lv_color_white(), 0);
}

static void set_state(enum state state)
{
	if (state != tg.state && form.ta)
		lv_textarea_set_text(form.ta, "");
	tg.state = state;
	telegram_show();
}

/* Updates and answers */

void login_auth_state(const struct json *u)
{
	const struct json *st = json_get(u, "authorization_state");
	const char *type = json_type(st);
	login.busy = false;

	if (!strcmp(type, "authorizationStateWaitTdlibParameters")) {
		send_parameters();
		set_state(ST_STARTING);
	} else if (!strcmp(type, "authorizationStateWaitPhoneNumber")) {
		set_state(ST_PHONE);
	} else if (!strcmp(type, "authorizationStateWaitCode")) {
		const struct json *info = json_get(st, "code_info");
		const char *via = json_type(json_get(info, "type"));
		snprintf(login.detail, sizeof(login.detail), "Sent to %s %s.",
			json_str(json_get(info, "phone_number"), ""),
			!strcmp(via, "authenticationCodeTypeTelegramMessage") ? "in Telegram on your other devices" :
			!strcmp(via, "authenticationCodeTypeSms") ? "by SMS" :
			!strcmp(via, "authenticationCodeTypeCall") ? "by a phone call" : "");
		set_state(ST_CODE);
	} else if (!strcmp(type, "authorizationStateWaitPassword")) {
		const char *hint = json_str(json_get(st, "password_hint"), "");
		snprintf(login.detail, sizeof(login.detail), hint[0] ? "Your password. Hint: %s" : "Your password.", hint);
		set_state(ST_PASSWORD);
	} else if (!strcmp(type, "authorizationStateReady")) {
		login.error[0] = '\0';
		td_request("{\"@type\":\"getMe\",\"@extra\":\"me\"}");
		tg.state = ST_READY;
		load_chats();
		telegram_show();
	} else if (!strcmp(type, "authorizationStateLoggingOut") ||
		   !strcmp(type, "authorizationStateClosing")) {
		set_state(ST_CLOSING);
	} else if (!strcmp(type, "authorizationStateClosed")) {
		/* A closed client cannot be reused: a new one asks for a number again */
		client_start();
		telegram_show();
	} else {
		/* Sign up, email address, purchase: done in another app for now */
		snprintf(login.detail, sizeof(login.detail),
			"This account needs a step this app cannot do (%s). Finish it in another "
			"Telegram app, then log in again.", type);
		set_state(ST_UNSUPPORTED);
	}
}

void login_error(const struct json *u)
{
	const char *message = json_str(json_get(u, "message"), "");
	printf("telegram: error %s\n", message);
	const char *text =
		!strcmp(message, "PHONE_NUMBER_INVALID") ? "Invalid phone number" :
		!strcmp(message, "PHONE_CODE_INVALID") ? "Wrong code" :
		!strcmp(message, "PHONE_CODE_EXPIRED") ? "The code expired" :
		!strcmp(message, "PASSWORD_HASH_INVALID") ? "Wrong password" :
		!strcmp(message, "API_ID_INVALID") ? "Invalid api_id or api_hash" :
		strncmp(message, "Too Many Requests", 17) ? message : "Too many attempts, try again later";
	snprintf(login.error, sizeof(login.error), "%s", text);
	login.busy = false;
	telegram_show();
}

void login_me(const struct json *u)
{
	const char *first = json_str(json_get(u, "first_name"), "");
	const char *last = json_str(json_get(u, "last_name"), "");
	snprintf(login.name, sizeof(login.name), "As %s%s%s.", first, last[0] ? " " : "", last);
	telegram_show();
}

/* Actions */

static void submit(void)
{
	char text[128], buf[256];
	if (login.busy)
		return;

	switch (tg.state) {
	case ST_NO_API:
		telegram_start();
		telegram_show();
		return;
	case ST_READY:
	case ST_UNSUPPORTED:
		ui_vibrate(15);
		login.name[0] = '\0';
		td_request("{\"@type\":\"logOut\"}");
		return;
	case ST_PHONE:
	case ST_CODE:
	case ST_PASSWORD:
		break;
	default:
		return;
	}

	json_escape(text, sizeof(text), lv_textarea_get_text(form.ta));
	if (!text[0])
		return;
	if (tg.state == ST_PHONE)
		snprintf(buf, sizeof(buf), "{\"@type\":\"setAuthenticationPhoneNumber\","
			"\"phone_number\":\"%s\",\"@extra\":\"auth\"}", text);
	else if (tg.state == ST_CODE)
		snprintf(buf, sizeof(buf), "{\"@type\":\"checkAuthenticationCode\","
			"\"code\":\"%s\",\"@extra\":\"auth\"}", text);
	else
		snprintf(buf, sizeof(buf), "{\"@type\":\"checkAuthenticationPassword\","
			"\"password\":\"%s\",\"@extra\":\"auth\"}", text);
	td_request(buf);
	login.busy = true;
	login.error[0] = '\0';
	ui_keyboard_show(form.kb, false);
	telegram_show();
}

static void button_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	submit();
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(form.kb, true);
		break;
	case LV_EVENT_READY: /* the keyboard's Enter key */
		submit();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(form.kb, false);
		break;
	default:
		break;
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&form, 0, sizeof(form));
}

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

void login_create(lv_obj_t *body)
{
	/* Shrinks above the keyboard */
	form.form = lv_obj_create(body);
	lv_obj_remove_style_all(form.form);
	lv_obj_set_width(form.form, LV_PCT(100));
	lv_obj_set_flex_grow(form.form, 1);
	lv_obj_set_flex_flow(form.form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(form.form, 32, 0);
	lv_obj_set_style_pad_ver(form.form, 48, 0);
	lv_obj_set_style_pad_row(form.form, 24, 0);
	lv_obj_set_scroll_dir(form.form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(form.form, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(form.form, delete_cb, LV_EVENT_DELETE, NULL);

	form.title = text_label(form.form, &ui_font_32, UI_COLOR_TEXT);
	form.hint = text_label(form.form, &ui_font_20, UI_COLOR_DIM);

	form.ta = ui_text_field(form.form, 88, 64, ta_cb, NULL);
	lv_obj_set_width(form.ta, LV_PCT(100));

	form.error = text_label(form.form, &ui_font_20, UI_COLOR_RED);

	form.button = lv_button_create(form.form);
	lv_obj_set_size(form.button, LV_PCT(100), 88);
	lv_obj_set_style_radius(form.button, 44, 0);
	lv_obj_set_style_shadow_width(form.button, 0, 0);
	lv_obj_add_event_cb(form.button, button_cb, LV_EVENT_CLICKED, NULL);
	form.button_label = lv_label_create(form.button);
	lv_obj_set_style_text_font(form.button_label, &ui_font_26, 0);
	lv_obj_center(form.button_label);

	/* Right under the form: the chat list is hidden while it shows */
	form.kb = ui_keyboard_create(body, form.ta);
}
