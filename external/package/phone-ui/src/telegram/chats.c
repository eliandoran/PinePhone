/*
 * Telegram's chats and users, kept from TDLib's updates while the client runs
 * (the screen may be closed), and the chats' avatars: downloaded by TDLib, then
 * decoded into round textures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

/* Chats */

struct chat *chat_find(long long id)
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

static void avatar_drop(struct chat *c)
{
	texture_delete(c->avatar_tex);
	free(c->avatar_path);
	c->avatar_file = 0;
	c->avatar_path = NULL;
	c->avatar_tex = 0;
	c->avatar_bad = false;
}

void chats_clear(void)
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

/* The next page of the main chat list: they arrive as updates */
void load_chats(void)
{
	if (tg.loading || tg.all_loaded || tg.state != ST_READY)
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"@type\":\"loadChats\",\"chat_list\":{\"@type\":\"chatListMain\"},"
		"\"limit\":%d,\"@extra\":\"chats\"}", PAGE);
	td_request(buf);
	tg.loading = true;
}

/* loadChats: 404 when the list has no more chats */
void chats_loaded(const struct json *u)
{
	tg.loading = false;
	if (!strcmp(json_type(u), "error")) {
		if (json_int(json_get(u, "code"), 0) == 404)
			tg.all_loaded = true;
		else
			printf("telegram: loadChats: %s\n", json_str(json_get(u, "message"), ""));
	}
	tg.chats_changed = true;
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
void avatar_file(const struct json *file)
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

/* Avatars downloaded and not decoded yet, those in the list first: they are
 * small (160 px, decoded at half size), a few per poll */
void avatar_decode_next(void)
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

static void chat_last_message(struct chat *c, const struct json *msg)
{
	free(c->preview);
	c->preview = message_preview(msg);
	c->date = json_int(json_get(msg, "date"), 0);
}

/* Updates of the chat list; false if the update is not one of them */
bool chat_update(const char *type, const struct json *u)
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

void user_update(const struct json *user)
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
const char *sender_name(long long id)
{
	struct user *u = user_find(id);
	if (u && u->name && u->name[0])
		return u->name;
	struct chat *c = chat_find(id);
	return c ? c->title : "";
}
