
/*
 * Assistant: conversations with a language model over its chat completions API
 * (OpenAI's format, which DeepSeek's follows), answers streamed into the bubble
 * as they come. Each conversation is a JSON file in /var/lib/assistant (its
 * messages with their times); the API keys, the model, thinking and web search
 * are in the settings (assistant.<provider>.key, assistant.model,
 * assistant.thinking, assistant.search), set on the settings screen.
 *
 * Web search is a function the model can call: the app runs each search with
 * the provider's search API, shows its query, and asks the model again with the
 * results, for up to MAX_ROUNDS answers with searches per question.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "ui.h"

#define DATA_DIR "/var/lib/assistant"
#define ROW_H 120
#define MAX_LENGTH 2000      /* characters typed in a message */
#define MAX_FILE (1 << 20)   /* bytes read from a conversation */
#define PREVIEW_MAX 120
#define MODEL_DEFAULT "deepseek-flash"
/* The answers are shown as plain text with the fonts' characters */
#define SYSTEM_PROMPT "You are a helpful assistant on a small phone screen. Answer briefly, " \
	"in plain text: no Markdown (no **, #, tables or code fences), no emoji, no LaTeX."
/* Web search, a function the model can call (when on in the settings): the app
 * runs it with the provider's search API and sends the results back */
#define SEARCH_TOOL "{\"type\":\"function\",\"function\":{\"name\":\"web_search\"," \
	"\"description\":\"Searches the web. Use it for recent events, current facts and " \
	"anything you are not sure of.\",\"parameters\":{\"type\":\"object\",\"properties\":" \
	"{\"query\":{\"type\":\"string\",\"description\":\"The search query, at most 128 " \
	"characters\"}},\"required\":[\"query\"]}}}"
#define SEARCH_RESULTS 5
#define MAX_CALLS 4  /* searches in one answer */
#define MAX_ROUNDS 3 /* answers with searches for one question */

struct model {
	const char *id, *info;
};

/* Providers with an OpenAI-style chat completions API */
static const struct provider {
	const char *name;
	const char *url;
	const char *key_setting;
	const char *key_hint;
	struct model models[4]; /* the first is the default */
	bool thinking; /* takes "thinking": {"type": "enabled"} or "disabled" */
	/* Web search: POST {"query", "count"}, answers {"results": [{"title",
	 * "url", "summary", "date"}]}; NULL: none */
	const char *search_url;
} providers[] = {
	{ "DeepSeek", "https://api.deepseek.com/chat/completions", "assistant.deepseek.key",
	  "Create one on platform.deepseek.com, API keys.",
	  { { "deepseek-flash", "Fast, cheap" },
	    { "deepseek-v4-pro", "Smarter, slower" } },
	  true, "https://api.deepseek.com/v1/tools/search" },
};
#define PROVIDER_COUNT (sizeof(providers) / sizeof(providers[0]))

/* Bumped when a conversation is saved or deleted: the list reloads */
static unsigned assistant_version;

static const struct model *model_find(const char *id, const struct provider **provider)
{
	for (unsigned p = 0; p < PROVIDER_COUNT; p++)
		for (const struct model *m = providers[p].models; m->id; m++)
			if (!strcmp(m->id, id)) {
				if (provider)
					*provider = &providers[p];
				return m;
			}
	return NULL;
}

/* The model chosen in the settings, and its provider */
static const struct model *model_current(const struct provider **provider)
{
	const struct model *m = model_find(ui_setting_get("assistant.model", MODEL_DEFAULT), provider);
	return m ? m : model_find(MODEL_DEFAULT, provider);
}

static const char *provider_key(const struct provider *p)
{
	return ui_setting_get(p->key_setting, "");
}

/* Files */

static char *file_read(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *text = malloc(MAX_FILE + 1);
	size_t n = text ? fread(text, 1, MAX_FILE, f) : 0;
	fclose(f);
	if (text)
		text[n] = '\0';
	return text;
}

static void put_string(FILE *f, const char *text)
{
	size_t size = strlen(text) * 6 + 1;
	char *escaped = malloc(size);
	if (!escaped)
		return;
	json_escape(escaped, size, text);
	fprintf(f, "\"%s\"", escaped);
	free(escaped);
}

/* A message of a conversation. Steps are the model's web searches: its call
 * (not shown) and each result (shown as the query), kept as the messages the
 * API takes, to be sent again with the rest. */
enum role { USER, ASSISTANT, STEP };
static const char *const role_names[] = { "user", "assistant", "step" };

