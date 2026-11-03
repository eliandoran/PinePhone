/*
 * Telegram: signs in to Telegram through TDLib's JSON interface (libtdjson,
 * loaded when the app is first opened). Login only for now: phone number, code,
 * password, then the account's name.
 *
 * TDLib keeps its database in /var/lib/telegram. The API credentials (from
 * my.telegram.org) are read from /var/lib/telegram/api: "<api_id> <api_hash>".
 * The client lives on after the screen is closed; a timer polls it.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui.h"

#define DIR "/var/lib/telegram"
#define API_FILE DIR "/api"
#define LIB "libtdjson.so"
#define POLL_MS 100
#define COLOR_RED 0xff453a
#define COLOR_GREEN 0x30d158

enum state {
	ST_NO_LIB,      /* libtdjson missing */
	ST_NO_API,      /* no API credentials */
	ST_STARTING,    /* waiting for TDLib */
	ST_PHONE,
	ST_CODE,
	ST_PASSWORD,
	ST_READY,
	ST_UNSUPPORTED, /* a step this app does not do (sign up, email) */
	ST_CLOSING,     /* logging out */
};

/* TDLib and the session: outlive the screen */
static struct {
	bool loaded;
	int (*create_client_id)(void);
	void (*send)(int client_id, const char *request);
	const char *(*receive)(double timeout);
	const char *(*execute)(const char *request);
	int client;
	lv_timer_t *poll;
	enum state state;
	bool busy; /* a login request is on its way */
	int api_id;
	char api_hash[64];
	char name[128];   /* account, once ready */
	char detail[192]; /* where the code went, the password hint */
	char error[128];
} tg;

/* The screen, when open */
static struct {
	lv_obj_t *scr;
	lv_obj_t *title;
	lv_obj_t *hint;
	lv_obj_t *ta;
	lv_obj_t *error;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *kb;
} ui;

/* JSON */

/* The string value of the first "key" in json (TDLib writes compact JSON) */
static bool json_str(const char *json, const char *key, char *out, size_t size)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *s = json ? strstr(json, pattern) : NULL;
	if (!s)
		return false;
	s += strlen(pattern);

	size_t n = 0;
	while (*s && *s != '"' && n + 4 < size) {
		if (*s != '\\') {
			out[n++] = *s++;
			continue;
		}
		s++;
		switch (*s) {
		case 'n': out[n++] = '\n'; s++; break;
		case 't': out[n++] = '\t'; s++; break;
		case 'u': {
			unsigned c = 0;
			if (sscanf(s + 1, "%4x", &c) != 1)
				return false;
			s += 5;
			if (c >= 0xd800 && c < 0xe000) /* surrogate pairs: not in our fonts */
				c = '?';
			if (c < 0x80) {
				out[n++] = c;
			} else if (c < 0x800) {
				out[n++] = 0xc0 | c >> 6;
				out[n++] = 0x80 | (c & 0x3f);
			} else {
				out[n++] = 0xe0 | c >> 12;
				out[n++] = 0x80 | (c >> 6 & 0x3f);
				out[n++] = 0x80 | (c & 0x3f);
			}
			break;
		}
		case '\0': break;
		default: out[n++] = *s++; break; /* \" \\ \/ */
		}
	}
	out[n] = '\0';
	return true;
}

/* The "@type" of the object that follows "key" */
static bool json_type_of(const char *json, const char *key, char *out, size_t size)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\":{", key);
	const char *s = strstr(json, pattern);
	return s && json_str(s, "@type", out, size);
}

/* text as a JSON string body */
static void json_escape(char *out, size_t size, const char *text)
{
	size_t n = 0;
	for (; *text && n + 7 < size; text++) {
		unsigned char c = *text;
		if (c == '"' || c == '\\') {
			out[n++] = '\\';
			out[n++] = c;
		} else if (c < 0x20) {
			n += snprintf(out + n, size - n, "\\u%04x", c);
		} else {
			out[n++] = c;
		}
	}
	out[n] = '\0';
}

/* TDLib */

