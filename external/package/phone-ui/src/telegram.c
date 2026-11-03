/*
 * Telegram: signs in to Telegram through TDLib's JSON interface (libtdjson,
 * loaded when the app is first opened), then lists the chats: name, last
 * message, time, unread count. The settings button shows the account and logs out.
 * A chat shows its messages as bubbles (older ones loaded when scrolling up),
 * marks them read and sends text messages.
 *
 * Photos: TDLib downloads the smallest size at least as wide as a bubble into
 * its files directory; it is decoded (libjpeg-turbo, scaled down while decoding)
 * into a GL texture shown by an lv_3dtexture, like the map tiles: the draw unit's
 * cache knows images by their address. One photo is decoded per poll, newest
 * first; the texture lives with the message, across rebuilds of the view.
 *
 * TDLib keeps its database in /var/lib/telegram. The API credentials (from
 * my.telegram.org) are read from /var/lib/telegram/api: "<api_id> <api_hash>".
 * The client lives on after the screen is closed; a timer polls it, and the
 * chats are kept from its updates.
 */

#include <dlfcn.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

#include <jpeglib.h>

#include "json.h"
#include "ui.h"

#if LV_USE_3DTEXTURE
#include <GLES2/gl2.h>
#endif

#define DIR "/var/lib/telegram"
#define API_FILE DIR "/api"
#define LIB "libtdjson.so"
#define POLL_MS 100
#define PAGE 50          /* chats loaded at a time */
#define PREVIEW_MAX 120  /* bytes of the last message shown */
#define MAX_LENGTH 1024  /* characters typed in a message */
#define ROW_H 120
#define AVATAR 80
#define PHOTO_MAX_W 540  /* photos on screen: 75% of the width */
#define PHOTO_MAX_H 640
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
	bool group;      /* several senders: their names are shown */
	/* Avatar: the chat's small photo (the user's in a private chat) */
	int avatar_file; /* TDLib file id, 0: none */
	char *avatar_path; /* once downloaded */
	unsigned avatar_tex; /* GL texture, round, 0: not decoded yet */
	bool avatar_bad;
};

struct user {
	long long id;
	char *name;
};

/* A message of the open chat */
struct msg {
	long long id;
	long long sender; /* user or chat id */
	long long date;
	bool outgoing;
	bool service;     /* not a message of a sender (or not supported): centered */
	enum { SENT, SENDING, NOT_SENT } state;
	char *text;       /* the caption for a photo */
	/* Photo: the size shown, downloaded by TDLib, then decoded into a texture */
	int photo_file;   /* TDLib file id, 0: no photo */
	int photo_w, photo_h; /* on screen */
	char *photo_path; /* once downloaded */
	unsigned tex;     /* GL texture, 0: not decoded yet */
	bool photo_bad;   /* could not be decoded */
	lv_obj_t *photo;  /* its box in the view */
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

	/* Names of the senders, from updateUser */
	struct user *users;
	unsigned user_count, user_cap;
} tg;

/* The open chat */
static struct {
	lv_obj_t *scr;
	lv_obj_t *list, *ta, *kb;
	long long chat_id;
	bool group;
	struct msg *msgs; /* oldest first */
	unsigned count, cap;
	bool loading;     /* getChatHistory on its way */
	bool at_start;    /* no older messages */
	bool filling;     /* view_fill() scrolls: not the user */
	/* Messages are grouped while they come from the same sender in the same
	 * minute, with the time under the group */
	lv_obj_t *group_obj;
	long long group_sender;
	bool group_outgoing;
	int group_state;
	char group_time[8];
	char day[32];
} cv;