struct turn {
	enum role role;
	time_t time;
	char *text;    /* shown, "" for none */
	char *message; /* a step: its message for the API (JSON), NULL otherwise */
};

/* Through a temporary file, synced: the battery can cut power at any time */
static void conversation_write(const char *path, const struct turn *turns, unsigned count)
{
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	mkdir(DATA_DIR, 0755);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		perror("phone-ui: assistant");
		return;
	}
	fputs("{\"messages\":[", f);
	for (unsigned i = 0; i < count; i++) {
		fprintf(f, "%s\n{\"role\":\"%s\",\"time\":%lld,\"content\":", i ? "," : "",
		        role_names[turns[i].role], (long long)turns[i].time);
		put_string(f, turns[i].text);
		if (turns[i].message) {
			fputs(",\"message\":", f);
			put_string(f, turns[i].message);
		}
		fputc('}', f);
	}
	fputs("]}\n", f);
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		perror("phone-ui: assistant");
		unlink(tmp);
	}
	assistant_version++;
}

/* Conversation */

/* A web search the model asked for, as it streams in */
struct call {
	char *id, *args;
	size_t id_len, args_len;
};

static struct {
	lv_obj_t *scr, *confirm;
	lv_obj_t *status; /* an error or a hint under the messages */
	struct ui_chat chat;
	char path[PATH_MAX]; /* "": not saved yet */
	struct turn *turns;
	unsigned count, cap;
	/* The answer on its way */
	struct ui_fetch fetch;
	size_t parsed;    /* bytes of the stream read */
	char *reply;      /* what came so far */
	size_t reply_len;
	char *reasoning;  /* thinking, sent back with search calls */
	size_t reasoning_len;
	lv_obj_t *reply_group, *reply_label, *reply_time;
	/* Web searches: the calls of the last answer, run one at a time */
	struct call calls[MAX_CALLS];
	unsigned call_count, searching;
	unsigned steps_start; /* the first turn of these searches */
	struct ui_fetch search;
	lv_obj_t *step_label;
	unsigned rounds;      /* answers with searches since the question */
} cv;

static bool busy(void)
{
	return cv.fetch.curl || cv.search.curl;
}

static void append(char **s, size_t *len, const char *add)
{
	size_t n = strlen(add);
	char *p = n ? realloc(*s, *len + n + 1) : NULL;
	if (!p)
		return;
	memcpy(p + *len, add, n + 1);
	*s = p;
	*len += n;
}

static bool turn_add(enum role role, time_t when, const char *text, const char *message)
{
	if (cv.count == cv.cap) {
		unsigned cap = cv.cap ? cv.cap * 2 : 16;
		struct turn *turns = realloc(cv.turns, cap * sizeof(*turns));
		if (!turns)
			return false;
		cv.turns = turns;
		cv.cap = cap;
	}
	char *copy = strdup(text);
	char *msg = message ? strdup(message) : NULL;
	if (!copy || (message && !msg)) {
		free(copy);
		free(msg);
		return false;
	}
	cv.turns[cv.count++] = (struct turn){ role, when, copy, msg };
	return true;
}

static void turns_drop(unsigned from)
{
	while (cv.count > from) {
		cv.count--;
		free(cv.turns[cv.count].text);
		free(cv.turns[cv.count].message);
	}
}

static void conversation_save(void)
{
	if (!cv.count)
		return;
	if (!cv.path[0])
		snprintf(cv.path, sizeof(cv.path), DATA_DIR "/%lld.json", (long long)cv.turns[0].time);
	conversation_write(cv.path, cv.turns, cv.count);
}

static void conversation_load(const char *path)
{
	char *text = file_read(path);
	struct json *root = json_parse(text);
	free(text);
	const struct json *messages = json_get(root, "messages");
	for (const struct json *m = messages ? messages->child : NULL; m; m = m->next) {
		const char *role = json_str(json_get(m, "role"), "");
		const char *message = json_str(json_get(m, "message"), NULL);
		if (!strcmp(role, "step") && !message)
			continue;
		turn_add(!strcmp(role, "user") ? USER : !strcmp(role, "step") ? STEP : ASSISTANT,
		         json_int(json_get(m, "time"), 0), json_str(json_get(m, "content"), ""),
		         !strcmp(role, "step") ? message : NULL);
	}
	json_free(root);
	snprintf(cv.path, sizeof(cv.path), "%s", path);
}

/* View */

static bool view_at_end(void)
{
	return lv_obj_get_scroll_bottom(cv.chat.list) < 200 || lv_anim_get(cv.chat.list, NULL);
}

