/*
 * Notifications: TDLib's own (notification groups, asked for with the option
 * notification_group_count_max), which leave out muted chats and go once read, here or on
 * another device, posted to phone-ui's (ui_notify()), one per group: the chat's title, its
 * last message (who wrote it, in a group), how many. One added alerts (a banner) unless it
 * is silent or its chat is on screen. Tapped, the chat opens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define GROUPS 25 /* notification_group_count_max: TDLib's most */
#define NOTES 10  /* kept per group: TDLib's notification_group_size_max by default */

struct note {
	int id;
	long long date;
	char *text;
};

static struct group {
	int id;
	long long chat_id;
	int total;               /* TDLib's count: more than those it sent */
	struct note notes[NOTES]; /* oldest first */
	unsigned count;
} groups[GROUPS];
static unsigned group_count;

static struct group *group_find(int id)
{
	for (unsigned i = 0; i < group_count; i++)
		if (groups[i].id == id)
			return &groups[i];
	return NULL;
}

/* What a notification says: its message, after who wrote it in a group */
static char *note_text(const struct json *notification, long long chat_id)
{
	const struct json *type = json_get(notification, "type");
	const struct json *msg = json_get(type, "message");
	if (strcmp(json_type(type), "notificationTypeNewMessage") || !msg)
		return strdup(_("Message"));
	char *text = message_preview(msg);
	struct chat *c = chat_find(chat_id);
	if (!c || !c->group || !text)
		return text;
	const struct json *sender = json_get(msg, "sender_id");
	long long id = json_int(json_get(sender, "user_id"), json_int(json_get(sender, "chat_id"), 0));
	const char *name = sender_name(id);
	if (!name || !name[0])
		return text;
	size_t size = strlen(name) + strlen(text) + 3;
	char *named = malloc(size);
	if (!named)
		return text;
	snprintf(named, size, "%s: %s", name, text); /* NON-NLS */
	free(text);
	return named;
}

static void note_remove(struct group *g, unsigned i)
{
	free(g->notes[i].text);
	memmove(&g->notes[i], &g->notes[i + 1], (g->count - i - 1) * sizeof(g->notes[0]));
	g->count--;
}

static void note_add(struct group *g, const struct json *notification)
{
	if (g->count == NOTES)
		note_remove(g, 0);
	struct note *n = &g->notes[g->count++];
	n->id = json_int(json_get(notification, "id"), 0);
	n->date = json_int(json_get(notification, "date"), 0);
	n->text = note_text(notification, g->chat_id);
}

static void open_cb(const char *key)
{
	struct group *g = group_find(atoi(key));
	if (g)
		telegram_open_chat(g->chat_id);
}

/* To phone-ui, or withdrawn there and forgotten when empty */
static void post(struct group *g, bool alert)
{
	char key[16];
	snprintf(key, sizeof(key), "%d", g->id);
	if (!g->count || g->total <= 0) {
		ui_notify("Telegram", key, NULL, NULL, 0, 0, false, NULL); /* NON-NLS: app.json's name */
		for (unsigned i = 0; i < g->count; i++)
			free(g->notes[i].text);
		*g = groups[--group_count];
		return;
	}
	struct chat *c = chat_find(g->chat_id);
	const struct note *last = &g->notes[g->count - 1];
	ui_notify("Telegram", key, c ? chat_title(c) : "Telegram", last->text, /* NON-NLS */
	          (unsigned)g->total, (time_t)last->date, alert && chat_shown() != g->chat_id, open_cb);
}

/* A group as TDLib sends it whole (updateActiveNotifications) */
static void group_set(const struct json *group)
{
	int id = json_int(json_get(group, "id"), 0);
	struct group *g = group_find(id);
	if (!g) {
		if (group_count == GROUPS)
			return;
		g = &groups[group_count++];
		memset(g, 0, sizeof(*g));
		g->id = id;
	}
	while (g->count)
		note_remove(g, g->count - 1);
	g->chat_id = json_int(json_get(group, "chat_id"), 0);
	g->total = json_int(json_get(group, "total_count"), 0);
	for (const struct json *n = json_get(group, "notifications") ? json_get(group, "notifications")->child : NULL;
	     n; n = n->next)
		note_add(g, n);
	post(g, false);
}

static void group_update(const struct json *u)
{
	int id = json_int(json_get(u, "notification_group_id"), 0);
	struct group *g = group_find(id);
	if (!g) {
		if (group_count == GROUPS)
			return;
		g = &groups[group_count++];
		memset(g, 0, sizeof(*g));
		g->id = id;
	}
	g->chat_id = json_int(json_get(u, "chat_id"), 0);
	g->total = json_int(json_get(u, "total_count"), 0);
	const struct json *removed = json_get(u, "removed_notification_ids");
	for (const struct json *r = removed ? removed->child : NULL; r; r = r->next)
		for (unsigned i = 0; i < g->count; i++)
			if (g->notes[i].id == json_int(r, 0)) {
				note_remove(g, i);
				break;
			}
	bool alert = false;
	const struct json *added = json_get(u, "added_notifications");
	for (const struct json *n = added ? added->child : NULL; n; n = n->next) {
		note_add(g, n);
		alert |= !json_bool(json_get(n, "is_silent"));
	}
	post(g, alert);
}

/* A notification's message edited */
static void note_update(const struct json *u)
{
	struct group *g = group_find(json_int(json_get(u, "notification_group_id"), 0));
	const struct json *notification = json_get(u, "notification");
	int id = json_int(json_get(notification, "id"), 0);
	for (unsigned i = 0; g && i < g->count; i++)
		if (g->notes[i].id == id) {
			free(g->notes[i].text);
			g->notes[i].text = note_text(notification, g->chat_id);
			post(g, false);
			return;
		}
}

bool notify_update(const char *type, const struct json *u)
{
	if (!strcmp(type, "updateActiveNotifications")) {
		const struct json *list = json_get(u, "groups");
		for (const struct json *g = list ? list->child : NULL; g; g = g->next)
			group_set(g);
	} else if (!strcmp(type, "updateNotificationGroup")) {
		group_update(u);
	} else if (!strcmp(type, "updateNotification")) {
		note_update(u);
	} else {
		return false;
	}
	return true;
}

void notify_clear(void)
{
	while (group_count) {
		groups[0].total = 0;
		post(&groups[0], false);
	}
}
