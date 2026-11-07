/*
 * Telegram's open chat: its messages as bubbles, grouped by sender and minute
 * with the time under the group, sender names in groups, a line between days.
 * Older messages load when scrolling up; new, edited and deleted messages update
 * it, and it is marked read. Text messages are sent from the bar at the bottom,
 * photos from the Gallery through the button next to it. A message swiped left is
 * replied to (ui_chat_reply()); a reply shows the message it answers at its top, asked
 * for (getRepliedMessage) when it is not loaded.
 *
 * Photos: TDLib downloads the smallest size at least as wide as a bubble into
 * its files directory; it is decoded into a GL texture (media.c) shown by an
 * lv_3dtexture. One photo is decoded per poll, newest first; the texture lives
 * with the message, across rebuilds of the view. Stickers go the same way, without
 * a background: static ones, the thumbnail of the others. A tap opens the photo viewer
 * on it, which gets the largest size once downloaded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "telegram.h"

#define MAX_LENGTH 1024  /* characters typed in a message */
#define PHOTO_MAX_W 540  /* photos on screen: 75% of the width */
#define PHOTO_MAX_H 640
#define STICKER 256      /* stickers on screen, across */
#define BIG_EMOJI 160    /* large emoji (a message of one emoji), across */

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
	/* The largest size, for the viewer: downloaded when the photo is tapped */
	int full_file;
	char *full_path;
	/* A sticker: shown like a photo, without a background; its emoji (composed
	 * for the fonts) until then, or if it cannot be read */
	bool sticker;
	char *emoji;
	bool big;         /* only an emoji (large emoji, dice, a sticker without an image): drawn large */
	/* A reply: the message it answers (reply_id, in this chat, else 0); who wrote it and
	 * its text (the part quoted) as given with it (another chat's), else from that
	 * message if loaded, else asked for */
	bool reply;
	long long reply_id;
	char *reply_name, *reply_text;
	bool reply_asked;
};