static void view_set_text(lv_obj_t *label, const char *text)
{
	char *composed = ui_emoji_compose(text);
	lv_label_set_text(label, composed ? composed : text);
	free(composed);
}

/* A message as a group of one bubble with its time; answers take the width */
static lv_obj_t *view_add(bool user, const char *text, time_t when, lv_obj_t **time_label)
{
	lv_obj_t *time;
	lv_obj_t *group = ui_chat_group_create(&cv.chat, user, &time);
	lv_obj_t *label = ui_chat_bubble_create(group, user);
	view_set_text(label, text);
	if (!user)
		lv_obj_set_style_max_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) - 32 - 48, 0);
	if (when) {
		char buf[8];
		struct tm tm;
		localtime_r(&when, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(time, buf);
	}
	if (time_label)
		*time_label = time;
	return label;
}

/* Dim and centered between the messages */
static lv_obj_t *view_note(const char *text, uint32_t color)
{
	lv_obj_t *label = lv_label_create(cv.chat.list);
	view_set_text(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_set_style_pad_ver(label, 12, 0);
	return label;
}

static void view_turn(const struct turn *t)
{
	if (t->role == STEP) {
		if (t->text[0])
			view_note(t->text, UI_COLOR_DIM);
	} else {
		view_add(t->role == USER, t->text, t->time, NULL);
	}
}

/* Under the messages: tapping it calls cb (try again) */
static void view_status(const char *text, uint32_t color, lv_event_cb_t cb)
{
	if (cv.status)
		lv_obj_delete(cv.status);
	cv.status = view_note(text, color);
	if (cb) {
		lv_obj_set_clickable(cv.status, true);
		lv_obj_add_event_cb(cv.status, cb, LV_EVENT_CLICKED, NULL);
	}
}

static void view_status_clear(void)
{
	if (cv.status)
		lv_obj_delete(cv.status);
	cv.status = NULL;
}

/* Asking */

static void ask(void);

static void retry_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ask();
}

static bool search_on(const struct provider *p)
{
	return p->search_url && ui_setting_get_int("assistant.search", 0);
}

/* Server-sent events: "data: {chunk}" lines, then "data: [DONE]". `all`: the
 * last line even without its newline (the end of the stream) */
static void stream_read(const char *data, size_t size, bool all)
{
	bool changed = false;
	while (cv.parsed < size) {
		const char *line = data + cv.parsed;
		const char *nl = memchr(line, '\n', size - cv.parsed);
		if (!nl && !all)
			break;
		size_t len = nl ? (size_t)(nl - line) : size - cv.parsed;
		cv.parsed += len + (nl != NULL);
		if (len < 6 || strncmp(line, "data: ", 6) != 0)
			continue; /* blank lines, ": keep-alive" */
		char *payload = strndup(line + 6, len - 6);
		struct json *chunk = json_parse(payload);
		free(payload);
		const struct json *choices = json_get(chunk, "choices");
		const struct json *delta = json_get(choices ? choices->child : NULL, "delta");

		size_t before = cv.reply_len;
		append(&cv.reply, &cv.reply_len, json_str(json_get(delta, "content"), ""));
		changed |= cv.reply_len != before;
		const char *reasoning = json_str(json_get(delta, "reasoning_content"), NULL);
		if (reasoning) {
			append(&cv.reasoning, &cv.reasoning_len, reasoning);
			if (!cv.reply_len)
				lv_label_set_text_static(cv.reply_time, "Thinking...");
		}
		/* Calls come in pieces: the id and name first, then the arguments */
		const struct json *calls = json_get(delta, "tool_calls");
		for (const struct json *c = calls ? calls->child : NULL; c; c = c->next) {
			long long i = json_int(json_get(c, "index"), 0);
			if (i < 0 || i >= MAX_CALLS)
				continue;
			struct call *call = &cv.calls[i];
			if (cv.call_count <= i)
				cv.call_count = i + 1;
			append(&call->id, &call->id_len, json_str(json_get(c, "id"), ""));
			append(&call->args, &call->args_len,
			       json_str(json_get(json_get(c, "function"), "arguments"), ""));
			lv_label_set_text_static(cv.reply_time, "Searching the web...");
		}
		json_free(chunk);
	}
	if (!changed)
		return;
	bool end = view_at_end();
	/* Leading new lines (after the reasoning) are not shown */
	const char *text = cv.reply;
	while (*text == '\n')
		text++;
	view_set_text(cv.reply_label, text);
	lv_label_set_text_static(cv.reply_time, "Writing...");
	if (end)
		ui_chat_scroll_end(&cv.chat, LV_ANIM_OFF);
}

