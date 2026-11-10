/*
 * TDLib's JSON interface for test_telegram.c: see fake_tdjson.h. One client at a time
 * (a new one after logging out), everything on the caller's thread: a request queues its
 * answer and the updates it causes, td_receive() hands them out.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fake_tdjson.h"
#include "json.h"

/* What a request carries back: its "@extra", as a JSON value */
#define EXTRA 256

static struct {
	int clients;    /* made so far, the last one is the current one */
	bool started;   /* the current client got its first request */
	bool signed_in; /* the account: ready once the parameters are set */
	bool waiting_code;
	char password[64], hint[64]; /* two-step verification, "" none */

	char first[128], last[128], username[64], phone[32], bio[512];
	int day, month, year; /* the birthdate, day 0: none */
	char photo[256];   /* the profile photo's file, "" none */
	int photo_file;    /* its file id (both sizes) */
	bool photo_local;  /* downloaded (set from the phone), not until downloadFile otherwise */
	int next_file;
	char taken[64];

	struct contact {
		long long id;
		char first[128], last[128], status[256], photo[256];
		int photo_file;
	} contacts[16];
	unsigned contact_count;

	/* Chats' messages (ui_test_td_message(), sent ones), oldest first: their JSON */
	struct message { long long chat, id; char *json; } *messages;
	unsigned message_count;
	long long next_message;

	/* Instant views (getWebPageInstantView) by URL, files downloadFile gives by id */
	struct { char *url, *json; } views[8];
	unsigned view_count;
	struct { int id; char path[256]; } files[32];
	unsigned file_count;

	struct { char type[64]; int code; char message[128]; } fails[8];
	unsigned fail_count;

	/* No network: text sent stays pending until it is back (offline_sent); a refusal for the
	 * next text sent (send_refused: its error) */
	bool offline;
	struct { long long chat, id, reply_to; char *text; } offline_sent[8];
	unsigned offline_count;
	int refuse_code;
	bool refuse_retry;
	char refuse_message[128];

	bool hold;
	char **queue;
	unsigned queue_len, queue_cap, queue_head;
	char *given; /* the last td_receive(), freed by the next */

	struct { char type[64]; char *json; } *requests;
	unsigned request_count, request_cap;
} td = { .next_file = 100, .next_message = 1LL << 40 };

static void push(char *json)
{
	if (td.queue_len == td.queue_cap) {
		td.queue_cap = td.queue_cap ? td.queue_cap * 2 : 64;
		td.queue = realloc(td.queue, td.queue_cap * sizeof(*td.queue));
	}
	td.queue[td.queue_len++] = json;
}

static char *vformat(const char *fmt, va_list ap)
{
	char *s = NULL;
	if (vasprintf(&s, fmt, ap) < 0)
		abort();
	return s;
}

/* An update, as TDLib sends it */
static void update(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	push(vformat(fmt, ap));
	va_end(ap);
}

/* The answer to a request: an object, its "@extra" added */
static void answer(const char *extra, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *obj = vformat(fmt, ap);
	va_end(ap);
	size_t n = strlen(obj);
	if (!extra[0] || n < 2) {
		push(obj);
		return;
	}
	char *s = NULL;
	obj[n - 1] = '\0'; /* its closing brace */
	if (asprintf(&s, "%s,\"@extra\":%s}", obj, extra) < 0)
		abort();
	free(obj);
	push(s);
}

static void ok(const char *extra)
{
	answer(extra, "{\"@type\":\"ok\"}");
}

static void error(const char *extra, int code, const char *message)
{
	char m[256];
	json_escape(m, sizeof(m), message);
	answer(extra, "{\"@type\":\"error\",\"code\":%d,\"message\":\"%s\"}", code, m);
}

static void auth_state(const char *state)
{
	update("{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"%s\"}}", state);
}

static void ready(void)
{
	td.signed_in = true;
	td.waiting_code = false;
	auth_state("authorizationStateReady");
}

/* A file not downloaded yet */
static char *remote_file_json(int id)
{
	char *s = NULL;
	if (asprintf(&s, "{\"@type\":\"file\",\"id\":%d,\"size\":1000,\"local\":{\"@type\":\"localFile\","
		     "\"path\":\"\",\"is_downloading_completed\":false},\"remote\":{\"@type\":\"remoteFile\","
		     "\"id\":\"remote%d\",\"is_uploading_completed\":true}}", id, id) < 0)
		abort();
	return s;
}

