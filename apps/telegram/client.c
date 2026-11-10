/*
 * Telegram's TDLib client: libtdjson, loaded when the app is first opened (or with
 * phone-ui once signed in, for its notifications), and a timer polling it, which hands
 * updates and answers to the other files: often while one of the app's screens is shown,
 * every second otherwise. The client lives on after the screen is closed.
 *
 * TDLib keeps its database in /var/lib/telegram. The API credentials (from
 * my.telegram.org), typed in the login (login.c), are kept in /var/lib/telegram/api:
 * "<api_id> <api_hash>".
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "telegram.h"

#define POLL_MS 100       /* the app on screen */
#define POLL_AWAY_MS 1000 /* otherwise: its notifications */

struct telegram tg;

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
void td_request(const char *json)
{
	tg.send(tg.client, json);
}

void client_start(void)
{
	chats_clear();
	contact_list_clear();
	profile_clear();
	notify_clear();
	tg.client = tg.create_client_id();
	tg.closed = false;
	tg.state = ST_STARTING;
	/* The client starts with its first request */
	td_request("{\"@type\":\"getOption\",\"name\":\"version\"}");
}

void send_parameters(void)
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
	/* Its notifications, as many groups as it keeps (muted chats left out) */
	td_request("{\"@type\":\"setOption\",\"name\":\"notification_group_count_max\","
		   "\"value\":{\"@type\":\"optionValueInteger\",\"value\":25}}");
}

/* A file TDLib finished downloading: an avatar, a photo of the open chat, the
 * profile's photo */
static void file_done(const struct json *file)
{
	avatar_file(file);
	photo_file(file);
	instant_file(file);
	profile_file(file);
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
		login_auth_state(u);
	} else if (!strcmp(type, "updateConnectionState")) {
		telegram_connection(json_get(u, "state"));
	} else if (chat_update(type, u)) {
		/* shown after this batch of updates */
	} else if (notify_update(type, u)) {
		/* posted */
	} else if (!strcmp(type, "updateFile")) {
		file_done(json_get(u, "file"));
	} else if (!strcmp(type, "updateUser")) {
		const struct json *user = json_get(u, "user");
		user_update(user);
		if (tg.me && json_int(json_get(user, "id"), 0) == tg.me)
			profile_me(user);
	} else if (!strcmp(type, "updateUserStatus")) {
		user_status(json_int(json_get(u, "user_id"), 0), json_get(u, "status"));
	} else if (!strcmp(type, "updateUserFullInfo")) {
		if (tg.me && json_int(json_get(u, "user_id"), 0) == tg.me)
			profile_full_info(json_get(u, "user_full_info"));
	} else if (!strncmp(type, "update", 6)) {
		chat_update_messages(type, u);
	} else if (!strncmp(extra, "history ", 8)) {
		chat_history_answer(u);
	} else if (!strncmp(extra, "replied ", 8)) {
		chat_replied_answer(u);
	} else if (!strncmp(extra, "instant view ", 13)) {
		instant_answer(u);
	} else if (!strcmp(extra, "chats")) {
		chats_loaded(LIST_MAIN, u);
	} else if (!strcmp(extra, "archive")) {
		chats_loaded(LIST_ARCHIVE, u);
	} else if (!strcmp(extra, "contacts")) {
		contact_list_loaded(u);
	} else if (!strcmp(extra, "contact chat")) { /* NON-NLS */
		contact_chat(u);
	} else if (!strcmp(extra, "download")) {
		/* Already downloaded: the answer says so, no updateFile follows */
		if (!strcmp(type, "file"))
			file_done(u);
		else
			printf("telegram: downloadFile: %s\n", json_str(json_get(u, "message"), ""));
	} else if (!strcmp(type, "error") && !strcmp(extra, "send")) {
		chat_send_error(u);
	} else if (!strcmp(type, "error") && !strcmp(extra, "auth")) {
		login_error(u);
	} else if (!strcmp(type, "user") && !strcmp(extra, "me")) {
		login_me(u);
	} else if (!strcmp(type, "userFullInfo") && !strcmp(extra, "full info")) {
		profile_full_info(u);
	} else if (!strcmp(extra, "profile")) {
		profile_saved(u);
	} else if (!strcmp(type, "error") && !strcmp(extra, "profile photo")) {
		printf("telegram: setProfilePhoto: %s\n", json_str(json_get(u, "message"), ""));
	}
	json_free(u);
}

static void poll_cb(lv_timer_t *timer)
{
	lv_timer_set_period(timer, telegram_in_front() ? POLL_MS : POLL_AWAY_MS);
	/* A few at a time: the UI runs in the same loop */
	for (int i = 0; i < 64; i++) {
		const char *json = tg.receive(0);
		if (!json)
			break;
		handle(json);
	}
	avatar_decode_next();
	list_poll();
	contact_list_poll();
	chat_poll();
	instant_poll();
}

/* Starts TDLib once; the state says what is missing otherwise */
void telegram_start(void)
{
	if (tg.poll) {
		lv_timer_set_period(tg.poll, POLL_MS); /* opened: not a second away */
		return;
	}
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

void telegram_start_signed_in(void)
{
	struct stat st;
	if (stat(DIR "/td.binlog", &st) == 0)
		telegram_start();
}