static void stream_progress(const char *data, size_t size)
{
	stream_read(data, size, false);
}

static void reply_clear(void)
{
	free(cv.reply);
	free(cv.reasoning);
	cv.reply = cv.reasoning = NULL;
	cv.reply_len = cv.reasoning_len = 0;
	for (unsigned i = 0; i < MAX_CALLS; i++) {
		free(cv.calls[i].id);
		free(cv.calls[i].args);
	}
	memset(cv.calls, 0, sizeof(cv.calls));
	cv.call_count = cv.searching = 0;
}

/* The answer as a message, saved */
static void reply_keep(void)
{
	const char *text = cv.reply ? cv.reply : "";
	while (*text == '\n')
		text++;
	if (*text) {
		turn_add(ASSISTANT, time(NULL), text, NULL);
		conversation_save();
	}
	reply_clear();
}

/* The query the model asked for, "" if none */
static char *call_query(const struct call *call)
{
	struct json *args = json_parse(call->args);
	char *query = strdup(json_str(json_get(args, "query"), ""));
	json_free(args);
	return query;
}

static void search_next(void);

/* The results for the model: title, date, address and summary of each */
static void search_done(const char *data, long status)
{
	const struct call *call = &cv.calls[cv.searching];
	char *result = NULL;
	size_t result_size = 0;
	FILE *r = open_memstream(&result, &result_size);
	if (!r)
		return;
	struct json *root = status == 200 ? json_parse(data) : NULL;
	const struct json *results = json_get(root, "results");
	unsigned n = 0;
	for (const struct json *it = results ? results->child : NULL; it; it = it->next, n++) {
		const char *date = json_str(json_get(it, "date"), NULL);
		fprintf(r, "%u. %s%s%.10s%s\n%s\n%s\n\n", n + 1, json_str(json_get(it, "title"), ""),
		        date ? " (" : "", date ? date : "", date ? ")" : "",
		        json_str(json_get(it, "url"), ""), json_str(json_get(it, "summary"), ""));
	}
	if (!status)
		fputs("The search failed: no internet connection.", r);
	else if (status != 200)
		fprintf(r, "The search failed (HTTP %ld).", status);
	else if (!n)
		fputs("No results.", r);
	json_free(root);
	fclose(r);

	char *message = NULL;
	size_t message_size = 0;
	FILE *f = open_memstream(&message, &message_size);
	if (f) {
		fputs("{\"role\":\"tool\",\"tool_call_id\":", f);
		put_string(f, call->id ? call->id : "");
		fputs(",\"content\":", f);
		put_string(f, result ? result : "");
		fputc('}', f);
		fclose(f);
	}
	free(result);

	char *query = call_query(call);
	char shown[192];
	snprintf(shown, sizeof(shown), status == 200 ? LV_SYMBOL_OK " Searched the web: %s" :
	         LV_SYMBOL_WARNING " Web search failed: %s", query ? query : "");
	free(query);
	view_set_text(cv.step_label, shown);
	turn_add(STEP, time(NULL), shown, message ? message : "{}");
	free(message);

	cv.searching++;
	search_next();
}

/* The calls one at a time, then the model again with their results */
static void search_next(void)
{
	if (cv.searching == cv.call_count) {
		reply_clear();
		conversation_save();
		ask();
		return;
	}
	const struct provider *p;
	model_current(&p);
	char *query = call_query(&cv.calls[cv.searching]);
	char text[192];
	snprintf(text, sizeof(text), "Searching the web: %s", query ? query : "");
	cv.step_label = view_note(text, UI_COLOR_DIM);
	ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
	if (!query || !query[0] || !p->search_url) {
		free(query);
		search_done(NULL, 400);
		return;
	}

	char *body = NULL;
	size_t body_size = 0;
	FILE *f = open_memstream(&body, &body_size);
	if (f) {
		fputs("{\"query\":", f);
		put_string(f, query);
		fprintf(f, ",\"count\":%d}", SEARCH_RESULTS);
		fclose(f);
	}
	free(query);
	char auth[256];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", provider_key(p));
	const char *headers[] = { "Content-Type: application/json", auth, NULL };
	ui_fetch_post(&cv.search, p->search_url, headers, body ? body : "{}", NULL, search_done);
	free(body);
}

/* The model asked for searches: its message goes in the conversation as a step
 * (with its thinking, which the API wants back), then the searches */