static bool td_load(void)
{
	if (tg.loaded)
		return true;
	void *lib = dlopen(LIB, RTLD_NOW);
	if (!lib) {
		printf("telegram: %s\n", dlerror());
		return false;
	}
	tg.create_client_id = dlsym(lib, "td_create_client_id");
	tg.send = dlsym(lib, "td_send");
	tg.receive = dlsym(lib, "td_receive");
	tg.execute = dlsym(lib, "td_execute");
	if (!tg.create_client_id || !tg.send || !tg.receive || !tg.execute) {
		printf("telegram: %s lacks the JSON interface\n", LIB);
		dlclose(lib);
		return false;
	}
	/* Errors and warnings only, to our log */
	tg.execute("{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}");
	tg.loaded = true;
	return true;
}

static bool api_load(void)
{
	FILE *f = fopen(API_FILE, "r");
	if (!f)
		return false;
	bool ok = fscanf(f, "%d %63s", &tg.api_id, tg.api_hash) == 2 && tg.api_id > 0;
	fclose(f);
	return ok;
}

/* A request; extra tags the answer (see handle()) */
static void td_request(const char *json)
{
	tg.send(tg.client, json);
}

static void client_start(void)
{
	tg.client = tg.create_client_id();
	tg.state = ST_STARTING;
	/* The client starts with its first request */
	td_request("{\"@type\":\"getOption\",\"name\":\"version\"}");
}

static void send_parameters(void)
{
	char buf[512];
	mkdir(DIR, 0700);
	snprintf(buf, sizeof(buf),
		"{\"@type\":\"setTdlibParameters\","
		"\"database_directory\":\"" DIR "\","
		"\"files_directory\":\"" DIR "/files\","
		"\"use_file_database\":true,"
		"\"use_chat_info_database\":true,"
		"\"use_message_database\":true,"
		"\"use_secret_chats\":false,"
		"\"api_id\":%d,\"api_hash\":\"%s\","
		"\"system_language_code\":\"en\","
		"\"device_model\":\"PinePhone\","
		"\"system_version\":\"Linux\","
		"\"application_version\":\"0.1\"}",
		tg.api_id, tg.api_hash);
	td_request(buf);
}

/* Screen */

