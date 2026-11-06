/*
 * Telegram: signs in to Telegram through TDLib's JSON interface, lists the chats
 * (name, last message, time, unread count, avatar) and opens them: messages as
 * bubbles, photos, sending text. Shared between its files:
 *
 * - telegram.c  the app's screen: the login, or the chats and the profile, switched by
 *               a bar at the bottom
 * - client.c    TDLib: loading it, the client, the timer polling it, which hands
 *               updates and answers to the files below
 * - login.c     signing in and logging out
 * - profile.c   the account: its photo, name, username, bio, changing them
 * - chats.c     chats and users, kept from TDLib's updates; their avatars
 * - list.c      the chat lists: the main one, the archive
 * - chat.c      an open chat: messages, photos, sending
 * - text.c      messages as text for labels, dates
 * - media.c     JPEG photos and avatars decoded into GL textures, drawn avatars
 * - plugin.c    what phone-ui looks for in telegram.so
 */

#ifndef PHONE_UI_TELEGRAM_H
#define PHONE_UI_TELEGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "i18n.h"
#include "json.h"
#include "ui.h"

#define DIR "/var/lib/telegram"
#define API_FILE DIR "/api"
#define LIB "libtdjson.so"
#define PAGE 50   /* chats or messages loaded at a time */
#define AVATAR 80 /* avatars in the chat list */

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

/* The chat lists shown: the main one and the archive */
enum list {
	LIST_MAIN,
	LIST_ARCHIVE,
	LISTS,
};

struct chat {
	long long id;
	long long order[LISTS]; /* in each list, 0: not in it */
	bool pinned[LISTS];
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

/* TDLib and the session: outlive the screen */
struct telegram {
	bool loaded;
	int (*create_client_id)(void);
	void (*send)(int client_id, const char *request);
	const char *(*receive)(double timeout);
	const char *(*execute)(const char *request);
	int client;
	lv_timer_t *poll;
	enum state state;
	int api_id;
	char api_hash[64];

	/* Chats (chats.c) */
	struct chat *chats;
	unsigned chat_count, chat_cap;
	bool chats_changed;
	/* Per list */
	bool loading[LISTS];    /* loadChats on its way */
	bool all_loaded[LISTS]; /* the list has no more chats */

	/* Names of the senders, from updateUser (chats.c) */
	struct user *users;
	unsigned user_count, user_cap;

	long long me; /* the account's user id (getMe), 0: not known yet */
};
extern struct telegram tg;

/* telegram.c */
void telegram_open(void);  /* the app (plugin.c) */
void telegram_show(void); /* the login or the chats, after a change */

/* client.c */
void telegram_start(void);          /* TDLib, once; tg.state says what is missing */
void client_start(void);            /* a new client (also after logging out) */
void send_parameters(void);
void td_request(const char *json);  /* "@extra" tags its answer */

/* login.c */
void login_create(lv_obj_t *body);  /* the form, then its keyboard */
void login_show(bool shown);
void login_auth_state(const struct json *update);
void login_error(const struct json *error);
void login_me(const struct json *user);
void log_out(void);

/* profile.c */
void profile_create(lv_obj_t *body);
void profile_show(bool shown);
void profile_refresh(void);
void profile_clear(void);                        /* logged out */
void profile_me(const struct json *user);        /* getMe, updateUser of the account */
void profile_full_info(const struct json *info); /* its userFullInfo: the bio */
void profile_saved(const struct json *answer);   /* ok or error, of a change */
void profile_file(const struct json *file);      /* downloaded */

/* chats.c */
void chats_clear(void);
void load_chats(enum list list);    /* the next page; they come as updates */
void load_first_chats(void);        /* the first page of the lists that have none */
void chats_loaded(enum list list, const struct json *answer);
bool chat_update(const char *type, const struct json *update); /* false: not about chats */
struct chat *chat_find(long long id);
void user_update(const struct json *user);
const char *sender_name(long long id);
/* Saved messages (the chat with oneself) has its own title and avatar */
const char *chat_title(const struct chat *c);
unsigned chat_avatar_tex(const struct chat *c);
void avatar_file(const struct json *file); /* downloaded */
void avatar_decode_next(void);

/* list.c */
void list_create(lv_obj_t *body);
void list_show(bool shown);
void list_poll(void);               /* refreshed if the chats changed */
void archive_open(void);            /* the archived chats' screen */

/* chat.c */
void chat_open(long long id);
void chat_update_messages(const char *type, const struct json *update);
void chat_history_answer(const struct json *answer);
void chat_send_error(const struct json *error);
void photo_file(const struct json *file); /* downloaded */
void chat_poll(void);

/* text.c */
void set_str(char **dst, const char *src);
char *message_preview(const struct json *msg);
char *message_text(const struct json *content, bool *service);
void day_format(char *buf, size_t size, time_t t);

/* media.c */
unsigned jpeg_texture(const char *path, int width, bool round);
unsigned sticker_texture(const char *path, int width); /* WEBP or JPEG, with alpha */
unsigned saved_texture(int size);   /* Saved messages' avatar */
unsigned archive_texture(int size); /* the archive's */
unsigned pin_texture(int size, uint32_t color); /* a pinned chat's mark */
bool jpeg_size(const char *path, int *w, int *h);
bool ppm_to_jpeg(const char *path, const char *out, int *w, int *h);

#endif