static void calls_run(void)
{
	char *message = NULL;
	size_t message_size = 0;
	FILE *f = open_memstream(&message, &message_size);
	if (!f)
		return;
	fputs("{\"role\":\"assistant\",\"content\":", f);
	put_string(f, cv.reply ? cv.reply : "");
	if (cv.reasoning) {
		fputs(",\"reasoning_content\":", f);
		put_string(f, cv.reasoning);
	}
	fputs(",\"tool_calls\":[", f);
	for (unsigned i = 0; i < cv.call_count; i++) {
		fprintf(f, "%s{\"id\":", i ? "," : "");
		put_string(f, cv.calls[i].id ? cv.calls[i].id : "");
		fputs(",\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"arguments\":", f);
		put_string(f, cv.calls[i].args ? cv.calls[i].args : "{}");
		fputs("}}", f);
	}
	fputs("]}", f);
	fclose(f);

	lv_obj_delete(cv.reply_group);
	cv.reply_group = NULL;
	cv.steps_start = cv.count;
	cv.rounds++;
	turn_add(STEP, time(NULL), "", message);
	free(message);
	cv.searching = 0;
	search_next();
}

static void stream_done(const char *data, long status)
{
	if (status == 200 && data)
		stream_read(data, strlen(data), true);
	if (status == 200 && cv.call_count) {
		calls_run();
		return;
	}
	if (status == 200 && cv.reply_len) {
		reply_keep();
		char buf[8];
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(cv.reply_time, buf);
		return;
	}

	/* Failed: the error from the API ({"error":{"message":...}}) if any */
	lv_obj_delete(cv.reply_group);
	cv.reply_group = NULL;
	reply_clear();
	char text[256];
	struct json *root = json_parse(data);
	const char *message = json_str(json_get(json_get(root, "error"), "message"), NULL);
	if (!status)
		snprintf(text, sizeof(text), LV_SYMBOL_WARNING " No internet connection. Tap to try again.");
	else if (status == 200)
		snprintf(text, sizeof(text), LV_SYMBOL_WARNING " Empty answer. Tap to try again.");
	else if (message)
		snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s (%ld). Tap to try again.", message, status);
	else
		snprintf(text, sizeof(text), LV_SYMBOL_WARNING " HTTP error %ld. Tap to try again.", status);
	json_free(root);
	view_status(text, UI_COLOR_RED, retry_cb);
	ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
}

/* The conversation so far to the model, which answers the last message.
 * Messages of the same role in a row (a question not answered) are joined;
 * steps are sent as they are. */
