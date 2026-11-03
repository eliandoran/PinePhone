/*
 * Telegram: signs in to Telegram through TDLib's JSON interface (libtdjson,
 * loaded when the app is first opened), then lists the chats: name, last
 * message, time, unread count. The settings button shows the account and logs out.
 *
 * TDLib keeps its database in /var/lib/telegram. The API credentials (from
 * my.telegram.org) are read from /var/lib/telegram/api: "<api_id> <api_hash>".
 * The client lives on after the screen is closed; a timer polls it, and the
 * chats are kept from its updates.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "json.h"
#include "ui.h"

#define DIR "/var/lib/telegram"
#define API_FILE DIR "/api"
#define LIB "libtdjson.so"
#define POLL_MS 100
#define PAGE 50          /* chats loaded at a time */
#define PREVIEW_MAX 120  /* bytes of the last message shown */
#define ROW_H 120
#define AVATAR 80
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

struct chat {
	long long id;
	long long order; /* in the main list, 0: not in it */
	char *title;
	char *preview;   /* last message */
	long long date;  /* of the last message */
	int unread;
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

	struct chat *chats;
	unsigned chat_count, chat_cap;
	bool chats_changed;
	bool loading;    /* loadChats on its way */
	bool all_loaded; /* the main list has no more chats */
} tg;

struct row {
	lv_obj_t *obj, *avatar, *initial, *title, *time, *preview, *badge, *count;
	long long chat_id;
};

/* The screen, when open */
static struct {
	lv_obj_t *scr;
	lv_obj_t *account;
	bool show_account;
	/* Login and account */
	lv_obj_t *form;
	lv_obj_t *title;
	lv_obj_t *hint;
	lv_obj_t *ta;
	lv_obj_t *error;
	lv_obj_t *button;
	lv_obj_t *button_label;
	lv_obj_t *kb;
	/* Chats */
	lv_obj_t *list;
	lv_obj_t *card;
	lv_obj_t *empty;
	struct row *rows;
	unsigned row_count;
} ui;

static void show_state(void);

/* Text */

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

static void set_str(char **dst, const char *src)
{
	free(*dst);
	*dst = strdup(src ? src : "");
}

/* One line of at most PREVIEW_MAX bytes, whitespace runs as one space */
static char *one_line(const char *prefix, const char *text)
{
	char buf[PREVIEW_MAX + 1];
	size_t n = snprintf(buf, sizeof(buf), "%s", prefix);
	bool space = false;
	while (*text) {
		if (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r') {
			space = n > 0;
			text++;
			continue;
		}
		/* Whole characters only */
		unsigned char c = *text;
		size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		if (n + space + len > PREVIEW_MAX || strnlen(text, len) < len)
			break;
		if (space)
			buf[n++] = ' ';
		space = false;
		memcpy(buf + n, text, len);
		n += len;
		text += len;
	}
	buf[n] = '\0';
	return strdup(buf);
}

static char *message_preview(const struct json *msg)
{
	if (!msg)
		return strdup("");
	const struct json *content = json_get(msg, "content");
	const char *type = json_type(content);
	const char *text =
		!strcmp(type, "messageText") ? json_str(json_get(json_get(content, "text"), "text"), "") :
		!strcmp(type, "messagePhoto") ? "Photo" :
		!strcmp(type, "messageVideo") ? "Video" :
		!strcmp(type, "messageAnimation") ? "GIF" :
		!strcmp(type, "messageSticker") ? "Sticker" :
		!strcmp(type, "messageVoiceNote") ? "Voice message" :
		!strcmp(type, "messageVideoNote") ? "Video message" :
		!strcmp(type, "messageAudio") ? "Audio" :
		!strcmp(type, "messageDocument") ? "File" :
		!strcmp(type, "messageContact") ? "Contact" :
		!strcmp(type, "messageLocation") ? "Location" :
		!strcmp(type, "messagePoll") ? "Poll" :
		!strcmp(type, "messageCall") ? "Call" : "Message";
	return one_line(json_bool(json_get(msg, "is_outgoing")) ? "You: " : "", text);
}

/* Today: the time; this week: the day; otherwise the date */
static void when_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	const char *fmt = tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday ? "%H:%M" :
	                  now - t < 6 * 24 * 3600 ? "%a" :
	                  tm.tm_year == today.tm_year ? "%-d %b" : "%-d %b %Y";
	strftime(buf, size, fmt, &tm);
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

/* A request; "@extra" tags its answer (see handle()) */
static void td_request(const char *json)
{
	tg.send(tg.client, json);
}