struct row {
	lv_obj_t *obj, *avatar, *initial, *photo, *title, *time, *preview, *badge, *count;
	long long chat_id;
	unsigned tex; /* shown by photo */
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
static void chat_open(long long id);

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

/* For labels: emoji sequences as one character */
static void set_str(char **dst, const char *src)
{
	free(*dst);
	*dst = ui_emoji_compose(src ? src : "");
}

/* One line of at most PREVIEW_MAX bytes, whitespace runs as one space */
static char *one_line(const char *prefix, const char *original)
{
	char buf[PREVIEW_MAX + 1];
	size_t n = snprintf(buf, sizeof(buf), "%s", prefix);
	bool space = false;
	/* Emoji sequences first: not cut in the middle */
	char *composed = ui_emoji_compose(original);
	const char *text = composed ? composed : "";
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
	free(composed);
	return strdup(buf);
}

/* What a message that is not text holds, NULL if not known */
static const char *content_kind(const char *type)
{
	return
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
		!strcmp(type, "messageCall") ? "Call" : NULL;
}

static char *message_preview(const struct json *msg)
{
	if (!msg)
		return strdup("");
	const struct json *content = json_get(msg, "content");
	const char *type = json_type(content);
	const char *text = !strcmp(type, "messageText") ?
		json_str(json_get(json_get(content, "text"), "text"), "") : content_kind(type);
	return one_line(json_bool(json_get(msg, "is_outgoing")) ? "You: " : "", text ? text : "Message");
}

/* The whole text of a message: the kind of content, then its caption */
static char *message_text(const struct json *content, bool *service)
{
	const char *type = json_type(content);
	*service = false;
	if (!strcmp(type, "messageText"))
		return ui_emoji_compose(json_str(json_get(json_get(content, "text"), "text"), ""));
	const char *kind = content_kind(type);
	if (!kind) {
		*service = true;
		return strdup("Unsupported message");
	}
	char *caption = ui_emoji_compose(json_str(json_get(json_get(content, "caption"), "text"), ""));
	size_t size = strlen(kind) + (caption ? strlen(caption) : 0) + 2;
	char *text = malloc(size);
	if (text)
		snprintf(text, size, caption && caption[0] ? "%s\n%s" : "%s", kind, caption ? caption : "");
	free(caption);
	return text;
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

static void avatar_drop(struct chat *c)
{
#if LV_USE_3DTEXTURE
	if (c->avatar_tex)
		glDeleteTextures(1, &c->avatar_tex);
#endif
	free(c->avatar_path);
	c->avatar_file = 0;
	c->avatar_path = NULL;
	c->avatar_tex = 0;
	c->avatar_bad = false;
}

static void chats_clear(void)
{
	for (unsigned i = 0; i < tg.chat_count; i++) {
		free(tg.chats[i].title);
		free(tg.chats[i].preview);
		avatar_drop(&tg.chats[i]);
	}
	free(tg.chats);
	tg.chats = NULL;
	tg.chat_count = tg.chat_cap = 0;
	tg.loading = tg.all_loaded = false;
	tg.chats_changed = true;

	for (unsigned i = 0; i < tg.user_count; i++)
		free(tg.users[i].name);
	free(tg.users);
	tg.users = NULL;
	tg.user_count = tg.user_cap = 0;
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

/* A downloaded file: its path to the chats showing it as their avatar */
static void avatar_file(const struct json *file)
{
	const struct json *local = json_get(file, "local");
	if (!json_bool(json_get(local, "is_downloading_completed")))
		return;
	int id = json_int(json_get(file, "id"), 0);
	const char *path = json_str(json_get(local, "path"), "");
	for (unsigned i = 0; i < tg.chat_count; i++) {
		struct chat *c = &tg.chats[i];
		if (c->avatar_file == id && !c->avatar_path && path[0])
			c->avatar_path = strdup(path);
	}
}

/* A chatPhotoInfo, or null: no photo */
static void chat_avatar(struct chat *c, const struct json *photo)
{
	const struct json *file = json_get(photo, "small");
	int id = json_int(json_get(file, "id"), 0);
	if (id == c->avatar_file)
		return;
	avatar_drop(c);
	if (!id)
		return;
	c->avatar_file = id;
	avatar_file(file);
	if (!c->avatar_path) {
		char buf[128];
		snprintf(buf, sizeof(buf), "{\"@type\":\"downloadFile\",\"file_id\":%d,\"priority\":2,"
			"\"offset\":0,\"limit\":0,\"synchronous\":false,\"@extra\":\"download\"}", id);
		td_request(buf);
	}
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
		const struct json *kind = json_get(chat, "type");
		c->group = !strcmp(json_type(kind), "chatTypeBasicGroup") ||
			(!strcmp(json_type(kind), "chatTypeSupergroup") && !json_bool(json_get(kind, "is_channel")));
		chat_last_message(c, json_get(chat, "last_message"));
		chat_positions(c, json_get(chat, "positions"));
		chat_avatar(c, json_get(chat, "photo"));
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
		else if (!strcmp(type, "updateChatPhoto"))
			chat_avatar(c, json_get(u, "photo"));
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

/* Users */

static struct user *user_find(long long id)
{
	for (unsigned i = 0; i < tg.user_count; i++)
		if (tg.users[i].id == id)
			return &tg.users[i];
	return NULL;
}

static void user_update(const struct json *user)
{
	long long id = json_int(json_get(user, "id"), 0);
	struct user *u = user_find(id);
	if (!u) {
		if (tg.user_count == tg.user_cap) {
			unsigned cap = tg.user_cap ? tg.user_cap * 2 : 64;
			struct user *users = realloc(tg.users, cap * sizeof(*users));
			if (!users)
				return;
			tg.users = users;
			tg.user_cap = cap;
		}
		u = &tg.users[tg.user_count++];
		u->id = id;
		u->name = NULL;
	}
	const char *first = json_str(json_get(user, "first_name"), "");
	const char *last = json_str(json_get(user, "last_name"), "");
	char *name = malloc(strlen(first) + strlen(last) + 2);
	if (name)
		sprintf(name, "%s%s%s", first, first[0] && last[0] ? " " : "", last);
	free(u->name);
	u->name = name ? ui_emoji_compose(name) : NULL;
	free(name);
}

/* A user's name or a chat's title (channels and groups post too) */
static const char *sender_name(long long id)
{
	struct user *u = user_find(id);
	if (u && u->name && u->name[0])
		return u->name;
	struct chat *c = chat_find(id);
	return c ? c->title : "";
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

/* Rows are passed by index: the array moves when it grows */
static void row_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i < ui.row_count)
		chat_open(ui.rows[i].chat_id);
}

static void row_create(struct row *r, unsigned index)
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
	lv_obj_set_style_radius(r->obj, 24, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r->obj, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r->obj, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_scrollable(r->obj, false);
	lv_obj_set_clickable(r->obj, true);
	lv_obj_add_event_cb(r->obj, row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

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
	/* The photo over the initial, once decoded */
#if LV_USE_3DTEXTURE
	r->photo = lv_3dtexture_create(r->avatar);
	lv_obj_set_size(r->photo, AVATAR, AVATAR);
	lv_obj_set_clickable(r->photo, false);
	lv_obj_set_hidden(r->photo, true);
#endif
	r->tex = 0;

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
#if LV_USE_3DTEXTURE
	if (r->tex != c->avatar_tex) {
		r->tex = c->avatar_tex;
		lv_obj_set_hidden(r->photo, !r->tex);
		/* Behind the photo's soft edge: no colored fringe */
		lv_obj_set_hidden(r->initial, r->tex);
		lv_obj_set_style_bg_opa(r->avatar, r->tex ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
		if (r->tex)
			lv_3dtexture_set_src(r->photo, r->tex);
	}
#endif

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
		for (; ui.row_count < count; ui.row_count++)
			row_create(&ui.rows[ui.row_count], ui.row_count);
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

/* Chat */

static void photo_drop(struct msg *m)
{
#if LV_USE_3DTEXTURE
	if (m->tex)
		glDeleteTextures(1, &m->tex);
#endif
	free(m->photo_path);
	m->photo_file = 0;
	m->photo_path = NULL;
	m->tex = 0;
	m->photo_bad = false;
	m->photo = NULL;
}

static void msg_free(struct msg *m)
{
	free(m->text);
	photo_drop(m);
}

/* A downloaded file: its path to the photos showing it */
static void photo_file(const struct json *file)
{
	const struct json *local = json_get(file, "local");
	if (!json_bool(json_get(local, "is_downloading_completed")))
		return;
	int id = json_int(json_get(file, "id"), 0);
	const char *path = json_str(json_get(local, "path"), "");
	for (unsigned i = 0; i < cv.count; i++) {
		struct msg *m = &cv.msgs[i];
		if (m->photo_file == id && !m->photo_path && path[0])
			m->photo_path = strdup(path);
	}
}

/* The size to show: the smallest at least as wide as on screen; the caption
 * becomes the text */
static void photo_set(struct msg *m, const struct json *content)
{
	const struct json *sizes = json_get(json_get(content, "photo"), "sizes");
	const struct json *best = NULL, *largest = NULL;
	for (const struct json *s = sizes ? sizes->child : NULL; s; s = s->next)
		if (!largest || json_int(json_get(s, "width"), 0) > json_int(json_get(largest, "width"), 0))
			largest = s;
	int w = json_int(json_get(largest, "width"), 0), h = json_int(json_get(largest, "height"), 0);
	if (w <= 0 || h <= 0) {
		photo_drop(m);
		return;
	}

	/* On screen: within PHOTO_MAX_W x PHOTO_MAX_H, never enlarged */
	float scale = (float)PHOTO_MAX_W / w;
	if ((float)PHOTO_MAX_H / h < scale)
		scale = (float)PHOTO_MAX_H / h;
	if (scale > 1)
		scale = 1;
	int sw = w * scale, sh = h * scale;
	for (const struct json *s = sizes->child; s; s = s->next) {
		int sz = json_int(json_get(s, "width"), 0);
		if (sz >= sw && (!best || sz < json_int(json_get(best, "width"), 0)))
			best = s;
	}
	if (!best)
		best = largest;

	const struct json *file = json_get(best, "photo");
	int id = json_int(json_get(file, "id"), 0);
	if (id != m->photo_file) {
		photo_drop(m);
		m->photo_file = id;
		photo_file(file);
		if (!m->photo_path)
			for (unsigned i = 0; i < cv.count; i++)
				if (&cv.msgs[i] != m && cv.msgs[i].photo_path && cv.msgs[i].photo_file == id)
					m->photo_path = strdup(cv.msgs[i].photo_path);
		if (!m->photo_path) {
			char buf[128];
			snprintf(buf, sizeof(buf), "{\"@type\":\"downloadFile\",\"file_id\":%d,\"priority\":1,"
				"\"offset\":0,\"limit\":0,\"synchronous\":false,\"@extra\":\"download\"}", id);
			td_request(buf);
		}
	}
	m->photo_w = sw > 0 ? sw : 1;
	m->photo_h = sh > 0 ? sh : 1;

	free(m->text);
	m->text = strdup(json_str(json_get(json_get(content, "caption"), "text"), ""));
}

struct jpeg_error {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

/* libjpeg's default exits the program */
static void jpeg_error_exit(j_common_ptr cinfo)
{
	char text[JMSG_LENGTH_MAX];
	cinfo->err->format_message(cinfo, text);
	printf("telegram: photo: %s\n", text);
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
}

/* Transparent outside the inscribed circle, with a one pixel soft edge (the
 * draw unit blends textures with their alpha) */
static void round_alpha(unsigned char *pixels, int w, int h)
{
	float cx = w / 2.0f, cy = h / 2.0f, r = (w < h ? w : h) / 2.0f;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			float a = r - sqrtf(dx * dx + dy * dy) + 0.5f;
			unsigned char *p = pixels + (y * w + x) * 4;
			p[3] = a >= 1 ? 255 : a <= 0 ? 0 : (unsigned char)(a * 255);
		}
	}
}

/* Decoded at the smallest scale (n/8) still as wide as on screen; round: a
 * circle (avatars) */
static unsigned jpeg_texture(const char *path, int width, bool round)
{
#if LV_USE_3DTEXTURE
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error err;
	unsigned char *volatile pixels = NULL;
	unsigned tex = 0;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_decompress(&cinfo);
		free(pixels);
		fclose(f);
		return 0;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	/* LVGL's layout: its texture shader swaps red and blue back (c.bgra) */
	cinfo.out_color_space = JCS_EXT_BGRA;
	cinfo.scale_denom = 8;
	cinfo.scale_num = (width * 8 + cinfo.image_width - 1) / cinfo.image_width;
	if (cinfo.scale_num < 1)
		cinfo.scale_num = 1;
	if (cinfo.scale_num > 8)
		cinfo.scale_num = 8;
	jpeg_start_decompress(&cinfo);

	size_t stride = cinfo.output_width * 4;
	pixels = malloc(stride * cinfo.output_height);
	if (!pixels)
		longjmp(err.jmp, 1);
	while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char *row = pixels + cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	if (round)
		round_alpha(pixels, cinfo.output_width, cinfo.output_height);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cinfo.output_width, cinfo.output_height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(pixels);
	fclose(f);
	return tex;
#else
	LV_UNUSED(path);
	LV_UNUSED(width);
	LV_UNUSED(round);
	return 0;
#endif
}

static void photo_show(struct msg *m)
{
#if LV_USE_3DTEXTURE
	if (!m->photo || !m->tex)
		return;
	lv_obj_t *quad = lv_3dtexture_create(m->photo);
	lv_obj_set_size(quad, LV_PCT(100), LV_PCT(100));
	lv_obj_set_clickable(quad, false);
	lv_3dtexture_set_src(quad, m->tex);
#else
	LV_UNUSED(m);
#endif
}

/* A file TDLib finished downloading: an avatar or a photo of the open chat */
static void file_done(const struct json *file)
{
	avatar_file(file);
	photo_file(file);
}

/* Avatars downloaded and not decoded yet, those in the list first: they are
 * small (160 px, decoded at half size), a few per poll */
static void avatar_decode_next(void)
{
	int n = 0;
	for (int pass = 0; pass < 2 && n < 8; pass++) {
		for (unsigned i = 0; i < tg.chat_count && n < 8; i++) {
			struct chat *c = &tg.chats[i];
			if (!c->avatar_path || c->avatar_tex || c->avatar_bad || (!pass && !c->order))
				continue;
			c->avatar_tex = jpeg_texture(c->avatar_path, AVATAR, true);
			c->avatar_bad = !c->avatar_tex;
			tg.chats_changed = true;
			n++;
		}
	}
}

/* The newest downloaded photo not decoded yet: one per poll, the UI waits for it */
static void photo_decode_next(void)
{
	for (unsigned i = cv.count; i-- > 0;) {
		struct msg *m = &cv.msgs[i];
		if (!m->photo_path || m->tex || m->photo_bad)
			continue;
		m->tex = jpeg_texture(m->photo_path, m->photo_w, false);
		m->photo_bad = !m->tex;
		photo_show(m);
		return;
	}
}

static void msg_content(struct msg *m, const struct json *content)
{
	free(m->text);
	m->text = message_text(content, &m->service);
	if (!strcmp(json_type(content), "messagePhoto"))
		photo_set(m, content);
	else
		photo_drop(m);
}

static void msg_set(struct msg *m, const struct json *j)
{
	const struct json *sender = json_get(j, "sender_id");
	m->id = json_int(json_get(j, "id"), 0);
	m->sender = json_int(json_get(sender, !strcmp(json_type(sender), "messageSenderChat") ?
		"chat_id" : "user_id"), 0);
	m->date = json_int(json_get(j, "date"), 0);
	m->outgoing = json_bool(json_get(j, "is_outgoing"));
	const char *sending = json_type(json_get(j, "sending_state"));
	m->state = !strcmp(sending, "messageSendingStatePending") ? SENDING :
	           !strcmp(sending, "messageSendingStateFailed") ? NOT_SENT : SENT;
	msg_content(m, json_get(j, "content"));
}

static struct msg *msg_find(long long id)
{
	for (unsigned i = 0; i < cv.count; i++)
		if (cv.msgs[i].id == id)
			return &cv.msgs[i];
	return NULL;
}

/* Kept in id order (the order they were sent in) */
static void msg_insert(const struct json *j)
{
	long long id = json_int(json_get(j, "id"), 0);
	struct msg *m = msg_find(id);
	if (!m) {
		if (cv.count == cv.cap) {
			unsigned cap = cv.cap ? cv.cap * 2 : 64;
			struct msg *msgs = realloc(cv.msgs, cap * sizeof(*msgs));
			if (!msgs)
				return;
			cv.msgs = msgs;
			cv.cap = cap;
		}
		unsigned i = cv.count;
		while (i && cv.msgs[i - 1].id > id)
			i--;
		memmove(&cv.msgs[i + 1], &cv.msgs[i], (cv.count - i) * sizeof(*cv.msgs));
		cv.count++;
		m = &cv.msgs[i];
		memset(m, 0, sizeof(*m));
	}
	msg_set(m, j);
}

static void msgs_free(void)
{
	for (unsigned i = 0; i < cv.count; i++)
		msg_free(&cv.msgs[i]);
	free(cv.msgs);
	cv.msgs = NULL;
	cv.count = cv.cap = 0;
}

static void day_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL), yesterday = now - 24 * 3600;
	struct tm tm, today, prev;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	localtime_r(&yesterday, &prev);
	if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
		snprintf(buf, size, "Today");
	else if (tm.tm_year == prev.tm_year && tm.tm_yday == prev.tm_yday)
		snprintf(buf, size, "Yesterday");
	else
		strftime(buf, size, tm.tm_year == today.tm_year ? "%A %-d %B" : "%-d %B %Y", &tm);
}

static lv_obj_t *centered_label(const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(cv.list);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

static void view_add(struct msg *m)
{
	char day[32], when[8];
	day_format(day, sizeof(day), m->date);
	if (strcmp(day, cv.day)) {
		snprintf(cv.day, sizeof(cv.day), "%s", day);
		lv_obj_t *label = centered_label(&ui_font_16);
		ui_set_text(label, day);
		lv_obj_set_style_margin_top(label, 8, 0);
		cv.group_obj = NULL;
	}
	if (m->service) {
		lv_label_set_text(centered_label(&ui_font_20), m->text);
		cv.group_obj = NULL;
		return;
	}

	time_t t = m->date;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(when, sizeof(when), "%H:%M", &tm);
	if (!cv.group_obj || cv.group_sender != m->sender || cv.group_outgoing != m->outgoing ||
	    cv.group_state != (int)m->state || strcmp(cv.group_time, when)) {
		cv.group_obj = lv_obj_create(cv.list);
		lv_obj_remove_style_all(cv.group_obj);
		lv_obj_set_size(cv.group_obj, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(cv.group_obj, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(cv.group_obj, LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(cv.group_obj, 6, 0);
		lv_obj_set_clickable(cv.group_obj, false);
		lv_obj_set_scrollable(cv.group_obj, false);

		if (cv.group && !m->outgoing) {
			lv_obj_t *name = lv_label_create(cv.group_obj);
			ui_set_text(name, sender_name(m->sender));
			lv_obj_set_style_text_font(name, &ui_font_20, 0);
			lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_ACCENT), 0);
			lv_obj_set_style_pad_hor(name, 12, 0);
		}
		lv_obj_t *time_label = lv_label_create(cv.group_obj);
		if (m->state == SENDING)
			lv_label_set_text_static(time_label, "Sending...");
		else if (m->state == NOT_SENT)
			ui_set_text_fmt(time_label, "%s  " LV_SYMBOL_WARNING " Not sent", when);
		else
			ui_set_text(time_label, when);
		lv_obj_set_style_text_font(time_label, &ui_font_16, 0);
		lv_obj_set_style_text_color(time_label, lv_color_hex(m->state == NOT_SENT ? COLOR_RED : UI_COLOR_DIM), 0);
		lv_obj_set_style_pad_hor(time_label, 12, 0);
		lv_obj_set_style_margin_top(time_label, 2, 0);

		cv.group_sender = m->sender;
		cv.group_outgoing = m->outgoing;
		cv.group_state = m->state;
		snprintf(cv.group_time, sizeof(cv.group_time), "%s", when);
	}

	if (m->photo_file) {
		/* An opaque box: a placeholder until the texture covers it */
		m->photo = lv_obj_create(cv.group_obj);
		lv_obj_remove_style_all(m->photo);
		lv_obj_set_size(m->photo, m->photo_w, m->photo_h);
		lv_obj_set_style_bg_opa(m->photo, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(m->photo, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_clickable(m->photo, false);
		lv_obj_set_scrollable(m->photo, false);
		lv_obj_move_to_index(m->photo, (int32_t)lv_obj_get_child_count(cv.group_obj) - 2);
		lv_obj_t *icon = lv_label_create(m->photo);
		lv_label_set_text_static(icon, m->photo_bad ? LV_SYMBOL_WARNING : LV_SYMBOL_IMAGE);
		lv_obj_set_style_text_font(icon, &ui_font_48, 0);
		lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_center(icon);
		photo_show(m);
		if (!m->text[0])
			return;
	}

	lv_obj_t *bubble = lv_obj_create(cv.group_obj);
	lv_obj_remove_style_all(bubble);
	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bubble, 24, 0);
	lv_obj_set_style_pad_ver(bubble, 16, 0);
	lv_obj_set_style_radius(bubble, 24, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
	/* Not on the server yet: paler */
	if (m->state != SENT)
		lv_obj_set_style_bg_opa(bubble, LV_OPA_60, 0);
	lv_obj_set_clickable(bubble, false);
	lv_obj_set_scrollable(bubble, false);
	/* Before the time */
	lv_obj_move_to_index(bubble, (int32_t)lv_obj_get_child_count(cv.group_obj) - 2);

	/* Wraps at 75% of the screen (bubble padding included) */
	lv_obj_t *label = lv_label_create(bubble);
	lv_label_set_text(label, m->text);
	lv_obj_set_style_max_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) * 3 / 4 - 48, 0);
	lv_obj_set_style_text_color(label, m->outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
}

static void view_scroll_end(lv_anim_enable_t anim)
{
	lv_obj_update_layout(cv.list);
	lv_obj_scroll_to_y(cv.list, lv_obj_get_scroll_y(cv.list) + lv_obj_get_scroll_bottom(cv.list), anim);
}

/* At the end, or on the way there: only scrolls to the end are animated */
static bool view_at_end(void)
{
	return lv_obj_get_scroll_bottom(cv.list) < 200 || lv_anim_get(cv.list, NULL);
}

/* Rebuilt when older messages come or messages change; the distance to the
 * end of the list stays the same, or it stays at the end */
static void view_fill(void)
{
	/* Emptying the list and scrolling back send SCROLL_END: without this, each
	 * rebuild looked like a scroll to the top and loaded the whole history */
	cv.filling = true;
	lv_obj_update_layout(cv.list);
	bool end = view_at_end();
	/* A running scroll aims at where the end was */
	lv_anim_delete(cv.list, NULL);
	int32_t bottom = end ? 0 : lv_obj_get_scroll_bottom(cv.list);
	lv_obj_clean(cv.list);
	cv.group_obj = NULL;
	cv.day[0] = '\0';

	if (!cv.count) {
		lv_obj_t *hint = centered_label(&ui_font_24);
		lv_label_set_text_static(hint, cv.at_start ? "No messages" : "Loading...");
		lv_obj_set_style_margin_top(hint, 200, 0);
	}
	for (unsigned i = 0; i < cv.count; i++)
		view_add(&cv.msgs[i]);

	lv_obj_update_layout(cv.list);
	lv_obj_scroll_to_y(cv.list, lv_obj_get_scroll_y(cv.list) + lv_obj_get_scroll_bottom(cv.list) - bottom,
	                   LV_ANIM_OFF);
	cv.filling = false;
}

/* Read up to the newest message, as the app shows it */
static void view_read(void)
{
	if (!cv.count)
		return;
	char buf[160];
	snprintf(buf, sizeof(buf), "{\"@type\":\"viewMessages\",\"chat_id\":%lld,\"message_ids\":[%lld],"
		"\"force_read\":true}", cv.chat_id, cv.msgs[cv.count - 1].id);
	td_request(buf);
}

/* Older messages than those shown; they come in small batches */
static void history_load(void)
{
	if (cv.loading || cv.at_start)
		return;
	char buf[192];
	snprintf(buf, sizeof(buf), "{\"@type\":\"getChatHistory\",\"chat_id\":%lld,\"from_message_id\":%lld,"
		"\"offset\":0,\"limit\":%d,\"only_local\":false,\"@extra\":\"history %lld\"}",
		cv.chat_id, cv.count ? cv.msgs[0].id : 0, PAGE, cv.chat_id);
	td_request(buf);
	cv.loading = true;
}

static void history_answer(const struct json *u)
{
	long long id = 0;
	sscanf(json_str(json_get(u, "@extra"), ""), "history %lld", &id);
	if (!cv.scr || id != cv.chat_id)
		return;
	cv.loading = false;

	/* The batch starts with the oldest message shown */
	const struct json *list = json_get(u, "messages");
	unsigned before = cv.count;
	for (const struct json *m = list ? list->child : NULL; m; m = m->next)
		msg_insert(m);
	if (cv.count == before)
		cv.at_start = true;
	view_fill();
	view_read();
	/* Enough to fill the screen */
	if (cv.count < 30)
		history_load();
}

/* Updates of the open chat's messages */
static void view_update(const char *type, const struct json *u)
{
	if (!strcmp(type, "updateNewMessage")) {
		const struct json *j = json_get(u, "message");
		if (json_int(json_get(j, "chat_id"), 0) != cv.chat_id)
			return;
		bool end = view_at_end() || json_bool(json_get(j, "is_outgoing"));
		long long id = json_int(json_get(j, "id"), 0);
		if (!cv.count || id > cv.msgs[cv.count - 1].id) {
			if (!cv.count)
				lv_obj_clean(cv.list); /* the hint */
			msg_insert(j);
			view_add(&cv.msgs[cv.count - 1]);
		} else {
			msg_insert(j);
			view_fill();
		}
		if (end) {
			/* From where it is, not from the end of a scroll still running */
			lv_anim_delete(cv.list, NULL);
			view_scroll_end(LV_ANIM_ON);
		}
		view_read();
	} else if (!strcmp(type, "updateMessageContent")) {
		if (json_int(json_get(u, "chat_id"), 0) != cv.chat_id)
			return;
		struct msg *m = msg_find(json_int(json_get(u, "message_id"), 0));
		if (!m)
			return;
		msg_content(m, json_get(u, "new_content"));
		view_fill();
	} else if (!strcmp(type, "updateMessageSendSucceeded") || !strcmp(type, "updateMessageSendFailed")) {
		/* The message the server returned replaces the temporary one (new id) */
		const struct json *j = json_get(u, "message");
		if (json_int(json_get(j, "chat_id"), 0) != cv.chat_id)
			return;
		struct msg *m = msg_find(json_int(json_get(u, "old_message_id"), 0));
		if (m) {
			msg_free(m);
			memmove(m, m + 1, (cv.msgs + --cv.count - m) * sizeof(*m));
		}
		msg_insert(j);
		if (!strcmp(type, "updateMessageSendFailed"))
			printf("telegram: not sent: %s\n", json_str(json_get(json_get(u, "error"), "message"), ""));
		view_fill();
	} else if (!strcmp(type, "updateDeleteMessages")) {
		if (json_int(json_get(u, "chat_id"), 0) != cv.chat_id || !json_bool(json_get(u, "is_permanent")))
			return;
		const struct json *ids = json_get(u, "message_ids");
		for (const struct json *v = ids ? ids->child : NULL; v; v = v->next) {
			struct msg *m = msg_find(json_int(v, 0));
			if (!m)
				continue;
			msg_free(m);
			memmove(m, m + 1, (cv.msgs + --cv.count - m) * sizeof(*m));
		}
		view_fill();
	}
}

/* Near the top: older messages */
static void view_scroll_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!cv.filling && lv_obj_get_scroll_top(cv.list) < 600)
		history_load();
}

static void view_keyboard(bool show)
{
	ui_keyboard_show(cv.kb, show);
	view_scroll_end(LV_ANIM_OFF);
}

/* It shows up through updateNewMessage, pending until the server has it */
static void view_send(void)
{
	const char *text = lv_textarea_get_text(cv.ta);
	while (*text == ' ')
		text++;
	size_t n = strlen(text);
	while (n && text[n - 1] == ' ')
		n--;
	view_keyboard(false);
	if (!n)
		return;

	char *trimmed = strndup(text, n);
	char *escaped = malloc(n * 6 + 1);
	char *buf = malloc(n * 6 + 256);
	if (trimmed && escaped && buf) {
		json_escape(escaped, n * 6 + 1, trimmed);
		snprintf(buf, n * 6 + 256, "{\"@type\":\"sendMessage\",\"chat_id\":%lld,"
			"\"input_message_content\":{\"@type\":\"inputMessageText\","
			"\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}},\"@extra\":\"send\"}",
			cv.chat_id, escaped);
		td_request(buf);
		lv_textarea_set_text(cv.ta, "");
		ui_vibrate(15);
	}
	free(trimmed);
	free(escaped);
	free(buf);
}

/* Refused before it was sent (no right to write in this chat, ...) */
static void view_send_error(const struct json *u)
{
	const char *message = json_str(json_get(u, "message"), "");
	printf("telegram: sendMessage: %s\n", message);
	if (!cv.scr)
		return;
	lv_obj_t *label = centered_label(&ui_font_20);
	ui_set_text_fmt(label, LV_SYMBOL_WARNING " Not sent: %s", message);
	lv_obj_set_style_text_color(label, lv_color_hex(COLOR_RED), 0);
	cv.group_obj = NULL;
	view_scroll_end(LV_ANIM_ON);
}

static void view_send_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	view_send();
}

static void view_ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		view_keyboard(true);
		break;
	case LV_EVENT_READY: /* the keyboard's Enter key */
		view_send();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		view_keyboard(false);
		break;
	default:
		break;
	}
}

static void view_click_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!lv_obj_is_hidden(cv.kb))
		view_keyboard(false);
}

