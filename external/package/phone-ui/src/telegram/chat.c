/*
 * Telegram's open chat: its messages as bubbles, grouped by sender and minute
 * with the time under the group, sender names in groups, a line between days.
 * Older messages load when scrolling up; new, edited and deleted messages update
 * it, and it is marked read. Text messages are sent from the bar at the bottom.
 *
 * Photos: TDLib downloads the smallest size at least as wide as a bubble into
 * its files directory; it is decoded into a GL texture (media.c) shown by an
 * lv_3dtexture. One photo is decoded per poll, newest first; the texture lives
 * with the message, across rebuilds of the view.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define MAX_LENGTH 1024  /* characters typed in a message */
#define PHOTO_MAX_W 540  /* photos on screen: 75% of the width */
#define PHOTO_MAX_H 640

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

/* Messages */

static void photo_drop(struct msg *m)
{
	ui_texture_delete(m->tex);
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
void photo_file(const struct json *file)
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

/* View */

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
		lv_obj_set_style_text_color(time_label, lv_color_hex(m->state == NOT_SENT ? UI_COLOR_RED : UI_COLOR_DIM), 0);
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

void chat_history_answer(const struct json *u)
{
	if (strcmp(json_type(u), "messages")) {
		cv.loading = false;
		return;
	}
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
void chat_update_messages(const char *type, const struct json *u)
{
	if (!cv.scr)
		return;
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
void chat_send_error(const struct json *u)
{
	const char *message = json_str(json_get(u, "message"), "");
	printf("telegram: sendMessage: %s\n", message);
	if (!cv.scr)
		return;
	lv_obj_t *label = centered_label(&ui_font_20);
	ui_set_text_fmt(label, LV_SYMBOL_WARNING " Not sent: %s", message);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_RED), 0);
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

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"closeChat\",\"chat_id\":%lld}", cv.chat_id);
	td_request(buf);
	msgs_free();
	memset(&cv, 0, sizeof(cv));
}

void chat_open(long long id)
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

	cv.ta = ui_compose_bar(body, MAX_LENGTH, view_ta_cb, view_send_cb, NULL);
	/* Under the bar, shown while typing; the messages shrink above it */
	cv.kb = ui_keyboard_create(body, cv.ta);

	/* Open: TDLib sends its updates and counts it as viewed */
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"openChat\",\"chat_id\":%lld}", id);
	td_request(buf);
	view_fill();
	history_load();
	ui_push(cv.scr);
}

/* One photo decoded per poll */
void chat_poll(void)
{
	if (cv.scr)
		photo_decode_next();
}