static void show_state(void)
{
	if (!ui.scr)
		return;

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
		hint = tg.detail;
		placeholder = "Code";
		accept = "0123456789";
		input = true;
		break;
	case ST_PASSWORD:
		title = "Two-step verification";
		hint = tg.detail;
		placeholder = "Password";
		input = true;
		password = true;
		break;
	case ST_READY:
		title = "Logged in";
		hint = tg.name;
		button = "Log out";
		break;
	case ST_UNSUPPORTED:
		title = "Not supported yet";
		hint = tg.detail;
		button = "Log out";
		break;
	case ST_CLOSING:
		title = "Logging out...";
		has_button = false;
		break;
	}

	lv_label_set_text_static(ui.title, title);
	lv_obj_set_style_text_color(ui.title, lv_color_hex(tg.state == ST_READY ? COLOR_GREEN : UI_COLOR_TEXT), 0);
	ui_set_text(ui.hint, hint);
	ui_set_text(ui.error, tg.error);
	lv_obj_set_hidden(ui.error, !tg.error[0]);

	if (input) {
		lv_textarea_set_placeholder_text(ui.ta, placeholder);
		lv_textarea_set_accepted_chars(ui.ta, accept);
		lv_textarea_set_password_mode(ui.ta, password);
	} else {
		ui_keyboard_show(ui.kb, false);
	}
	lv_obj_set_hidden(ui.ta, !input);

	lv_obj_set_hidden(ui.button, !has_button);
	lv_obj_set_state(ui.button, LV_STATE_DISABLED, tg.busy);
	lv_label_set_text_static(ui.button_label, tg.busy ? "Wait..." : button);
	lv_obj_set_style_bg_color(ui.button, lv_color_hex(tg.state == ST_READY || tg.state == ST_UNSUPPORTED ?
		UI_COLOR_BUTTON : UI_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(ui.button_label, lv_color_hex(tg.state == ST_READY || tg.state == ST_UNSUPPORTED ?
		COLOR_RED : 0xffffff), 0);
}

static void set_state(enum state state)
{
	if (state != tg.state && ui.ta)
		lv_textarea_set_text(ui.ta, "");
	tg.state = state;
	show_state();
}

/* Updates and answers */

static void auth_state(const char *json)
{
	char type[64] = "";
	json_type_of(json, "authorization_state", type, sizeof(type));
	tg.busy = false;

	if (!strcmp(type, "authorizationStateWaitTdlibParameters")) {
		send_parameters();
		set_state(ST_STARTING);
	} else if (!strcmp(type, "authorizationStateWaitPhoneNumber")) {
		set_state(ST_PHONE);
	} else if (!strcmp(type, "authorizationStateWaitCode")) {
		char phone[32] = "", via[64] = "";
		json_str(json, "phone_number", phone, sizeof(phone));
		json_type_of(json, "type", via, sizeof(via));
		snprintf(tg.detail, sizeof(tg.detail), "Sent to %s %s.", phone,
			!strcmp(via, "authenticationCodeTypeTelegramMessage") ? "in Telegram on your other devices" :
			!strcmp(via, "authenticationCodeTypeSms") ? "by SMS" :
			!strcmp(via, "authenticationCodeTypeCall") ? "by a phone call" : "");
		set_state(ST_CODE);
	} else if (!strcmp(type, "authorizationStateWaitPassword")) {
		char hint[96] = "";
		json_str(json, "password_hint", hint, sizeof(hint));
		snprintf(tg.detail, sizeof(tg.detail), hint[0] ? "Your password. Hint: %s" : "Your password.", hint);
		set_state(ST_PASSWORD);
	} else if (!strcmp(type, "authorizationStateReady")) {
		tg.error[0] = '\0';
		td_request("{\"@type\":\"getMe\",\"@extra\":\"me\"}");
		set_state(ST_READY);
	} else if (!strcmp(type, "authorizationStateLoggingOut") ||
		   !strcmp(type, "authorizationStateClosing")) {
		set_state(ST_CLOSING);
	} else if (!strcmp(type, "authorizationStateClosed")) {
		/* A closed client cannot be reused: a new one asks for a number again */
		client_start();
		show_state();
	} else {
		/* Sign up, email address, purchase: done in another app for now */
		snprintf(tg.detail, sizeof(tg.detail),
			"This account needs a step this app cannot do (%s). Finish it in another "
			"Telegram app, then log in again.", type);
		set_state(ST_UNSUPPORTED);
	}
}

static void auth_error(const char *json)
{
	char message[96] = "";
	json_str(json, "message", message, sizeof(message));
	printf("telegram: error %s\n", message);
	const char *text =
		!strcmp(message, "PHONE_NUMBER_INVALID") ? "Invalid phone number" :
		!strcmp(message, "PHONE_CODE_INVALID") ? "Wrong code" :
		!strcmp(message, "PHONE_CODE_EXPIRED") ? "The code expired" :
		!strcmp(message, "PASSWORD_HASH_INVALID") ? "Wrong password" :
		!strcmp(message, "API_ID_INVALID") ? "Invalid api_id or api_hash" :
		strncmp(message, "Too Many Requests", 17) ? message : "Too many attempts, try again later";
	snprintf(tg.error, sizeof(tg.error), "%s", text);
	tg.busy = false;
	show_state();
}

static void handle(const char *json)
{
	char type[64] = "", extra[16] = "";
	json_str(json, "@type", type, sizeof(type));
	json_str(json, "@extra", extra, sizeof(extra));

	if (!strcmp(type, "updateAuthorizationState")) {
		auth_state(json);
	} else if (!strcmp(type, "error") && !strcmp(extra, "auth")) {
		auth_error(json);
	} else if (!strcmp(type, "user") && !strcmp(extra, "me")) {
		char first[64] = "", last[64] = "";
		json_str(json, "first_name", first, sizeof(first));
		json_str(json, "last_name", last, sizeof(last));
		snprintf(tg.name, sizeof(tg.name), "As %s%s%s.", first, last[0] ? " " : "", last);
		show_state();
	}
}

static void poll_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	/* A few at a time: the UI runs in the same loop */
	for (int i = 0; i < 32; i++) {
		const char *json = tg.receive(0);
		if (!json)
			break;
		handle(json);
	}
}