static void ask(void)
{
	const struct provider *p;
	const struct model *model = model_current(&p);
	const char *key = provider_key(p);
	if (busy() || !cv.count || cv.turns[cv.count - 1].role == ASSISTANT)
		return;
	view_status_clear();
	if (!key[0]) {
		char text[128];
		snprintf(text, sizeof(text), "No %s API key: add one in the settings (" LV_SYMBOL_SETTINGS
		         "), then tap here.", p->name);
		view_status(text, UI_COLOR_DIM, retry_cb);
		ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
		return;
	}

	char *body = NULL;
	size_t body_size = 0;
	FILE *f = open_memstream(&body, &body_size);
	if (!f)
		return;
	fprintf(f, "{\"model\":\"%s\",\"stream\":true,", model->id);
	/* On by default: slow on a phone, so it is a setting (off unless set) */
	if (p->thinking)
		fprintf(f, "\"thinking\":{\"type\":\"%s\"},",
		        ui_setting_get_int("assistant.thinking", 0) ? "enabled" : "disabled");
	/* Searches until MAX_ROUNDS, then it has to answer. Earlier searches need
	 * the tool declared, even when it is off now. */
	bool steps = false;
	for (unsigned i = 0; i < cv.count && !steps; i++)
		steps = cv.turns[i].role == STEP;
	bool search = search_on(p) && cv.rounds < MAX_ROUNDS;
	if (search || steps)
		fprintf(f, "\"tools\":[" SEARCH_TOOL "],\"tool_choice\":\"%s\",", search ? "auto" : "none");

	char system[sizeof(SYSTEM_PROMPT) + 64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(system, sizeof(system), SYSTEM_PROMPT " Today is %04d-%02d-%02d.",
	         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	fputs("\"messages\":[{\"role\":\"system\",\"content\":", f);
	put_string(f, system);
	fputc('}', f);
	bool open = false; /* a message's content not closed yet */
	enum role prev = STEP;
	for (unsigned i = 0; i < cv.count; i++) {
		const struct turn *t = &cv.turns[i];
		if (t->role == STEP) {
			fprintf(f, "%s,%s", open ? "\"}" : "", t->message);
			open = false;
			prev = STEP;
			continue;
		}
		size_t size = strlen(t->text) * 6 + 1;
		char *escaped = malloc(size);
		if (!escaped)
			continue;
		json_escape(escaped, size, t->text);
		if (open && t->role == prev)
			fprintf(f, "\\n\\n%s", escaped);
		else
			fprintf(f, "%s,{\"role\":\"%s\",\"content\":\"%s", open ? "\"}" : "",
			        role_names[t->role], escaped);
		free(escaped);
		open = true;
		prev = t->role;
	}
	fputs(open ? "\"}]}" : "]}", f);
	fclose(f);

	char auth[256];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
	const char *headers[] = { "Content-Type: application/json", auth, NULL };
	cv.parsed = 0;
	ui_fetch_post(&cv.fetch, p->url, headers, body, stream_progress, stream_done);
	free(body);

	cv.reply_label = view_add(false, "...", 0, &cv.reply_time);
	cv.reply_group = lv_obj_get_parent(lv_obj_get_parent(cv.reply_label));
	lv_label_set_text_static(cv.reply_time, "Waiting...");
	ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
}

static void conversation_send(struct ui_chat *chat, const char *text)
{
	/* One question at a time: back in the field until the answer is there */
	if (busy() || !turn_add(USER, time(NULL), text, NULL)) {
		lv_textarea_set_text(chat->ta, text);
		return;
	}
	view_status_clear();
	view_add(true, text, cv.turns[cv.count - 1].time, NULL);
	conversation_save();
	cv.rounds = 0;
	ask();
}

static void settings_cb(lv_event_t *e);

static void trash_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_keyboard_show(cv.chat.kb, false);
	lv_obj_set_hidden(cv.confirm, !lv_obj_is_hidden(cv.confirm));
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(cv.confirm, true);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	ui_fetch_stop(&cv.fetch);
	ui_fetch_stop(&cv.search);
	reply_clear();
	if (cv.path[0]) {
		unlink(cv.path);
		assistant_version++;
	}
	turns_drop(0); /* nothing left to save */
	ui_pop();
}

static void conversation_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* Left during searches: the call needs all its results, they go; left
	 * while an answer came: what came is kept */
	if (cv.search.curl) {
		ui_fetch_stop(&cv.search);
		turns_drop(cv.steps_start);
		reply_clear();
		conversation_save();
	}
	ui_fetch_stop(&cv.fetch);
	reply_keep();
	turns_drop(0);
	free(cv.turns);
	memset(&cv, 0, sizeof(cv));
}

/* path: a saved conversation, NULL: a new one */
static void conversation_open(const char *path)
{
	if (cv.scr)
		return;
	const struct provider *p;
	model_current(&p);

	lv_obj_t *header, *body;
	cv.scr = ui_app_screen_create(p->name, &header, &body);
	lv_obj_add_event_cb(cv.scr, conversation_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, NULL);
	ui_header_button(header, LV_SYMBOL_SETTINGS, settings_cb, NULL);
	cv.chat.send = conversation_send;
	ui_chat_create(&cv.chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(cv.chat.ta, "Ask anything");

	/* Delete confirmation, above the compose bar */
	cv.confirm = ui_confirm_bar(body, LV_SYMBOL_TRASH "  Delete chat", delete_cb, cancel_cb, NULL);
	lv_obj_move_to_index(cv.confirm, 1);

	if (path)
		conversation_load(path);
	for (unsigned i = 0; i < cv.count; i++)
		view_turn(&cv.turns[i]);
	if (!cv.count) {
		lv_obj_t *hint = ui_empty_label(cv.chat.list, "Ask a question below");
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_margin_top(hint, 200, 0);
		cv.status = hint;
	} else if (cv.turns[cv.count - 1].role != ASSISTANT) {
		view_status("Not answered. Tap to try again.", UI_COLOR_DIM, retry_cb);
	}

	ui_chat_scroll_end(&cv.chat, LV_ANIM_OFF);
	ui_push(cv.scr);
	if (!cv.count)
		ui_keyboard_show(cv.chat.kb, true);
}

/* Settings: the API key of each provider, the model */

static struct {
	lv_obj_t *kb;
	lv_obj_t *keys[PROVIDER_COUNT];
	lv_obj_t *checks[PROVIDER_COUNT * 4];
} st;

static void keys_save(void)
{
	for (unsigned i = 0; i < PROVIDER_COUNT; i++) {
		if (!st.keys[i])
			continue;
		/* Pasted or typed with spaces around */
		char key[200];
		const char *text = lv_textarea_get_text(st.keys[i]);
		while (*text == ' ')
			text++;
		snprintf(key, sizeof(key), "%s", text);
		size_t n = strlen(key);
		while (n && key[n - 1] == ' ')
			key[--n] = '\0';
		ui_setting_set(providers[i].key_setting, key);
	}
}

static void key_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		lv_keyboard_set_textarea(st.kb, lv_event_get_target_obj(e));
		ui_keyboard_show(st.kb, true);
		break;
	case LV_EVENT_READY:
	case LV_EVENT_CANCEL:
		ui_keyboard_show(st.kb, false);
		keys_save();
		break;
	default:
		break;
	}
}

