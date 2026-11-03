/*
 * Telegram's text: messages as text for labels (emoji sequences composed, see
 * ui_emoji_compose()), one-line previews, times and days, JSON strings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define PREVIEW_MAX 120 /* bytes of the last message shown in the chat list */

/* text as a JSON string body */
void json_escape(char *out, size_t size, const char *text)
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
void set_str(char **dst, const char *src)
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

char *message_preview(const struct json *msg)
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
char *message_text(const struct json *content, bool *service)
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

void day_format(char *buf, size_t size, time_t t)
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