static void chats_clear(void)
{
	for (unsigned i = 0; i < tg.chat_count; i++) {
		free(tg.chats[i].title);
		free(tg.chats[i].preview);
	}
	free(tg.chats);
	tg.chats = NULL;
	tg.chat_count = tg.chat_cap = 0;
	tg.loading = tg.all_loaded = false;
	tg.chats_changed = true;
}

static void client_start(void)
{
	chats_clear();
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

/* The next page of the main chat list: they arrive as updates */
static void load_chats(void)
{
	if (tg.loading || tg.all_loaded || tg.state != ST_READY)
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"@type\":\"loadChats\",\"chat_list\":{\"@type\":\"chatListMain\"},"
		"\"limit\":%d,\"@extra\":\"chats\"}", PAGE);
	td_request(buf);
	tg.loading = true;
}

/* Chats */

static struct chat *chat_find(long long id)
{
	for (unsigned i = 0; i < tg.chat_count; i++)
		if (tg.chats[i].id == id)
			return &tg.chats[i];
	return NULL;
}

static struct chat *chat_add(long long id)
{
	struct chat *c = chat_find(id);
	if (c)
		return c;
	if (tg.chat_count == tg.chat_cap) {
		unsigned cap = tg.chat_cap ? tg.chat_cap * 2 : 64;
		struct chat *chats = realloc(tg.chats, cap * sizeof(*chats));
		if (!chats)
			return NULL;
		tg.chats = chats;
		tg.chat_cap = cap;
	}
	c = &tg.chats[tg.chat_count++];
	memset(c, 0, sizeof(*c));
	c->id = id;
	c->title = strdup("");
	c->preview = strdup("");
	return c;
}

/* A chatPosition: only the main list is shown */
static void chat_position(struct chat *c, const struct json *pos)
{
	if (strcmp(json_type(json_get(pos, "list")), "chatListMain"))
		return;
	c->order = json_int(json_get(pos, "order"), 0);
}

static void chat_positions(struct chat *c, const struct json *positions)
{
	for (const struct json *p = positions ? positions->child : NULL; p; p = p->next)
		chat_position(c, p);
}

static void chat_last_message(struct chat *c, const struct json *msg)
{
	free(c->preview);
	c->preview = message_preview(msg);
	c->date = json_int(json_get(msg, "date"), 0);
}

/* Updates of the chat list; false if the update is not one of them */
static bool chat_update(const char *type, const struct json *u)
{
	struct chat *c;
	if (!strcmp(type, "updateNewChat")) {
		const struct json *chat = json_get(u, "chat");
		if (!(c = chat_add(json_int(json_get(chat, "id"), 0))))
			return true;
		set_str(&c->title, json_str(json_get(chat, "title"), ""));
		c->unread = json_int(json_get(chat, "unread_count"), 0);
		chat_last_message(c, json_get(chat, "last_message"));
		chat_positions(c, json_get(chat, "positions"));
	} else if (!strncmp(type, "updateChat", 10)) {
		if (!(c = chat_find(json_int(json_get(u, "chat_id"), 0))))
			return true;
		if (!strcmp(type, "updateChatTitle"))
			set_str(&c->title, json_str(json_get(u, "title"), ""));
		else if (!strcmp(type, "updateChatPosition"))
			chat_position(c, json_get(u, "position"));
		else if (!strcmp(type, "updateChatLastMessage"))
			chat_last_message(c, json_get(u, "last_message"));
		else if (!strcmp(type, "updateChatReadInbox"))
			c->unread = json_int(json_get(u, "unread_count"), 0);
		else if (strcmp(type, "updateChatDraftMessage"))
			return false;
		/* Last message and draft updates carry the new positions */
		chat_positions(c, json_get(u, "positions"));
	} else {
		return false;
	}
	tg.chats_changed = true;
	return true;
}

/* Chat list screen */

static const uint32_t avatar_colors[] = {
	0xff6b6b, 0xff9f43, 0xa66cff, 0x30d158, 0x64d2ff, 0x0a84ff, 0xff375f,
};

static lv_obj_t *one_line_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

static lv_obj_t *line_create(lv_obj_t *parent)
{
	lv_obj_t *line = lv_obj_create(parent);
	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 16, 0);
	lv_obj_set_clickable(line, false);
	lv_obj_set_scrollable(line, false);
	return line;
}