static void model_cb(lv_event_t *e)
{
	const struct model *m = lv_event_get_user_data(e);
	ui_vibrate(15);
	ui_setting_set("assistant.model", m->id);
	const struct model *cur = model_current(NULL);
	unsigned k = 0;
	for (unsigned p = 0; p < PROVIDER_COUNT; p++)
		for (const struct model *pm = providers[p].models; pm->id; pm++, k++)
			lv_obj_set_hidden(st.checks[k], pm != cur);
}

static void switch_cb(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_target_obj(e);
	ui_vibrate(15);
	ui_setting_set_int(lv_event_get_user_data(e), lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* A row of the card with a switch for the setting `key` (0 or 1, off unless set) */
static void switch_row(lv_obj_t *card, bool divider, const char *title, const char *info, const char *key)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, NULL, NULL);
	lv_obj_t *text = ui_row_text(row);
	lv_obj_set_width(ui_one_line_label(text, title, &ui_font_26), LV_PCT(100));
	lv_obj_set_width(ui_dim_label(text, info, &ui_font_20), LV_PCT(100));

	lv_obj_t *sw = lv_switch_create(row);
	lv_obj_set_size(sw, 100, 56);
	lv_obj_set_ext_click_area(sw, 16);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUSED);
	lv_obj_set_state(sw, LV_STATE_CHECKED, ui_setting_get_int(key, 0));
	lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void settings_unload_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	keys_save();
}

static void settings_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&st, 0, sizeof(st));
}

static void keys_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Assistant settings", NULL, &body);
	lv_obj_add_event_cb(scr, settings_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_add_event_cb(scr, settings_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	/* Scrolls above the keyboard */
	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(content, 16, 0);
	lv_obj_set_style_pad_bottom(content, 32, 0);
	lv_obj_set_style_pad_row(content, 12, 0);
	lv_obj_set_scrollable(content, true);
	lv_obj_set_clickable(content, true);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

	const struct model *cur = model_current(NULL);
	unsigned k = 0;
	for (unsigned p = 0; p < PROVIDER_COUNT; p++) {
		const struct provider *pr = &providers[p];
		ui_section_label(content, pr->name);
		lv_obj_t *ta = ui_text_field(content, 88, 190, key_cb, NULL);
		lv_obj_set_width(ta, LV_PCT(100));
		lv_textarea_set_placeholder_text(ta, "API key");
		lv_textarea_set_text(ta, provider_key(pr));
		st.keys[p] = ta;
		lv_obj_t *hint = ui_dim_label(content, pr->key_hint, &ui_font_20);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_pad_hor(hint, 28, 0);

		ui_section_label(content, "Model");
		lv_obj_t *card = ui_card_create(content);
		for (const struct model *m = pr->models; m->id; m++, k++) {
			lv_obj_t *row = ui_row_create(card, ROW_H, m[1].id != NULL, model_cb, (void *)m);
			lv_obj_t *text = ui_row_text(row);
			lv_obj_set_width(ui_one_line_label(text, m->id, &ui_font_26), LV_PCT(100));
			lv_obj_set_width(ui_dim_label(text, m->info, &ui_font_20), LV_PCT(100));
			st.checks[k] = lv_label_create(row);
			lv_label_set_text_static(st.checks[k], LV_SYMBOL_OK);
			lv_obj_set_style_text_color(st.checks[k], lv_color_hex(UI_COLOR_ACCENT), 0);
			lv_obj_set_hidden(st.checks[k], m != cur);
		}
		if (pr->thinking || pr->search_url) {
			card = ui_card_create(content);
			lv_obj_set_style_margin_top(card, 16, 0);
		}
		if (pr->thinking)
			switch_row(card, pr->search_url != NULL, "Thinking", "Better answers, takes much longer",
			           "assistant.thinking");
		if (pr->search_url)
			switch_row(card, false, "Web search", "Looks things up, $0.75 per 1000 searches",
			           "assistant.search");
	}

	st.kb = ui_keyboard_create(body, st.keys[0]);
	ui_push(scr);
}

static void settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cv.chat.kb)
		ui_keyboard_show(cv.chat.kb, false);
	keys_open();
}