/* A file, downloaded */
static char *file_json(int id, const char *path)
{
	char p[512], *s = NULL;
	json_escape(p, sizeof(p), path);
	if (asprintf(&s, "{\"@type\":\"file\",\"id\":%d,\"size\":1000,\"expected_size\":1000,"
		     "\"local\":{\"@type\":\"localFile\",\"path\":\"%s\",\"can_be_downloaded\":true,"
		     "\"is_downloading_active\":false,\"is_downloading_completed\":true,"
		     "\"downloaded_size\":1000},"
		     "\"remote\":{\"@type\":\"remoteFile\",\"id\":\"remote%d\",\"is_uploading_completed\":true}}",
		     id, p, id) < 0)
		abort();
	return s;
}

/* The account as getMe and updateUser give it */
static char *me_json(void)
{
	char first[256], last[256], user[128], phone[64], usernames[512] = "null", photo[2048] = "null";
	json_escape(first, sizeof(first), td.first);
	json_escape(last, sizeof(last), td.last);
	json_escape(user, sizeof(user), td.username);
	json_escape(phone, sizeof(phone), td.phone);
	if (td.username[0])
		snprintf(usernames, sizeof(usernames), "{\"@type\":\"usernames\",\"active_usernames\":[\"%s\"],"
			 "\"disabled_usernames\":[],\"editable_username\":\"%s\",\"collectible_usernames\":[]}",
			 user, user);
	if (td.photo[0]) {
		char *f = td.photo_local ? file_json(td.photo_file, td.photo) : remote_file_json(td.photo_file);
		snprintf(photo, sizeof(photo), "{\"@type\":\"profilePhoto\",\"id\":\"%d\",\"small\":%s,"
			 "\"big\":%s,\"has_animation\":false,\"is_personal\":false}", td.photo_file, f, f);
		free(f);
	}
	char *s = NULL;
	if (asprintf(&s, "{\"@type\":\"user\",\"id\":%d,\"first_name\":\"%s\",\"last_name\":\"%s\","
		     "\"usernames\":%s,\"phone_number\":\"%s\","
		     "\"status\":{\"@type\":\"userStatusOnline\",\"expires\":2000000000},"
		     "\"profile_photo\":%s,\"is_contact\":false,\"is_mutual_contact\":false,"
		     "\"is_premium\":false,\"have_access\":true,\"type\":{\"@type\":\"userTypeRegular\"}}",
		     FAKE_TD_ME, first, last, usernames, phone, photo) < 0)
		abort();
	return s;
}

/* A contact as updateUser gives it */
static char *contact_json(const struct contact *c)
{
	char first[256], last[256], photo[2048] = "null", *s = NULL;
	json_escape(first, sizeof(first), c->first);
	json_escape(last, sizeof(last), c->last);
	if (c->photo[0]) {
		/* Not downloaded yet: downloadFile gives it */
		snprintf(photo, sizeof(photo), "{\"@type\":\"profilePhoto\",\"id\":\"%d\","
			 "\"small\":{\"@type\":\"file\",\"id\":%d,\"size\":1000,\"local\":{\"@type\":\"localFile\","
			 "\"path\":\"\",\"is_downloading_completed\":false}},"
			 "\"big\":{\"@type\":\"file\",\"id\":%d,\"size\":1000,\"local\":{\"@type\":\"localFile\","
			 "\"path\":\"\",\"is_downloading_completed\":false}},\"has_animation\":false}",
			 c->photo_file, c->photo_file, c->photo_file + 1);
	}
	if (asprintf(&s, "{\"@type\":\"user\",\"id\":%lld,\"first_name\":\"%s\",\"last_name\":\"%s\","
		     "\"usernames\":null,\"phone_number\":\"\",\"status\":%s,\"profile_photo\":%s,"
		     "\"is_contact\":true,\"is_mutual_contact\":true,\"have_access\":true,"
		     "\"type\":{\"@type\":\"userTypeRegular\"}}",
		     c->id, first, last, c->status, photo) < 0)
		abort();
	return s;
}

static struct contact *contact_find(long long id)
{
	for (unsigned i = 0; i < td.contact_count; i++)
		if (td.contacts[i].id == id)
			return &td.contacts[i];
	return NULL;
}

static void user_updated(void)
{
	char *me = me_json();
	update("{\"@type\":\"updateUser\",\"user\":%s}", me);
	free(me);
}

