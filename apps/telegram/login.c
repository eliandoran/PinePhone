/*
 * Telegram's login: one form following TDLib's authorization states (phone
 * number, code, password) until the account is ready; logging out.
 */

#include <stdio.h>
#include <string.h>

#include "telegram.h"

/* Outlive the screen */
static struct {
	bool busy;        /* a login request is on its way */
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

	const char *title = "", *hint = "", *placeholder = "", *button = _("Next");
	const char *accept = NULL;
	char text[256]; /* a hint made here */
	bool input = false, has_button = true, password = false;
	switch (tg.state) {
	case ST_NO_LIB:
		title = _("Telegram is not installed");
		snprintf(text, sizeof(text), _("This firmware has no TDLib (%s)."), LIB);
		hint = text;
		has_button = false;
		break;
	case ST_NO_API:
		title = _("No API credentials");
		snprintf(text, sizeof(text), _("Create an app on my.telegram.org and put its api_id and api_hash "
			"in %s, separated by a space."), API_FILE);
		hint = text;
		button = _("Try again");
		break;
	case ST_STARTING:
		title = _("Connecting...");
		has_button = false;
		break;
	case ST_PHONE:
		title = _("Your phone number");
		hint = _("With the country code, e.g. +33 6 12 34 56 78.");
		placeholder = "+";
		accept = "+0123456789 ";
		input = true;
		break;
	case ST_CODE:
		title = _("Enter the code");
		hint = login.detail;
		placeholder = _("Code");
		accept = "0123456789";
		input = true;
		break;
	case ST_PASSWORD:
		title = _("Two-step verification");
		hint = login.detail;
		placeholder = _("Password");
		input = true;
		password = true;
		break;
	case ST_READY: /* the chats and the profile instead */
		has_button = false;
		break;
	case ST_UNSUPPORTED:
		title = _("Not supported yet");
		hint = login.detail;
		button = _("Log out");
		break;
	case ST_CLOSING:
		title = _("Logging out...");
		has_button = false;
		break;
	}

	lv_label_set_text_static(form.title, title);
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

	bool logout = tg.state == ST_UNSUPPORTED;
	lv_obj_set_hidden(form.button, !has_button);
	lv_obj_set_state(form.button, LV_STATE_DISABLED, login.busy);
	lv_label_set_text_static(form.button_label, login.busy ? _("Wait...") : button);
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
		snprintf(login.detail, sizeof(login.detail),
			!strcmp(via, "authenticationCodeTypeTelegramMessage") ? _("Sent to %s in Telegram on your other devices.") :
			!strcmp(via, "authenticationCodeTypeSms") ? _("Sent to %s by SMS.") :
			!strcmp(via, "authenticationCodeTypeCall") ? _("Sent to %s by a phone call.") : _("Sent to %s."),
			json_str(json_get(info, "phone_number"), ""));
		set_state(ST_CODE);
	} else if (!strcmp(type, "authorizationStateWaitPassword")) {
		const char *hint = json_str(json_get(st, "password_hint"), "");
		snprintf(login.detail, sizeof(login.detail), hint[0] ? _("Your password. Hint: %s") : _("Your password."), hint);
		set_state(ST_PASSWORD);
	} else if (!strcmp(type, "authorizationStateReady")) {
		login.error[0] = '\0';
		td_request("{\"@type\":\"getMe\",\"@extra\":\"me\"}");
		tg.state = ST_READY;
		load_first_chats();
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
			_("This account needs a step this app cannot do (%s). Finish it in another "
			"Telegram app, then log in again."), type);
		set_state(ST_UNSUPPORTED);
	}
}

void login_error(const struct json *u)
{
	const char *message = json_str(json_get(u, "message"), "");
	printf("telegram: error %s\n", message);
	const char *text =
		!strcmp(message, "PHONE_NUMBER_INVALID") ? _("Invalid phone number") :
		!strcmp(message, "PHONE_CODE_INVALID") ? _("Wrong code") :
		!strcmp(message, "PHONE_CODE_EXPIRED") ? _("The code expired") :
		!strcmp(message, "PASSWORD_HASH_INVALID") ? _("Wrong password") :
		!strcmp(message, "API_ID_INVALID") ? _("Invalid api_id or api_hash") :
		strncmp(message, "Too Many Requests", 17) ? message : _("Too many attempts, try again later");
	snprintf(login.error, sizeof(login.error), "%s", text);
	login.busy = false;
	telegram_show();
}

/* getMe: the account, then its bio */
void login_me(const struct json *u)
{
	tg.me = json_int(json_get(u, "id"), 0);
	tg.chats_changed = true;
	profile_me(u);
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"@type\":\"getUserFullInfo\",\"user_id\":%lld,\"@extra\":\"full info\"}", tg.me); /* NON-NLS */
	td_request(buf);
	telegram_show();
}

/* Actions */

void log_out(void)
{
	ui_vibrate(UI_VIBRATE_TAP);
	td_request("{\"@type\":\"logOut\"}");
}

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
	case ST_UNSUPPORTED:
		log_out();
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

	form.title = text_label(form.form, UI_FONT_HEADLINE, UI_COLOR_TEXT);
	form.hint = text_label(form.form, UI_FONT_BODY, UI_COLOR_DIM);

	form.ta = ui_text_field(form.form, 88, 64, ta_cb, NULL);
	lv_obj_set_width(form.ta, LV_PCT(100));

	form.error = text_label(form.form, UI_FONT_BODY, UI_COLOR_RED);

	form.button = lv_button_create(form.form);
	lv_obj_set_size(form.button, LV_PCT(100), 88);
	lv_obj_set_style_radius(form.button, 44, 0);
	lv_obj_set_style_shadow_width(form.button, 0, 0);
	lv_obj_add_event_cb(form.button, button_cb, LV_EVENT_CLICKED, NULL);
	form.button_label = lv_label_create(form.button);
	lv_obj_set_style_text_font(form.button_label, UI_FONT_TITLE, 0);
	lv_obj_center(form.button_label);

	/* Right under the form: the chat list is hidden while it shows */
	form.kb = ui_keyboard_create(body, form.ta);
}