static void bar_create(lv_obj_t *body)
{
	lv_obj_t *bar = lv_obj_create(body);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_column(bar, 12, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_clickable(bar, false);
	lv_obj_set_scrollable(bar, false);

	cv.ta = lv_textarea_create(bar);
	lv_textarea_set_one_line(cv.ta, true);
	lv_textarea_set_placeholder_text(cv.ta, "Message");
	lv_textarea_set_max_length(cv.ta, MAX_LENGTH);
	lv_obj_set_height(cv.ta, 80);
	lv_obj_set_flex_grow(cv.ta, 1);
	lv_obj_set_style_radius(cv.ta, 40, 0);
	lv_obj_set_style_pad_hor(cv.ta, 28, 0);
	lv_obj_set_style_pad_ver(cv.ta, 24, 0);
	lv_obj_set_style_bg_opa(cv.ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(cv.ta, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_text_color(cv.ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(cv.ta, 0, 0);
	lv_obj_set_style_border_width(cv.ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(cv.ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(cv.ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(cv.ta, 0, 0);
	lv_obj_add_event_cb(cv.ta, view_ta_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(cv.ta, view_ta_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(cv.ta, view_ta_cb, LV_EVENT_CANCEL, NULL);

	lv_obj_t *button = lv_button_create(bar);
	lv_obj_set_size(button, 80, 80);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, view_send_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, LV_SYMBOL_UP);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);

	/* Under the bar, shown while typing; the messages shrink above it */
	cv.kb = ui_keyboard_create(body, cv.ta);
}

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"closeChat\",\"chat_id\":%lld}", cv.chat_id);
	td_request(buf);
	msgs_free();
	memset(&cv, 0, sizeof(cv));
}

static void chat_open(long long id)
{
	struct chat *c = chat_find(id);
	if (!c || cv.scr)
		return;

	lv_obj_t *body;
	/* The title must stay valid: the chat's can change */
	cv.scr = ui_app_screen_create(ui_intern(c->title), NULL, &body);
	lv_obj_add_event_cb(cv.scr, view_delete_cb, LV_EVENT_DELETE, NULL);
	cv.chat_id = id;
	cv.group = c->group;

	lv_obj_set_scrollable(body, false);
	cv.list = lv_obj_create(body);
	lv_obj_remove_style_all(cv.list);
	lv_obj_set_width(cv.list, LV_PCT(100));
	lv_obj_set_flex_grow(cv.list, 1);
	lv_obj_set_flex_flow(cv.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_event_cb(cv.list, view_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_pad_hor(cv.list, 16, 0);
	lv_obj_set_style_pad_ver(cv.list, 16, 0);
	lv_obj_set_style_pad_row(cv.list, 24, 0);
	lv_obj_set_scroll_dir(cv.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(cv.list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(cv.list, view_scroll_cb, LV_EVENT_SCROLL_END, NULL);
	bar_create(body);

	/* Open: TDLib sends its updates and counts it as viewed */
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"openChat\",\"chat_id\":%lld}", id);
	td_request(buf);
	view_fill();
	history_load();
	ui_push(cv.scr);
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
	} else if (!strcmp(type, "updateFile")) {
		file_done(json_get(u, "file"));
	} else if (!strcmp(type, "updateUser")) {
		user_update(json_get(u, "user"));
	} else if (!strncmp(type, "update", 6)) {
		if (cv.scr)
			view_update(type, u);
	} else if (!strncmp(extra, "history ", 8)) {
		if (!strcmp(type, "messages"))
			history_answer(u);
		else
			cv.loading = false;
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
	} else if (!strcmp(extra, "download")) {
		/* Already downloaded: the answer says so, no updateFile follows */
		if (!strcmp(type, "file"))
			file_done(u);
		else
			printf("telegram: downloadFile: %s\n", json_str(json_get(u, "message"), ""));
	} else if (!strcmp(type, "error") && !strcmp(extra, "send")) {
		view_send_error(u);
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
	avatar_decode_next();
	if (tg.chats_changed && ui.list && !lv_obj_is_hidden(ui.list))
		list_refresh();
	if (cv.scr)
		photo_decode_next();
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