/* The open chat */
static struct {
	lv_obj_t *scr;
	struct ui_chat chat;
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

/* A chat to open once the one shown is gone (a notification of another tapped) */
static long long pending;

static void pending_open_cb(void *data)
{
	LV_UNUSED(data);
	long long id = pending;
	pending = 0;
	if (id)
		chat_open(id);
}

/* Messages */

static void photo_drop(struct msg *m)
{
	ui_texture_delete(m->tex);
	free(m->photo_path);
	free(m->full_path);
	m->photo_file = m->full_file = 0;
	m->photo_path = m->full_path = NULL;
	m->tex = 0;
	m->photo_bad = false;
	m->photo = NULL;
}

static void msg_free(struct msg *m)
{
	free(m->text);
	free(m->reply_name);
	free(m->reply_text);
	free(m->emoji);
	m->emoji = NULL;
	photo_drop(m);
}

/* A file's path once downloaded, else NULL */
static const char *file_path(const struct json *file)
{
	const struct json *local = json_get(file, "local");
	const char *path = json_str(json_get(local, "path"), "");
	return json_bool(json_get(local, "is_downloading_completed")) && path[0] ? path : NULL;
}

/* A downloaded file: its path to the photos showing it; a largest size goes to
 * the viewer if it shows that photo */
void photo_file(const struct json *file)
{
	const char *path = file_path(file);
	if (!path)
		return;
	int id = json_int(json_get(file, "id"), 0);
	for (unsigned i = 0; i < cv.count; i++) {
		struct msg *m = &cv.msgs[i];
		if (m->photo_file == id && !m->photo_path)
			m->photo_path = strdup(path);
		if (m->full_file == id && !m->full_path) {
			m->full_path = strdup(path);
			if (m->photo_path && m->full_file != m->photo_file)
				ui_photo_view_replace(m->photo_path, path);
		}
	}
}

/* The file to show for a photo or sticker: its path if downloaded (or known from
 * another message), else downloaded */
static void photo_use(struct msg *m, const struct json *file)
{
	int id = json_int(json_get(file, "id"), 0);
	if (id == m->photo_file)
		return;
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

/* A sticker: the static one, else its thumbnail if it is an image (animated and
 * video stickers), size across at most. Its emoji, drawn large, without one
 * (also when st is NULL). */
static void sticker_set(struct msg *m, const struct json *st, const char *emoji, int size)
{
	const struct json *file = NULL;
	int w = json_int(json_get(st, "width"), 0), h = json_int(json_get(st, "height"), 0);
	if (!strcmp(json_type(json_get(st, "format")), "stickerFormatWebp")) {
		file = json_get(st, "sticker");
	} else {
		const struct json *thumb = json_get(st, "thumbnail");
		const char *format = json_type(json_get(thumb, "format"));
		if (!strcmp(format, "thumbnailFormatWebp") || !strcmp(format, "thumbnailFormatJpeg")) {
			file = json_get(thumb, "file");
			w = json_int(json_get(thumb, "width"), w);
			h = json_int(json_get(thumb, "height"), h);
		}
	}
	free(m->emoji);
	m->emoji = ui_emoji_compose(emoji);
	free(m->text);
	if (!file || w <= 0 || h <= 0) {
		photo_drop(m);
		m->sticker = false;
		m->big = m->emoji && m->emoji[0];
		m->text = strdup(m->big ? m->emoji : _("Sticker"));
		return;
	}
	m->text = strdup("");
	m->sticker = true;
	photo_use(m, file);
	/* Thumbnails are smaller: shown as large as the sticker would be */
	float scale = (float)size / (w > h ? w : h);
	m->photo_w = w * scale > 1 ? w * scale : 1;
	m->photo_h = h * scale > 1 ? h * scale : 1;
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

	photo_use(m, json_get(best, "photo"));
	const struct json *full = json_get(largest, "photo");
	int full_id = json_int(json_get(full, "id"), 0);
	if (full_id != m->full_file) {
		free(m->full_path);
		m->full_file = full_id;
		const char *path = file_path(full);
		m->full_path = path ? strdup(path) : NULL;
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
	/* A sticker's emoji would show through it */
	lv_obj_set_hidden(lv_obj_get_child(m->photo, 0), true);
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
		m->tex = m->sticker ? sticker_texture(m->photo_path, m->photo_w) :
		         jpeg_texture(m->photo_path, m->photo_w, false);
		m->photo_bad = !m->tex;
		photo_show(m);
		return;
	}
}

static void msg_content(struct msg *m, const struct json *content)
{
	free(m->text);
	m->text = message_text(content, &m->service);
	m->sticker = false;
	m->big = false;
	const char *type = json_type(content);
	if (!strcmp(type, "messagePhoto")) {
		photo_set(m, content);
	} else if (!strcmp(type, "messageSticker")) {
		const struct json *st = json_get(content, "sticker");
		sticker_set(m, st, json_str(json_get(st, "emoji"), ""), STICKER);
	} else if (!strcmp(type, "messageAnimatedEmoji")) {
		/* Its sticker is animated (TGS): its thumbnail if it has one */
		sticker_set(m, json_get(json_get(content, "animated_emoji"), "sticker"),
		            json_str(json_get(content, "emoji"), ""), BIG_EMOJI);
	} else {
		/* Dice: the emoji, not the value thrown */
		m->big = !strcmp(type, "messageDice");
		photo_drop(m);
	}
}

/* Who sent a message another chat's reply came from, "" if not said */
static const char *origin_name(const struct json *origin)
{
	const char *type = json_type(origin);
	if (!strcmp(type, "messageOriginUser"))
		return sender_name(json_int(json_get(origin, "sender_user_id"), 0));
	if (!strcmp(type, "messageOriginHiddenUser"))
		return json_str(json_get(origin, "sender_name"), "");
	if (!strcmp(type, "messageOriginChat"))
		return sender_name(json_int(json_get(origin, "sender_chat_id"), 0));
	if (!strcmp(type, "messageOriginChannel"))
		return sender_name(json_int(json_get(origin, "chat_id"), 0));
	return "";
}

/* What a messageReplyTo* says of the message answered: the part quoted, or another
 * chat's message's sender and content; the rest comes from the message (reply_find()) */
static void msg_reply_to(struct msg *m, const struct json *to)
{
	free(m->reply_name);
	free(m->reply_text);
	m->reply_name = m->reply_text = NULL;
	m->reply_id = 0;
	m->reply_asked = false;
	m->reply = !strcmp(json_type(to), "messageReplyToMessage");
	if (!m->reply)
		return;
	if (json_int(json_get(to, "chat_id"), cv.chat_id) == cv.chat_id)
		m->reply_id = json_int(json_get(to, "message_id"), 0);
	const char *name = origin_name(json_get(to, "origin"));
	if (name[0])
		m->reply_name = strdup(name);
	const char *quote = json_str(json_get(json_get(json_get(to, "quote"), "text"), "text"), "");
	const struct json *content = json_get(to, "content");
	bool service;
	if (quote[0])
		m->reply_text = ui_emoji_compose(quote);
	else if (content && content->kind == JSON_OBJECT)
		m->reply_text = message_text(content, &service);
	/* Another chat's, or not known: nothing to ask for */
	if (!m->reply_id) {
		if (!m->reply_name)
			m->reply_name = strdup("");
		if (!m->reply_text)
			m->reply_text = strdup(_("Message"));
	}
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
	msg_reply_to(m, json_get(j, "reply_to"));
}

static struct msg *msg_find(long long id)
{
	for (unsigned i = 0; i < cv.count; i++)
		if (cv.msgs[i].id == id)
			return &cv.msgs[i];
	return NULL;
}

/* A photo tapped: the viewer, with the size shown until the largest one is
 * downloaded (photo_file replaces it) */
static void photo_tap_cb(lv_event_t *e)
{
	struct msg *m = msg_find((intptr_t)lv_event_get_user_data(e));
	if (!m || !m->photo_path)
		return;
	const char *path = m->full_path ? m->full_path : m->photo_path;
	ui_photo_view(_("Photo"), &path, 1, 0, NULL);
	if (!m->full_path && m->full_file) {
		char buf[128];
		snprintf(buf, sizeof(buf), "{\"@type\":\"downloadFile\",\"file_id\":%d,\"priority\":32,"
			"\"offset\":0,\"limit\":0,\"synchronous\":false,\"@extra\":\"download\"}", m->full_file);
		td_request(buf);
	}
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

static void view_fill(void);

static lv_obj_t *centered_label(const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(cv.chat.list);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

/* Who wrote a message of the chat, and its text on one line (ui_chat_quote_create()) */
static const char *msg_author(const struct msg *m)
{
	return m->outgoing ? _("You") : sender_name(m->sender);
}

static const char *msg_summary(const struct msg *m)
{
	return m->text[0] ? m->text : m->sticker ? _("Sticker") : m->photo_file ? _("Photo") : _("Message");
}

/* The message a reply answers: as given with it, else from that message, else asked
 * for (chat_replied_answer()) */
static void reply_find(struct msg *m, const char **name, const char **text)
{
	const struct msg *to = m->reply_id ? msg_find(m->reply_id) : NULL;
	*name = m->reply_name ? m->reply_name : to ? msg_author(to) : "";
	*text = m->reply_text ? m->reply_text : to ? msg_summary(to) : _("Loading...");
	if (to || (m->reply_name && m->reply_text) || m->reply_asked || m->state != SENT)
		return;
	char buf[192];
	/* NON-NLS-BEGIN: TDLib's JSON */
	snprintf(buf, sizeof(buf), "{\"@type\":\"getRepliedMessage\",\"chat_id\":%lld,\"message_id\":%lld,"
		"\"@extra\":\"replied %lld %lld\"}", cv.chat_id, m->id, cv.chat_id, m->id);
	/* NON-NLS-END */
	td_request(buf);
	m->reply_asked = true;
}

void chat_replied_answer(const struct json *u)
{
	long long chat = 0, id = 0;
	sscanf(json_str(json_get(u, "@extra"), ""), "replied %lld %lld", &chat, &id);
	struct msg *m = cv.scr && chat == cv.chat_id ? msg_find(id) : NULL;
	if (!m)
		return;
	if (strcmp(json_type(u), "message")) {
		/* Deleted since */
		if (!m->reply_name)
			m->reply_name = strdup("");
		free(m->reply_text);
		m->reply_text = strdup(_("Deleted message"));
	} else {
		/* Not kept with the chat's: its photos are not downloaded */
		const struct json *sender = json_get(u, "sender_id");
		long long from = json_int(json_get(sender, !strcmp(json_type(sender), "messageSenderChat") ?
			"chat_id" : "user_id"), 0);
		if (!m->reply_name)
			m->reply_name = strdup(json_bool(json_get(u, "is_outgoing")) ? _("You") : sender_name(from));
		if (!m->reply_text) {
			bool service;
			m->reply_text = message_text(json_get(u, "content"), &service);
		}
	}
	view_fill();
}

/* Swiped: replying to it */
static void view_reply(struct ui_chat *chat, long long id)
{
	struct msg *m = msg_find(id);
	if (m)
		ui_chat_reply(chat, id, msg_author(m), msg_summary(m));
}

/* The quote of the message a reply answers, at the top of its bubble */
static void reply_show(struct msg *m, lv_obj_t *label)
{
	const char *name, *text;
	reply_find(m, &name, &text);
	ui_chat_quote_create(label, m->outgoing, name, text);
}

static void view_add(struct msg *m)
{
	char day[32], when[8];
	day_format(day, sizeof(day), m->date);
	if (strcmp(day, cv.day)) {
		snprintf(cv.day, sizeof(cv.day), "%s", day);
		lv_obj_t *label = centered_label(UI_FONT_CAPTION);
		ui_set_text(label, day);
		lv_obj_set_style_margin_top(label, 8, 0);
		cv.group_obj = NULL;
	}
	if (m->service) {
		lv_label_set_text(centered_label(UI_FONT_CAPTION), m->text);
		cv.group_obj = NULL;
		return;
	}

	time_t t = m->date;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(when, sizeof(when), "%H:%M", &tm);
	if (!cv.group_obj || cv.group_sender != m->sender || cv.group_outgoing != m->outgoing ||
	    cv.group_state != (int)m->state || strcmp(cv.group_time, when)) {
		lv_obj_t *time_label;
		cv.group_obj = ui_chat_group_create(&cv.chat, m->outgoing, &time_label);
		if (cv.group && !m->outgoing) {
			lv_obj_t *name = lv_label_create(cv.group_obj);
			ui_set_text(name, sender_name(m->sender));
			lv_obj_set_style_text_font(name, UI_FONT_CAPTION, 0);
			lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_ACCENT), 0);
			lv_obj_set_style_pad_hor(name, 12, 0);
			lv_obj_move_to_index(name, 0);
		}
		if (m->state == SENDING)
			lv_label_set_text_static(time_label, _("Sending..."));
		else if (m->state == NOT_SENT)
			ui_set_text_fmt(time_label, "%s  " LV_SYMBOL_WARNING " %s", when, _("Not sent"));
		else
			ui_set_text(time_label, when);
		if (m->state == NOT_SENT)
			lv_obj_set_style_text_color(time_label, lv_color_hex(UI_COLOR_RED), 0);

		cv.group_sender = m->sender;
		cv.group_outgoing = m->outgoing;
		cv.group_state = m->state;
		snprintf(cv.group_time, sizeof(cv.group_time), "%s", when);
	}

	/* A photo's reply: the quote in a bubble of its own, above it */
	if (m->reply && m->photo_file) {
		lv_obj_t *label = ui_chat_bubble_create(cv.group_obj, m->outgoing);
		reply_show(m, label);
		lv_obj_set_hidden(label, true);
	}
	if (m->photo_file) {
		/* An opaque box: a placeholder until the texture covers it; a sticker's
		 * has its emoji on nothing */
		m->photo = ui_box(cv.group_obj);
		lv_obj_set_size(m->photo, m->photo_w, m->photo_h);
		lv_obj_move_to_index(m->photo, (int32_t)lv_obj_get_child_count(cv.group_obj) - 2);
		lv_obj_t *icon = lv_label_create(m->photo);
		lv_obj_set_style_text_font(icon, &ui_font_48, 0);
		lv_obj_center(icon);
		if (m->sticker) {
			lv_label_set_text(icon, m->emoji ? m->emoji : "");
		} else {
			lv_obj_set_style_bg_opa(m->photo, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(m->photo, lv_color_hex(UI_COLOR_CARD), 0);
			lv_label_set_text_static(icon, m->photo_bad ? LV_SYMBOL_WARNING : LV_SYMBOL_IMAGE);
			lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);
			lv_obj_set_clickable(m->photo, true);
			lv_obj_add_event_cb(m->photo, photo_tap_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)m->id);
		}
		photo_show(m);
		if (m->state == SENT)
			ui_chat_replyable(&cv.chat, m->photo, m->id);
		if (!m->text[0])
			return;
	}

	lv_obj_t *label = ui_chat_bubble_create(cv.group_obj, m->outgoing);
	lv_label_set_text(label, m->text);
	if (m->reply && !m->photo_file)
		reply_show(m, label);
	if (m->big) {
		lv_obj_set_style_bg_opa(lv_obj_get_parent(label), LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_all(lv_obj_get_parent(label), 0, 0);
		lv_obj_set_style_text_font(label, UI_FONT_DISPLAY, 0);
	}
	/* Not on the server yet: paler */
	if (m->state != SENT)
		lv_obj_set_style_bg_opa(lv_obj_get_parent(label), LV_OPA_60, 0);
	else
		ui_chat_replyable(&cv.chat, lv_obj_get_parent(label), m->id);
}

/* At the end, or on the way there: only scrolls to the end are animated */
static bool view_at_end(void)
{
	return lv_obj_get_scroll_bottom(cv.chat.list) < 200 || lv_anim_get(cv.chat.list, NULL);
}

/* Rebuilt when older messages come or messages change; the distance to the
 * end of the list stays the same, or it stays at the end */
static void view_fill(void)
{
	/* Emptying the list and scrolling back send SCROLL_END: without this, each
	 * rebuild looked like a scroll to the top and loaded the whole history */
	cv.filling = true;
	lv_obj_update_layout(cv.chat.list);
	bool end = view_at_end();
	/* A running scroll aims at where the end was */
	lv_anim_delete(cv.chat.list, NULL);
	int32_t bottom = end ? 0 : lv_obj_get_scroll_bottom(cv.chat.list);
	lv_obj_clean(cv.chat.list);
	cv.group_obj = NULL;
	cv.day[0] = '\0';

	if (!cv.count) {
		lv_obj_t *hint = centered_label(UI_FONT_BODY);
		lv_label_set_text_static(hint, cv.at_start ? _("No messages") : _("Loading..."));
		lv_obj_set_style_margin_top(hint, 200, 0);
	}
	for (unsigned i = 0; i < cv.count; i++)
		view_add(&cv.msgs[i]);

	lv_obj_update_layout(cv.chat.list);
	lv_obj_scroll_to_y(cv.chat.list, lv_obj_get_scroll_y(cv.chat.list) + lv_obj_get_scroll_bottom(cv.chat.list) - bottom,
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
	/* NON-NLS-BEGIN: TDLib's JSON */
	snprintf(buf, sizeof(buf), "{\"@type\":\"getChatHistory\",\"chat_id\":%lld,\"from_message_id\":%lld,"
		"\"offset\":0,\"limit\":%d,\"only_local\":false,\"@extra\":\"history %lld\"}",
		cv.chat_id, cv.count ? cv.msgs[0].id : 0, PAGE, cv.chat_id);
	/* NON-NLS-END */
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
				lv_obj_clean(cv.chat.list); /* the hint */
			msg_insert(j);
			view_add(&cv.msgs[cv.count - 1]);
		} else {
			msg_insert(j);
			view_fill();
		}
		if (end) {
			/* From where it is, not from the end of a scroll still running */
			lv_anim_delete(cv.chat.list, NULL);
			ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
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
	if (!cv.filling && lv_obj_get_scroll_top(cv.chat.list) < 600)
		history_load();
}

/* The message replied to, for a sendMessage: "" if none */
static void reply_json(char *buf, size_t size, long long id)
{
	if (id)
		snprintf(buf, size, ",\"reply_to\":{\"@type\":\"inputMessageReplyToMessage\",\"message_id\":%lld}", id);
	else
		buf[0] = '\0';
}

/* It shows up through updateNewMessage, pending until the server has it */
static void view_send(struct ui_chat *chat, const char *text)
{
	char reply[128];
	reply_json(reply, sizeof(reply), chat->reply_to);
	size_t n = strlen(text);
	char *escaped = malloc(n * 6 + 1);
	char *buf = malloc(n * 6 + 256);
	if (escaped && buf) {
		json_escape(escaped, n * 6 + 1, text);
		snprintf(buf, n * 6 + 256, "{\"@type\":\"sendMessage\",\"chat_id\":%lld,"
			"\"input_message_content\":{\"@type\":\"inputMessageText\","
			"\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\"}}%s,\"@extra\":\"send\"}",
			cv.chat_id, escaped, reply);
		td_request(buf);
	}
	free(escaped);
	free(buf);
}

/* A photo from the Gallery, to the chat it was picked for; it shows up like a
 * text message. Screenshots (PPM) are sent as JPEG, kept in DIR/sent. */
static void photo_picked(const char *path, void *user_data)
{
	long long chat_id = (intptr_t)user_data;
	char jpeg[256];
	int w = 0, h = 0;
	const char *ext = strrchr(path, '.');
	if (ext && !strcmp(ext, ".ppm")) {
		const char *name = strrchr(path, '/') + 1;
		mkdir(DIR "/sent", 0700);
		snprintf(jpeg, sizeof(jpeg), DIR "/sent/%.*s.jpg", (int)(ext - name), name);
		if (!ppm_to_jpeg(path, jpeg, &w, &h)) {
			printf("telegram: cannot convert %s\n", path);
			return;
		}
		path = jpeg;
	} else if (!jpeg_size(path, &w, &h)) {
		printf("telegram: cannot read %s\n", path);
		return;
	}

	/* Replying in that chat: the photo is the reply */
	char reply[128];
	reply_json(reply, sizeof(reply), cv.scr && cv.chat_id == chat_id ? cv.chat.reply_to : 0);
	char escaped[512], buf[1024];
	json_escape(escaped, sizeof(escaped), path);
	snprintf(buf, sizeof(buf), "{\"@type\":\"sendMessage\",\"chat_id\":%lld,"
		"\"input_message_content\":{\"@type\":\"inputMessagePhoto\",\"photo\":{\"@type\":\"inputPhoto\","
		"\"photo\":{\"@type\":\"inputFileLocal\",\"path\":\"%s\"},\"width\":%d,\"height\":%d}}%s,"
		"\"@extra\":\"send\"}", chat_id, escaped, w, h, reply);
	td_request(buf);
	if (reply[0])
		ui_chat_reply(&cv.chat, 0, NULL, NULL);
}

static void attach_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_photo_pick(_("Send a photo"), photo_picked, (void *)(intptr_t)cv.chat_id);
}

/* Refused before it was sent (no right to write in this chat, ...) */
void chat_send_error(const struct json *u)
{
	const char *message = json_str(json_get(u, "message"), "");
	printf("telegram: sendMessage: %s\n", message);
	if (!cv.scr)
		return;
	lv_obj_t *label = centered_label(UI_FONT_BODY);
	char text[256];
	snprintf(text, sizeof(text), _("Not sent: %s"), message);
	ui_set_text_fmt(label, LV_SYMBOL_WARNING " %s", text);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_RED), 0);
	cv.group_obj = NULL;
	ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
}

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"closeChat\",\"chat_id\":%lld}", cv.chat_id);
	td_request(buf);
	msgs_free();
	memset(&cv, 0, sizeof(cv));
	if (pending)
		lv_async_call(pending_open_cb, NULL);
}

