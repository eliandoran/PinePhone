/*
 * TDLib's JSON interface for test_telegram.c: see fake_tdjson.h. One client at a time
 * (a new one after logging out), everything on the caller's thread: a request queues its
 * answer and the updates it causes, td_receive() hands them out.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_tdjson.h"
#include "json.h"

/* What a request carries back: its "@extra", as a JSON value */
#define EXTRA 256

static struct {
	int clients;    /* made so far, the last one is the current one */
	bool started;   /* the current client got its first request */
	bool signed_in; /* the account: ready once the parameters are set */
	bool waiting_code;

	char first[128], last[128], username[64], phone[32], bio[512];
	int day, month, year; /* the birthdate, day 0: none */
	char photo[256];   /* the profile photo's file, "" none */
	int photo_file;    /* its file id (both sizes) */
	int next_file;
	char taken[64];

	struct { char type[64]; int code; char message[128]; } fails[8];
	unsigned fail_count;

	bool hold;
	char **queue;
	unsigned queue_len, queue_cap, queue_head;
	char *given; /* the last td_receive(), freed by the next */

	struct { char type[64]; char *json; } *requests;
	unsigned request_count, request_cap;
} td = { .next_file = 100 };

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
	char first[256], last[256], user[128], phone[64], usernames[256] = "null", photo[2048] = "null";
	json_escape(first, sizeof(first), td.first);
	json_escape(last, sizeof(last), td.last);
	json_escape(user, sizeof(user), td.username);
	json_escape(phone, sizeof(phone), td.phone);
	if (td.username[0])
		snprintf(usernames, sizeof(usernames), "{\"@type\":\"usernames\",\"active_usernames\":[\"%s\"],"
			 "\"disabled_usernames\":[],\"editable_username\":\"%s\",\"collectible_usernames\":[]}",
			 user, user);
	if (td.photo[0]) {
		char *f = file_json(td.photo_file, td.photo);
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
	} else if (!strcmp(type, "logOut")) {
		ok(extra);
		td.signed_in = false;
		auth_state("authorizationStateLoggingOut");
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
		ok(extra);
		user_updated();
	} else if (!strcmp(type, "downloadFile")) {
		int id = json_int(json_get(r, "file_id"), 0);
		if (!td.photo[0] || id != td.photo_file) {
			error(extra, 404, "Not Found");
			return;
		}
		char *f = file_json(id, td.photo);
		answer(extra, "%s", f);
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