/* Conversation list, newest first */

struct item {
	char path[PATH_MAX];
	time_t time;   /* of the last message */
	char *title;   /* the first question */
	char *preview; /* the last message */
	char when[24];
};

static struct {
	lv_obj_t *body;
	struct item *items;
	unsigned count;
	unsigned version;
} list;

static void items_free(void)
{
	for (unsigned i = 0; i < list.count; i++) {
		free(list.items[i].title);
		free(list.items[i].preview);
	}
	free(list.items);
	list.items = NULL;
	list.count = 0;
}

static int item_cmp(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	return (y->time > x->time) - (y->time < x->time);
}

static void items_load(void)
{
	items_free();
	DIR *d = opendir(DATA_DIR);
	if (!d)
		return;
	unsigned cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 6 || strcmp(de->d_name + n - 5, ".json"))
			continue;
		if (list.count == cap) {
			cap = cap ? cap * 2 : 16;
			struct item *items = realloc(list.items, cap * sizeof(*items));
			if (!items)
				break;
			list.items = items;
		}
		struct item *it = &list.items[list.count];
		memset(it, 0, sizeof(*it));
		snprintf(it->path, sizeof(it->path), DATA_DIR "/%s", de->d_name);
		char *text = file_read(it->path);
		struct json *root = json_parse(text);
		free(text);
		const struct json *messages = json_get(root, "messages");
		/* The last message, not a search */
		const struct json *first = messages ? messages->child : NULL, *last = first;
		for (const struct json *m = first; m; m = m->next)
			if (strcmp(json_str(json_get(m, "role"), ""), "step"))
				last = m;
		if (first) {
			it->time = json_int(json_get(last, "time"), 0);
			it->title = ui_one_line("", json_str(json_get(first, "content"), ""), PREVIEW_MAX);
			it->preview = ui_one_line(strcmp(json_str(json_get(last, "role"), ""), "user") ? "" : "You: ",
			                          json_str(json_get(last, "content"), ""), PREVIEW_MAX);
			ui_when_format(it->when, sizeof(it->when), it->time);
			list.count++;
		}
		json_free(root);
	}
	closedir(d);
	qsort(list.items, list.count, sizeof(*list.items), item_cmp);
}

static void row_cb(lv_event_t *e)
{
	const struct item *it = lv_event_get_user_data(e);
	conversation_open(it->path);
}

static void list_fill(void)
{
	lv_obj_clean(list.body);
	items_load();
	list.version = assistant_version;
	if (!list.count) {
		ui_empty_label(list.body, "No chats");
		return;
	}
	lv_obj_t *card = ui_card_create(list.body);
	for (unsigned i = 0; i < list.count; i++) {
		struct item *it = &list.items[i];
		lv_obj_t *row = ui_row_create(card, ROW_H, i + 1 < list.count, row_cb, it);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *top = ui_row_line(text);
		lv_obj_set_flex_grow(ui_one_line_label(top, it->title ? it->title : "", &ui_font_26), 1);
		ui_dim_label(top, it->when, &ui_font_20);
		lv_obj_t *preview = ui_one_line_label(text, it->preview ? it->preview : "", &ui_font_20);
		lv_obj_set_width(preview, LV_PCT(100));
		lv_obj_set_style_text_color(preview, lv_color_hex(UI_COLOR_DIM), 0);
	}
}

/* Back from a conversation: it may have changed */
static void list_load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (list.version != assistant_version)
		list_fill();
}

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	items_free();
	memset(&list, 0, sizeof(list));
}

static void new_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	conversation_open(NULL);
}

void assistant_open(void)
{
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create("Assistant", &header, &list.body);
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	ui_header_button(header, LV_SYMBOL_EDIT, new_cb, NULL);
	ui_header_button(header, LV_SYMBOL_SETTINGS, settings_cb, NULL);
	lv_obj_set_style_pad_hor(list.body, 16, 0);
	lv_obj_set_style_pad_top(list.body, 8, 0);
	lv_obj_set_style_pad_bottom(list.body, 32, 0);
	lv_obj_set_scroll_dir(list.body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list.body, LV_SCROLLBAR_MODE_OFF);
	list_fill();
	ui_push(scr);
}