void chat_open(long long id)
{
	struct chat *c = chat_find(id);
	if (!c)
		return;
	if (cv.scr) {
		/* Another one on screen: back from it, then this one */
		if (cv.chat_id != id && lv_screen_active() == cv.scr) {
			pending = id;
			ui_pop();
		}
		return;
	}

	lv_obj_t *body;
	/* The title must stay valid: the chat's can change */
	cv.scr = telegram_screen(ui_intern(chat_title(c)), NULL, &body);
	lv_obj_add_event_cb(cv.scr, view_delete_cb, LV_EVENT_DELETE, NULL);
	cv.chat_id = id;
	cv.group = c->group;

	cv.chat.send = view_send;
	cv.chat.reply = view_reply;
	ui_chat_create(&cv.chat, body, MAX_LENGTH);
	lv_obj_add_event_cb(cv.chat.list, view_scroll_cb, LV_EVENT_SCROLL_END, NULL);
	/* Photos from the Gallery: a button before the text field */
	lv_obj_t *attach = ui_button(lv_obj_get_parent(cv.chat.ta), 80, 80, UI_COLOR_BUTTON,
	                             UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_IMAGE, attach_cb, NULL);
	lv_obj_move_to_index(attach, 0);

	/* Open: TDLib sends its updates and counts it as viewed */
	char buf[96];
	snprintf(buf, sizeof(buf), "{\"@type\":\"openChat\",\"chat_id\":%lld}", id);
	td_request(buf);
	view_fill();
	history_load();
	ui_push(cv.scr);
}

long long chat_shown(void)
{
	return cv.scr && lv_screen_active() == cv.scr && !ui_locked() ? cv.chat_id : 0;
}

/* One photo decoded per poll */
void chat_poll(void)
{
	if (cv.scr)
		photo_decode_next();
}
