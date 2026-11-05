/*
 * Telegram's text: messages as text for labels (emoji sequences composed, see
 * ui_emoji_compose()), one-line previews, times and days.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define PREVIEW_MAX 120 /* bytes of the last message shown in the chat list */

/* For labels: emoji sequences as one character */
void set_str(char **dst, const char *src)
{
	free(*dst);
	*dst = ui_emoji_compose(src ? src : "");
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
		json_str(json_get(json_get(content, "text"), "text"), "") :
		!strcmp(type, "messageAnimatedEmoji") || !strcmp(type, "messageDice") ?
		json_str(json_get(content, "emoji"), "") : content_kind(type);
	return ui_one_line(json_bool(json_get(msg, "is_outgoing")) ? "You: " : "", text ? text : "Message", PREVIEW_MAX);
}

/* The whole text of a message: the kind of content, then its caption */
char *message_text(const struct json *content, bool *service)
{
	const char *type = json_type(content);
	*service = false;
	if (!strcmp(type, "messageText"))
		return ui_emoji_compose(json_str(json_get(json_get(content, "text"), "text"), ""));
	if (!strcmp(type, "messageAnimatedEmoji") || !strcmp(type, "messageDice"))
		return ui_emoji_compose(json_str(json_get(content, "emoji"), ""));
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