static char *full_info_json(void)
{
	char bio[1024], birthdate[96] = "null", *s = NULL;
	json_escape(bio, sizeof(bio), td.bio);
	if (td.day)
		snprintf(birthdate, sizeof(birthdate), "{\"@type\":\"birthdate\",\"day\":%d,\"month\":%d,\"year\":%d}",
			 td.day, td.month, td.year);
	if (asprintf(&s, "{\"@type\":\"userFullInfo\",\"bio\":{\"@type\":\"formattedText\",\"text\":\"%s\","
		     "\"entities\":[]},\"birthdate\":%s,\"can_be_called\":true,\"group_in_common_count\":0}",
		     bio, birthdate) < 0)
		abort();
	return s;
}

/* Telegram's rules: 5 to 32 letters, digits and underscores, starting with a letter */
static bool username_valid(const char *u)
{
	size_t n = strlen(u);
	if (n < 5 || n > 32 || !((u[0] >= 'a' && u[0] <= 'z') || (u[0] >= 'A' && u[0] <= 'Z')))
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((u[i] >= 'a' && u[i] <= 'z') || (u[i] >= 'A' && u[i] <= 'Z') ||
		      (u[i] >= '0' && u[i] <= '9') || u[i] == '_'))
			return false;
	return true;
}

static bool failed(const char *type, const char *extra)
{
	for (unsigned i = 0; i < td.fail_count; i++) {
		if (strcmp(td.fails[i].type, type))
			continue;
		error(extra, td.fails[i].code, td.fails[i].message);
		memmove(&td.fails[i], &td.fails[i + 1], (td.fail_count - i - 1) * sizeof(td.fails[0]));
		td.fail_count--;
		return true;
	}
	return false;
}

/* Messages */

static void message_add(long long chat, long long id, const char *json)
{
	td.messages = realloc(td.messages, (td.message_count + 1) * sizeof(*td.messages));
	td.messages[td.message_count++] = (struct message){ chat, id, strdup(json) };
}

static const struct message *message_find(long long chat, long long id)
{
	for (unsigned i = 0; i < td.message_count; i++)
		if (td.messages[i].chat == chat && td.messages[i].id == id)
			return &td.messages[i];
	return NULL;
}

/* Older than from_message_id (0: the newest), newest first, as TDLib */
static void history(const struct json *r, const char *extra)
{
	long long chat = json_int(json_get(r, "chat_id"), 0), from = json_int(json_get(r, "from_message_id"), 0);
	int limit = json_int(json_get(r, "limit"), 0);
	char *list = strdup("");
	int n = 0;
	for (unsigned i = td.message_count; i-- > 0 && n < limit;) {
		const struct message *m = &td.messages[i];
		if (m->chat != chat || (from && m->id >= from))
			continue;
		char *longer = NULL;
		if (asprintf(&longer, "%s%s%s", list, n ? "," : "", m->json) < 0)
			abort();
		free(list);
		list = longer;
		n++;
	}
	answer(extra, "{\"@type\":\"messages\",\"total_count\":%d,\"messages\":[%s]}", n, list);
	free(list);
}

/* A message of the account in `chat`, `state` its sending state (JSON) */
static char *sent_json(long long chat, long long id, const char *text, long long reply_to, const char *state)
{
	char reply[160] = "null", *s = NULL;
	if (reply_to)
		snprintf(reply, sizeof(reply), "{\"@type\":\"messageReplyToMessage\",\"chat_id\":%lld,"
		         "\"message_id\":%lld}", chat, reply_to);
	if (asprintf(&s, "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%lld,\"sender_id\":"
		     "{\"@type\":\"messageSenderUser\",\"user_id\":%d},\"is_outgoing\":true,\"date\":%lld,"
		     "\"sending_state\":%s,\"reply_to\":%s,\"content\":{\"@type\":\"messageText\",\"text\":"
		     "{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}",
		     id, chat, FAKE_TD_ME, (long long)time(NULL), state, reply, text) < 0)
		abort();
	return s;
}

/* Pending message `id` on the server, with a new id */
static void sent_now(long long chat, long long id, const char *escaped, long long reply_to)
{
	char *sent = sent_json(chat, id + 1, escaped, reply_to, "null");
	update("{\"@type\":\"updateMessageSendSucceeded\",\"message\":%s,\"old_message_id\":%lld}", sent, id);
	message_add(chat, id + 1, sent);
	free(sent);
}