/* Starts TDLib once; the state says what is missing otherwise */
static void start(void)
{
	if (tg.poll)
		return;
	if (!td_load()) {
		tg.state = ST_NO_LIB;
		return;
	}
	if (!api_load()) {
		tg.state = ST_NO_API;
		return;
	}
	client_start();
	tg.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}

/* Actions */

static void submit(void)
{
	char text[128], buf[256];
	if (tg.busy)
		return;

	switch (tg.state) {
	case ST_NO_API:
		start();
		show_state();
		return;
	case ST_READY:
	case ST_UNSUPPORTED:
		ui_vibrate(15);
		tg.name[0] = '\0';
		td_request("{\"@type\":\"logOut\"}");
		return;
	case ST_PHONE:
	case ST_CODE:
	case ST_PASSWORD:
		break;
	default:
		return;
	}

	json_escape(text, sizeof(text), lv_textarea_get_text(ui.ta));
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
	tg.busy = true;
	tg.error[0] = '\0';
	ui_keyboard_show(ui.kb, false);
	show_state();
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
		ui_keyboard_show(ui.kb, true);
		break;
	case LV_EVENT_READY: /* the keyboard's Enter key */
		submit();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(ui.kb, false);
		break;
	default:
		break;
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&ui, 0, sizeof(ui));
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

void telegram_open(void)
{
	lv_obj_t *body;
	ui.scr = ui_app_screen_create("Telegram", NULL, &body);
	lv_obj_add_event_cb(ui.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	/* The form shrinks above the keyboard */
	lv_obj_t *form = lv_obj_create(body);
	lv_obj_remove_style_all(form);
	lv_obj_set_width(form, LV_PCT(100));
	lv_obj_set_flex_grow(form, 1);
	lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(form, 32, 0);
	lv_obj_set_style_pad_ver(form, 48, 0);
	lv_obj_set_style_pad_row(form, 24, 0);
	lv_obj_set_scroll_dir(form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(form, LV_SCROLLBAR_MODE_OFF);

	ui.title = text_label(form, &ui_font_32, UI_COLOR_TEXT);
	ui.hint = text_label(form, &ui_font_20, UI_COLOR_DIM);

	ui.ta = lv_textarea_create(form);
	lv_textarea_set_one_line(ui.ta, true);
	lv_textarea_set_max_length(ui.ta, 64);
	lv_obj_set_size(ui.ta, LV_PCT(100), 88);
	lv_obj_set_style_radius(ui.ta, 44, 0);
	lv_obj_set_style_pad_hor(ui.ta, 32, 0);
	lv_obj_set_style_pad_ver(ui.ta, 26, 0);
	lv_obj_set_style_bg_opa(ui.ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ui.ta, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_text_color(ui.ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(ui.ta, 0, 0);
	lv_obj_set_style_border_width(ui.ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ui.ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ui.ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(ui.ta, 0, 0);
	lv_obj_add_event_cb(ui.ta, ta_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(ui.ta, ta_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ui.ta, ta_cb, LV_EVENT_CANCEL, NULL);

	ui.error = text_label(form, &ui_font_20, COLOR_RED);

	ui.button = lv_button_create(form);
	lv_obj_set_size(ui.button, LV_PCT(100), 88);
	lv_obj_set_style_radius(ui.button, 44, 0);
	lv_obj_set_style_shadow_width(ui.button, 0, 0);
	lv_obj_add_event_cb(ui.button, button_cb, LV_EVENT_CLICKED, NULL);
	ui.button_label = lv_label_create(ui.button);
	lv_obj_set_style_text_font(ui.button_label, &ui_font_26, 0);
	lv_obj_center(ui.button_label);

	ui.kb = ui_keyboard_create(body, ui.ta);

	start();
	show_state();
	ui_push(ui.scr);
}