static void row_create(struct row *r)
{
	r->obj = lv_obj_create(ui.card);
	lv_obj_remove_style_all(r->obj);
	lv_obj_set_size(r->obj, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(r->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(r->obj, 24, 0);
	lv_obj_set_style_pad_column(r->obj, 20, 0);
	lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(r->obj, lv_color_hex(UI_COLOR_LINE), 0);
	lv_obj_set_clickable(r->obj, false);
	lv_obj_set_scrollable(r->obj, false);

	r->avatar = lv_obj_create(r->obj);
	lv_obj_remove_style_all(r->avatar);
	lv_obj_set_size(r->avatar, AVATAR, AVATAR);
	lv_obj_set_style_radius(r->avatar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(r->avatar, LV_OPA_COVER, 0);
	lv_obj_set_clickable(r->avatar, false);
	lv_obj_set_scrollable(r->avatar, false);
	r->initial = lv_label_create(r->avatar);
	lv_obj_set_style_text_font(r->initial, &ui_font_32, 0);
	lv_obj_set_style_text_color(r->initial, lv_color_white(), 0);
	lv_obj_center(r->initial);

	lv_obj_t *text = lv_obj_create(r->obj);
	lv_obj_remove_style_all(text);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 6, 0);
	lv_obj_set_clickable(text, false);
	lv_obj_set_scrollable(text, false);

	lv_obj_t *top = line_create(text);
	r->title = one_line_label(top, &ui_font_26, UI_COLOR_TEXT);
	lv_obj_set_flex_grow(r->title, 1);
	r->time = lv_label_create(top);
	lv_obj_set_style_text_font(r->time, &ui_font_20, 0);
	lv_obj_set_style_text_color(r->time, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *bottom = line_create(text);
	r->preview = one_line_label(bottom, &ui_font_20, UI_COLOR_DIM);
	lv_obj_set_flex_grow(r->preview, 1);
	r->badge = lv_obj_create(bottom);
	lv_obj_remove_style_all(r->badge);
	lv_obj_set_size(r->badge, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_min_width(r->badge, 36, 0);
	lv_obj_set_style_pad_hor(r->badge, 10, 0);
	lv_obj_set_style_radius(r->badge, 18, 0);
	lv_obj_set_style_bg_opa(r->badge, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(r->badge, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_clickable(r->badge, false);
	lv_obj_set_scrollable(r->badge, false);
	r->count = lv_label_create(r->badge);
	lv_obj_set_style_text_font(r->count, &ui_font_20, 0);
	lv_obj_set_style_text_color(r->count, lv_color_white(), 0);
	lv_obj_center(r->count);

	r->chat_id = 0;
}

/* Copies text into the label if it changed (titles and messages: any text) */
static void label_update(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void row_update(struct row *r, const struct chat *c, bool divider)
{
	if (r->chat_id != c->id) {
		r->chat_id = c->id;
		unsigned long long id = c->id < 0 ? -c->id : c->id;
		lv_obj_set_style_bg_color(r->avatar, lv_color_hex(avatar_colors[id % 7]), 0);
	}

	/* First character of the title, upper case if ASCII */
	char initial[5] = "";
	size_t n = 1;
	while (c->title[n] && ((unsigned char)c->title[n] & 0xc0) == 0x80 && n < 4)
		n++;
	if (c->title[0]) {
		memcpy(initial, c->title, n);
		initial[n] = '\0';
		if (initial[0] >= 'a' && initial[0] <= 'z')
			initial[0] -= 'a' - 'A';
	}
	ui_set_text(r->initial, initial);

	label_update(r->title, c->title);
	label_update(r->preview, c->preview);
	char when[32] = "";
	if (c->date)
		when_format(when, sizeof(when), c->date);
	ui_set_text(r->time, when);

	lv_obj_set_hidden(r->badge, !c->unread);
	if (c->unread)
		ui_set_text_fmt(r->count, "%d", c->unread);
	lv_obj_set_style_border_width(r->obj, divider, 0);
}

static int chat_cmp(const void *a, const void *b)
{
	const struct chat *x = *(const struct chat **)a, *y = *(const struct chat **)b;
	return (x->order < y->order) - (x->order > y->order);
}

static void list_refresh(void)
{
	tg.chats_changed = false;

	struct chat **sorted = malloc((tg.chat_count + 1) * sizeof(*sorted));
	if (!sorted)
		return;
	unsigned count = 0;
	for (unsigned i = 0; i < tg.chat_count; i++)
		if (tg.chats[i].order)
			sorted[count++] = &tg.chats[i];
	qsort(sorted, count, sizeof(*sorted), chat_cmp);

	if (count > ui.row_count) {
		struct row *rows = realloc(ui.rows, count * sizeof(*rows));
		if (!rows) {
			free(sorted);
			return;
		}
		ui.rows = rows;
		while (ui.row_count < count)
			row_create(&ui.rows[ui.row_count++]);
	}
	for (unsigned i = 0; i < ui.row_count; i++) {
		lv_obj_set_hidden(ui.rows[i].obj, i >= count);
		if (i < count)
			row_update(&ui.rows[i], sorted[i], i + 1 < count);
	}
	free(sorted);

	lv_obj_set_hidden(ui.card, !count);
	lv_obj_set_hidden(ui.empty, count);
	lv_label_set_text_static(ui.empty, tg.all_loaded ? "No chats" : "Loading chats...");
}

/* Near the end of the list: the next page */
static void list_scroll_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (lv_obj_get_scroll_bottom(ui.list) < 4 * ROW_H)
		load_chats();
}

/* Login and account */

static void show_state(void)
{
	if (!ui.scr)
		return;

	bool chats = tg.state == ST_READY && !ui.show_account;
	lv_obj_set_hidden(ui.account, tg.state != ST_READY);
	lv_obj_set_hidden(ui.list, !chats);
	lv_obj_set_hidden(ui.form, chats);
	if (chats) {
		ui_keyboard_show(ui.kb, false);
		list_refresh();
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

	bool logout = tg.state == ST_READY || tg.state == ST_UNSUPPORTED;
	lv_obj_set_hidden(ui.button, !has_button);
	lv_obj_set_state(ui.button, LV_STATE_DISABLED, tg.busy);
	lv_label_set_text_static(ui.button_label, tg.busy ? "Wait..." : button);
	lv_obj_set_style_bg_color(ui.button, lv_color_hex(logout ? UI_COLOR_BUTTON : UI_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(ui.button_label, logout ? lv_color_hex(COLOR_RED) : lv_color_white(), 0);
}

static void set_state(enum state state)
{
	if (state != tg.state && ui.ta)
		lv_textarea_set_text(ui.ta, "");
	tg.state = state;
	show_state();
}

/* Updates and answers */

static void auth_state(const struct json *u)
{
	const struct json *st = json_get(u, "authorization_state");
	const char *type = json_type(st);
	tg.busy = false;

	if (!strcmp(type, "authorizationStateWaitTdlibParameters")) {
		send_parameters();
		set_state(ST_STARTING);
	} else if (!strcmp(type, "authorizationStateWaitPhoneNumber")) {
		set_state(ST_PHONE);
	} else if (!strcmp(type, "authorizationStateWaitCode")) {
		const struct json *info = json_get(st, "code_info");
		const char *via = json_type(json_get(info, "type"));
		snprintf(tg.detail, sizeof(tg.detail), "Sent to %s %s.",
			json_str(json_get(info, "phone_number"), ""),
			!strcmp(via, "authenticationCodeTypeTelegramMessage") ? "in Telegram on your other devices" :
			!strcmp(via, "authenticationCodeTypeSms") ? "by SMS" :
			!strcmp(via, "authenticationCodeTypeCall") ? "by a phone call" : "");
		set_state(ST_CODE);
	} else if (!strcmp(type, "authorizationStateWaitPassword")) {
		const char *hint = json_str(json_get(st, "password_hint"), "");
		snprintf(tg.detail, sizeof(tg.detail), hint[0] ? "Your password. Hint: %s" : "Your password.", hint);
		set_state(ST_PASSWORD);
	} else if (!strcmp(type, "authorizationStateReady")) {
		tg.error[0] = '\0';
		td_request("{\"@type\":\"getMe\",\"@extra\":\"me\"}");
		tg.state = ST_READY;
		load_chats();
		show_state();
	} else if (!strcmp(type, "authorizationStateLoggingOut") ||
		   !strcmp(type, "authorizationStateClosing")) {
		set_state(ST_CLOSING);
	} else if (!strcmp(type, "authorizationStateClosed")) {
		/* A closed client cannot be reused: a new one asks for a number again */
		ui.show_account = false;
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

static void auth_error(const struct json *u)
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
	snprintf(tg.error, sizeof(tg.error), "%s", text);
	tg.busy = false;
	show_state();
}

static void handle(const char *text)
{
	struct json *u = json_parse(text);
	if (!u) {
		printf("telegram: unreadable update %.80s\n", text);
		return;
	}
	const char *type = json_type(u);
	const char *extra = json_str(json_get(u, "@extra"), "");

	if (!strcmp(type, "updateAuthorizationState")) {
		auth_state(u);
	} else if (chat_update(type, u)) {
		/* shown after this batch of updates */
	} else if (!strcmp(extra, "chats")) {
		/* loadChats: 404 when the list has no more chats */
		tg.loading = false;
		if (!strcmp(type, "error")) {
			if (json_int(json_get(u, "code"), 0) == 404)
				tg.all_loaded = true;
			else
				printf("telegram: loadChats: %s\n", json_str(json_get(u, "message"), ""));
		}
		tg.chats_changed = true;
	} else if (!strcmp(type, "error") && !strcmp(extra, "auth")) {
		auth_error(u);
	} else if (!strcmp(type, "user") && !strcmp(extra, "me")) {
		const char *first = json_str(json_get(u, "first_name"), "");
		const char *last = json_str(json_get(u, "last_name"), "");
		snprintf(tg.name, sizeof(tg.name), "As %s%s%s.", first, last[0] ? " " : "", last);
		show_state();
	}
	json_free(u);
}

static void poll_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	/* A few at a time: the UI runs in the same loop */
	for (int i = 0; i < 64; i++) {
		const char *json = tg.receive(0);
		if (!json)
			break;
		handle(json);
	}
	if (tg.chats_changed && ui.list && !lv_obj_is_hidden(ui.list))
		list_refresh();
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

/* The account (and log out) instead of the chats, and back */
static void account_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui.show_account = !ui.show_account;
	show_state();
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
	free(ui.rows);
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

static void form_create(lv_obj_t *body)
{
	/* Shrinks above the keyboard */
	ui.form = lv_obj_create(body);
	lv_obj_remove_style_all(ui.form);
	lv_obj_set_width(ui.form, LV_PCT(100));
	lv_obj_set_flex_grow(ui.form, 1);
	lv_obj_set_flex_flow(ui.form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(ui.form, 32, 0);
	lv_obj_set_style_pad_ver(ui.form, 48, 0);
	lv_obj_set_style_pad_row(ui.form, 24, 0);
	lv_obj_set_scroll_dir(ui.form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(ui.form, LV_SCROLLBAR_MODE_OFF);

	ui.title = text_label(ui.form, &ui_font_32, UI_COLOR_TEXT);
	ui.hint = text_label(ui.form, &ui_font_20, UI_COLOR_DIM);

	ui.ta = lv_textarea_create(ui.form);
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

	ui.error = text_label(ui.form, &ui_font_20, COLOR_RED);

	ui.button = lv_button_create(ui.form);
	lv_obj_set_size(ui.button, LV_PCT(100), 88);
	lv_obj_set_style_radius(ui.button, 44, 0);
	lv_obj_set_style_shadow_width(ui.button, 0, 0);
	lv_obj_add_event_cb(ui.button, button_cb, LV_EVENT_CLICKED, NULL);
	ui.button_label = lv_label_create(ui.button);
	lv_obj_set_style_text_font(ui.button_label, &ui_font_26, 0);
	lv_obj_center(ui.button_label);
}

static void list_create(lv_obj_t *body)
{
	ui.list = lv_obj_create(body);
	lv_obj_remove_style_all(ui.list);
	lv_obj_set_width(ui.list, LV_PCT(100));
	lv_obj_set_flex_grow(ui.list, 1);
	lv_obj_set_flex_flow(ui.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(ui.list, 16, 0);
	lv_obj_set_style_pad_bottom(ui.list, 16, 0);
	lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(ui.list, list_scroll_cb, LV_EVENT_SCROLL_END, NULL);

	ui.empty = lv_label_create(ui.list);
	lv_label_set_text_static(ui.empty, "");
	lv_obj_set_width(ui.empty, LV_PCT(100));
	lv_obj_set_style_text_align(ui.empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(ui.empty, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(ui.empty, 200, 0);

	ui.card = lv_obj_create(ui.list);
	lv_obj_remove_style_all(ui.card);
	lv_obj_set_size(ui.card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(ui.card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_radius(ui.card, 24, 0);
	lv_obj_set_style_bg_opa(ui.card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ui.card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_clickable(ui.card, false);
	lv_obj_set_scrollable(ui.card, false);
}

void telegram_open(void)
{
	lv_obj_t *header, *body;
	ui.scr = ui_app_screen_create("Telegram", &header, &body);
	lv_obj_add_event_cb(ui.scr, delete_cb, LV_EVENT_DELETE, NULL);
	ui.account = ui_header_button(header, LV_SYMBOL_SETTINGS, account_cb, NULL);
	lv_obj_set_scrollable(body, false);

	form_create(body);
	list_create(body);
	ui.kb = ui_keyboard_create(body, ui.ta);

	start();
	show_state();
	/* The first page if the chats are not loaded yet (login done earlier) */
	load_chats();
	ui_push(ui.scr);
}