/* As TDLib: the message pending (the answer, updateNewMessage), then on the server
 * with a new id (updateMessageSendSucceeded); without network, once it is back;
 * refused (ui_test_td_send_refused()), failed (updateMessageSendFailed) */
static void send_core(long long chat, const char *escaped, long long reply_to, const char *extra)
{
	long long id = td.next_message;
	td.next_message += 2;
	char *pending = sent_json(chat, id, escaped, reply_to, "{\"@type\":\"messageSendingStatePending\"}");
	if (extra)
		answer(extra, "%s", pending);
	update("{\"@type\":\"updateNewMessage\",\"message\":%s}", pending);
	free(pending);
	if (td.refuse_code) {
		/* Refused by the server: failed, with another id, as TDLib does */
		char state[640], message[256];
		json_escape(message, sizeof(message), td.refuse_message);
		snprintf(state, sizeof(state), "{\"@type\":\"messageSendingStateFailed\",\"error\":{\"@type\":\"error\","
		         "\"code\":%d,\"message\":\"%s\"},\"can_retry\":%s,\"need_another_sender\":false,"
		         "\"need_another_reply_quote\":false,\"need_drop_reply\":false,"
		         "\"required_paid_message_star_count\":0,\"retry_after\":0}", td.refuse_code, message,
		         td.refuse_retry ? "true" : "false");
		char *failed = sent_json(chat, id + 1, escaped, reply_to, state);
		update("{\"@type\":\"updateMessageSendFailed\",\"message\":%s,\"old_message_id\":%lld,\"error\":"
		       "{\"@type\":\"error\",\"code\":%d,\"message\":\"%s\"}}", failed, id, td.refuse_code, message);
		message_add(chat, id + 1, failed);
		free(failed);
		td.refuse_code = 0;
		return;
	}
	if (td.offline && td.offline_count < sizeof(td.offline_sent) / sizeof(td.offline_sent[0])) {
		/* Sent once the network is back */
		td.offline_sent[td.offline_count].chat = chat;
		td.offline_sent[td.offline_count].id = id;
		td.offline_sent[td.offline_count].reply_to = reply_to;
		td.offline_sent[td.offline_count++].text = strdup(escaped);
		return;
	}
	sent_now(chat, id, escaped, reply_to);
}

static void send_text(const struct json *r, const char *extra)
{
	const struct json *to = json_get(r, "reply_to");
	long long reply_to = !strcmp(json_type(to), "inputMessageReplyToMessage") ?
		json_int(json_get(to, "message_id"), 0) : 0;
	const char *text = json_str(json_get(json_get(json_get(r, "input_message_content"), "text"), "text"), "");
	char escaped[4096];
	json_escape(escaped, sizeof(escaped), text);
	send_core(json_int(json_get(r, "chat_id"), 0), escaped, reply_to, extra);
}

/* Gone from the chat for good (updateDeleteMessages) */
static void message_delete(long long chat, long long id)
{
	for (unsigned i = 0; i < td.message_count; i++)
		if (td.messages[i].chat == chat && td.messages[i].id == id) {
			free(td.messages[i].json);
			memmove(&td.messages[i], &td.messages[i + 1], (--td.message_count - i) * sizeof(*td.messages));
			break;
		}
	update("{\"@type\":\"updateDeleteMessages\",\"chat_id\":%lld,\"message_ids\":[%lld],\"is_permanent\":true,"
	       "\"from_cache\":false}", chat, id);
}

/* resendMessages: as TDLib, each failed one deleted and sent again as a new one */
static void resend(const struct json *r, const char *extra)
{
	long long chat = json_int(json_get(r, "chat_id"), 0);
	const struct json *ids = json_get(r, "message_ids");
	for (const struct json *v = ids ? ids->child : NULL; v; v = v->next) {
		const struct message *m = message_find(chat, json_int(v, 0));
		struct json *j = m ? json_parse(m->json) : NULL;
		if (!j || strcmp(json_type(json_get(j, "sending_state")), "messageSendingStateFailed")) {
			json_free(j);
			error(extra, 400, "Message can't be resent");
			return;
		}
		char escaped[4096];
		json_escape(escaped, sizeof(escaped),
		            json_str(json_get(json_get(json_get(j, "content"), "text"), "text"), ""));
		long long reply_to = json_int(json_get(json_get(j, "reply_to"), "message_id"), 0);
		message_delete(chat, json_int(v, 0));
		json_free(j);
		send_core(chat, escaped, reply_to, NULL);
	}
	answer(extra, "{\"@type\":\"messages\",\"total_count\":0,\"messages\":[]}");
}

static void handle(const struct json *r, const char *type, const char *extra)
{
	if (failed(type, extra))
		return;

	if (!strcmp(type, "getOption")) {
		answer(extra, "{\"@type\":\"optionValueString\",\"value\":\"fake\"}");
	} else if (!strcmp(type, "setTdlibParameters")) {
		ok(extra);
		if (td.signed_in)
			ready();
		else
			auth_state("authorizationStateWaitPhoneNumber");
	} else if (!strcmp(type, "setAuthenticationPhoneNumber")) {
		snprintf(td.phone, sizeof(td.phone), "%s", json_str(json_get(r, "phone_number"), ""));
		ok(extra);
		td.waiting_code = true;
		char phone[64];
		json_escape(phone, sizeof(phone), td.phone);
		update("{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
		       "{\"@type\":\"authorizationStateWaitCode\",\"code_info\":{\"@type\":\"authenticationCodeInfo\","
		       "\"phone_number\":\"%s\",\"type\":{\"@type\":\"authenticationCodeTypeSms\",\"length\":5},"
		       "\"timeout\":60}}}", phone);
	} else if (!strcmp(type, "checkAuthenticationCode")) {
		if (strcmp(json_str(json_get(r, "code"), ""), FAKE_TD_CODE)) {
			error(extra, 400, "PHONE_CODE_INVALID");
			return;
		}
		if (!td.first[0])
			snprintf(td.first, sizeof(td.first), "New");
		ok(extra);
		if (!td.password[0]) {
			ready();
			return;
		}
		char hint[128];
		json_escape(hint, sizeof(hint), td.hint);
		update("{\"@type\":\"updateAuthorizationState\",\"authorization_state\":"
		       "{\"@type\":\"authorizationStateWaitPassword\",\"password_hint\":\"%s\","
		       "\"has_recovery_email_address\":false,\"recovery_email_address_pattern\":\"\"}}", hint);
	} else if (!strcmp(type, "checkAuthenticationPassword")) {
		if (strcmp(json_str(json_get(r, "password"), ""), td.password)) {
			error(extra, 400, "PASSWORD_HASH_INVALID");
			return;
		}
		ok(extra);
		ready();
	} else if (!strcmp(type, "getMe")) {
		char *me = me_json();
		answer(extra, "%s", me);
		free(me);
	} else if (!strcmp(type, "getUserFullInfo")) {
		char *info = full_info_json();
		answer(extra, "%s", info);
		free(info);
	} else if (!strcmp(type, "loadChats")) {
		error(extra, 404, "Not Found");
	} else if (!strcmp(type, "getContacts")) {
		/* Users are known before their ids come */
		char ids[512] = "";
		size_t n = 0;
		for (unsigned i = 0; i < td.contact_count; i++) {
			char *u = contact_json(&td.contacts[i]);
			update("{\"@type\":\"updateUser\",\"user\":%s}", u);
			free(u);
			n += snprintf(ids + n, sizeof(ids) - n, "%s%lld", i ? "," : "", td.contacts[i].id);
		}
		answer(extra, "{\"@type\":\"users\",\"total_count\":%u,\"user_ids\":[%s]}", td.contact_count, ids);
	} else if (!strcmp(type, "createPrivateChat")) {
		struct contact *c = contact_find(json_int(json_get(r, "user_id"), 0));
		if (!c) {
			error(extra, 400, "USER_ID_INVALID");
			return;
		}
		char title[600], first[256], last[256], *chat = NULL;
		json_escape(first, sizeof(first), c->first);
		json_escape(last, sizeof(last), c->last);
		snprintf(title, sizeof(title), "%s%s%s", first, last[0] ? " " : "", last);
		if (asprintf(&chat, "{\"@type\":\"chat\",\"id\":%lld,\"type\":{\"@type\":\"chatTypePrivate\","
			     "\"user_id\":%lld},\"title\":\"%s\",\"photo\":null,\"positions\":[],\"unread_count\":0,"
			     "\"last_message\":null}", c->id, c->id, title) < 0)
			abort();
		update("{\"@type\":\"updateNewChat\",\"chat\":%s}", chat);
		answer(extra, "%s", chat);
		free(chat);
	} else if (!strcmp(type, "getChatHistory")) {
		history(r, extra);
	} else if (!strcmp(type, "resendMessages")) {
		resend(r, extra);
	} else if (!strcmp(type, "deleteMessages")) {
		const struct json *ids = json_get(r, "message_ids");
		for (const struct json *v = ids ? ids->child : NULL; v; v = v->next)
			message_delete(json_int(json_get(r, "chat_id"), 0), json_int(v, 0));
		ok(extra);
	} else if (!strcmp(type, "getWebPageInstantView")) {
		const char *url = json_str(json_get(r, "url"), "");
		unsigned i = 0;
		while (i < td.view_count && strcmp(td.views[i].url, url))
			i++;
		if (i < td.view_count)
			answer(extra, "%s", td.views[i].json);
		else
			error(extra, 404, "Not Found");
	} else if (!strcmp(type, "getRepliedMessage")) {
		const struct message *m = message_find(json_int(json_get(r, "chat_id"), 0),
		                                       json_int(json_get(r, "message_id"), 0));
		struct json *j = m ? json_parse(m->json) : NULL;
		const struct json *to = json_get(j, "reply_to");
		const struct message *replied = !strcmp(json_type(to), "messageReplyToMessage") ?
			message_find(json_int(json_get(to, "chat_id"), 0), json_int(json_get(to, "message_id"), 0)) : NULL;
		json_free(j);
		if (replied)
			answer(extra, "%s", replied->json);
		else
			error(extra, 404, "Not Found");
	} else if (!strcmp(type, "sendMessage") &&
	           !strcmp(json_type(json_get(r, "input_message_content")), "inputMessageText")) {
		send_text(r, extra);
	} else if (!strcmp(type, "logOut")) {
		ok(extra);
		td.signed_in = false;
		auth_state("authorizationStateLoggingOut");
		auth_state("authorizationStateClosing");
		auth_state("authorizationStateClosed");
	} else if (!strcmp(type, "close")) {
		ok(extra);
		auth_state("authorizationStateClosing");
		auth_state("authorizationStateClosed");
	} else if (!strcmp(type, "setName")) {
		const char *first = json_str(json_get(r, "first_name"), "");
		if (!first[0] || strlen(first) > 64) {
			error(extra, 400, "FIRSTNAME_INVALID");
			return;
		}
		snprintf(td.first, sizeof(td.first), "%s", first);
		snprintf(td.last, sizeof(td.last), "%s", json_str(json_get(r, "last_name"), ""));
		user_updated();
		ok(extra);
	} else if (!strcmp(type, "setUsername")) {
		const char *u = json_str(json_get(r, "username"), "");
		if (u[0] && !username_valid(u)) {
			error(extra, 400, "USERNAME_INVALID");
			return;
		}
		if (u[0] && !strcasecmp(u, td.taken)) {
			error(extra, 400, "USERNAME_OCCUPIED");
			return;
		}
		snprintf(td.username, sizeof(td.username), "%s", u);
		user_updated();
		ok(extra);
	} else if (!strcmp(type, "setBio")) {
		const char *bio = json_str(json_get(r, "bio"), "");
		if (strlen(bio) > 70 || strchr(bio, '\n')) {
			error(extra, 400, "ABOUT_TOO_LONG");
			return;
		}
		snprintf(td.bio, sizeof(td.bio), "%s", bio);
		char *info = full_info_json();
		update("{\"@type\":\"updateUserFullInfo\",\"user_id\":%d,\"user_full_info\":%s}", FAKE_TD_ME, info);
		free(info);
		ok(extra);
	} else if (!strcmp(type, "setBirthdate")) {
		/* null removes it; a day that month does not have is refused */
		static const int days[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		const struct json *b = json_get(r, "birthdate");
		int day = json_int(json_get(b, "day"), 0), month = json_int(json_get(b, "month"), 0);
		int year = json_int(json_get(b, "year"), 0);
		if (b && b->kind == JSON_OBJECT &&
		    (month < 1 || month > 12 || day < 1 || day > days[month - 1] || (year && (year < 1900 || year > 2100)))) {
			error(extra, 400, "BIRTHDAY_INVALID");
			return;
		}
		td.day = b && b->kind == JSON_OBJECT ? day : 0;
		td.month = month;
		td.year = year;
		char *info = full_info_json();
		update("{\"@type\":\"updateUserFullInfo\",\"user_id\":%d,\"user_full_info\":%s}", FAKE_TD_ME, info);
		free(info);
		ok(extra);
	} else if (!strcmp(type, "setProfilePhoto")) {
		const struct json *photo = json_get(r, "photo");
		const char *path = json_str(json_get(json_get(photo, "photo"), "path"), "");
		if (strcmp(json_type(photo), "inputChatPhotoStatic") || !path[0]) {
			error(extra, 400, "PHOTO_INVALID");
			return;
		}
		snprintf(td.photo, sizeof(td.photo), "%s", path);
		td.photo_file = td.next_file++;
		td.photo_local = true;
		ok(extra);
		user_updated();
	} else if (!strcmp(type, "downloadFile")) {
		int id = json_int(json_get(r, "file_id"), 0);
		const char *path = td.photo[0] && id == td.photo_file ? td.photo : NULL;
		for (unsigned i = 0; !path && i < td.contact_count; i++)
			if (td.contacts[i].photo[0] && (id == td.contacts[i].photo_file || id == td.contacts[i].photo_file + 1))
				path = td.contacts[i].photo;
		for (unsigned i = 0; !path && i < td.file_count; i++)
			if (td.files[i].id == id)
				path = td.files[i].path;
		if (!path) {
			error(extra, 404, "Not Found");
			return;
		}
		/* As TDLib: the answer, then the update once it is downloaded */
		char *f = file_json(id, path);
		answer(extra, "%s", f);
		update("{\"@type\":\"updateFile\",\"file\":%s}", f);
		free(f);
	} else {
		if (strcmp(type, "setLogVerbosityLevel"))
			printf("fake TDLib: %s answered ok\n", type);
		ok(extra);
	}
}

static void log_request(const char *type, const char *json)
{
	if (td.request_count == td.request_cap) {
		td.request_cap = td.request_cap ? td.request_cap * 2 : 64;
		td.requests = realloc(td.requests, td.request_cap * sizeof(*td.requests));
	}
	snprintf(td.requests[td.request_count].type, sizeof(td.requests[0].type), "%s", type);
	td.requests[td.request_count++].json = strdup(json);
}

/* Requests held: answered when released */
static char **held;
static unsigned held_count;

static void process(const char *request)
{
	struct json *r = json_parse(request);
	if (!r) {
		printf("fake TDLib: unreadable request %.80s\n", request);
		return;
	}
	const char *type = json_type(r);
	log_request(type, request);
	/* The "@extra" as it came, a string or a number */
	char extra[EXTRA] = "";
	const struct json *e = json_get(r, "@extra");
	if (e) {
		char escaped[EXTRA - 2];
		json_escape(escaped, sizeof(escaped), json_str(e, ""));
		snprintf(extra, sizeof(extra), "\"%s\"", escaped);
	}
	/* The first request starts the client */
	if (!td.started) {
		td.started = true;
		auth_state("authorizationStateWaitTdlibParameters");
	}
	handle(r, type, extra);
	json_free(r);
}

int fake_td_create_client_id(void)
{
	td.started = false;
	return ++td.clients;
}

void fake_td_send(int client_id, const char *request)
{
	if (client_id != td.clients) {
		printf("fake TDLib: request to client %d, not the current one\n", client_id);
		return;
	}
	if (td.hold) {
		held = realloc(held, (held_count + 1) * sizeof(*held));
		held[held_count++] = strdup(request);
		return;
	}
	process(request);
}

const char *fake_td_receive(double timeout)
{
	(void)timeout;
	free(td.given);
	td.given = NULL;
	if (td.queue_head == td.queue_len)
		return NULL;
	td.given = td.queue[td.queue_head++];
	if (td.queue_head == td.queue_len)
		td.queue_head = td.queue_len = 0;
	return td.given;
}

const char *fake_td_execute(const char *request)
{
	(void)request;
	return "{\"@type\":\"ok\"}";
}

/* The tests' side */

void ui_test_td_account(const char *first, const char *last, const char *username, const char *phone)
{
	td.signed_in = true;
	snprintf(td.first, sizeof(td.first), "%s", first);
	snprintf(td.last, sizeof(td.last), "%s", last);
	snprintf(td.username, sizeof(td.username), "%s", username);
	snprintf(td.phone, sizeof(td.phone), "%s", phone);
}

void ui_test_td_password(const char *password, const char *hint)
{
	snprintf(td.password, sizeof(td.password), "%s", password);
	snprintf(td.hint, sizeof(td.hint), "%s", hint);
}

void ui_test_td_bio(const char *bio)
{
	snprintf(td.bio, sizeof(td.bio), "%s", bio);
}

void ui_test_td_birthdate(int day, int month, int year)
{
	td.day = day;
	td.month = month;
	td.year = year;
}

void ui_test_td_photo(const char *path)
{
	snprintf(td.photo, sizeof(td.photo), "%s", path);
	td.photo_file = td.next_file++;
}

void ui_test_td_contact(long long id, const char *first, const char *last, const char *status, const char *photo)
{
	if (td.contact_count == sizeof(td.contacts) / sizeof(td.contacts[0]))
		return;
	struct contact *c = &td.contacts[td.contact_count++];
	c->id = id;
	snprintf(c->first, sizeof(c->first), "%s", first);
	snprintf(c->last, sizeof(c->last), "%s", last);
	snprintf(c->status, sizeof(c->status), "%s",
		 status ? status : "{\"@type\":\"userStatusRecently\",\"by_my_privacy_settings\":false}");
	snprintf(c->photo, sizeof(c->photo), "%s", photo ? photo : "");
	if (photo) {
		c->photo_file = td.next_file;
		td.next_file += 2; /* small and big */
	}
}

void ui_test_td_status(long long id, const char *status)
{
	struct contact *c = contact_find(id);
	if (c)
		snprintf(c->status, sizeof(c->status), "%s", status);
	update("{\"@type\":\"updateUserStatus\",\"user_id\":%lld,\"status\":%s}", id, status);
}

void ui_test_td_username_taken(const char *username)
{
	snprintf(td.taken, sizeof(td.taken), "%s", username);
}

void ui_test_td_fail(const char *type, int code, const char *message)
{
	if (td.fail_count == sizeof(td.fails) / sizeof(td.fails[0]))
		return;
	snprintf(td.fails[td.fail_count].type, sizeof(td.fails[0].type), "%s", type);
	td.fails[td.fail_count].code = code;
	snprintf(td.fails[td.fail_count].message, sizeof(td.fails[0].message), "%s", message);
	td.fail_count++;
}

void ui_test_td_hold(bool hold)
{
	td.hold = hold;
	if (hold)
		return;
	for (unsigned i = 0; i < held_count; i++) {
		process(held[i]);
		free(held[i]);
	}
	free(held);
	held = NULL;
	held_count = 0;
}

void ui_test_td_update(const char *json)
{
	push(strdup(json));
}

const char *ui_test_td_requested(const char *type)
{
	for (unsigned i = td.request_count; i-- > 0;)
		if (!strcmp(td.requests[i].type, type))
			return td.requests[i].json;
	return NULL;
}

unsigned ui_test_td_request_count(const char *type)
{
	unsigned n = 0;
	for (unsigned i = 0; i < td.request_count; i++)
		n += !strcmp(td.requests[i].type, type);
	return n;
}

void ui_test_td_message(const char *json)
{
	struct json *j = json_parse(json);
	if (!j) {
		printf("fake TDLib: unreadable message %.80s\n", json);
		return;
	}
	message_add(json_int(json_get(j, "chat_id"), 0), json_int(json_get(j, "id"), 0), json);
	json_free(j);
}

void ui_test_td_instant_view(const char *url, const char *json)
{
	if (td.view_count == sizeof(td.views) / sizeof(td.views[0]))
		return;
	td.views[td.view_count].url = strdup(url);
	td.views[td.view_count++].json = strdup(json);
}

void ui_test_td_file(int id, const char *path)
{
	if (td.file_count == sizeof(td.files) / sizeof(td.files[0]))
		return;
	td.files[td.file_count].id = id;
	snprintf(td.files[td.file_count++].path, sizeof(td.files[0].path), "%s", path);
}

void ui_test_td_offline(bool offline)
{
	td.offline = offline;
	update("{\"@type\":\"updateConnectionState\",\"state\":{\"@type\":\"%s\"}}",
	       offline ? "connectionStateWaitingForNetwork" : "connectionStateReady");
	if (offline)
		return;
	for (unsigned i = 0; i < td.offline_count; i++) {
		sent_now(td.offline_sent[i].chat, td.offline_sent[i].id, td.offline_sent[i].text,
		         td.offline_sent[i].reply_to);
		free(td.offline_sent[i].text);
	}
	td.offline_count = 0;
}

void ui_test_td_send_refused(int code, const char *message, bool can_retry)
{
	td.refuse_code = code;
	td.refuse_retry = can_retry;
	snprintf(td.refuse_message, sizeof(td.refuse_message), "%s", message);
}
