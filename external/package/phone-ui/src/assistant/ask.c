/*
 * Assistant asking the model: the conversation as a request, the answer
 * streamed into its bubble, the web searches it asks for.
 */

#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "assistant.h"

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

bool ai_busy(void)
{
	return ai_cv.fetch.curl || ai_cv.search.curl;
}

void ai_retry_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ai_ask();
}

/* Server-sent events: "data: {chunk}" lines, then "data: [DONE]". `all`: the
 * last line even without its newline (the end of the stream) */
static void stream_read(const char *data, size_t size, bool all)
{
	bool changed = false;
	while (ai_cv.parsed < size) {
		const char *line = data + ai_cv.parsed;
		const char *nl = memchr(line, '\n', size - ai_cv.parsed);
		if (!nl && !all)
			break;
		size_t len = nl ? (size_t)(nl - line) : size - ai_cv.parsed;
		ai_cv.parsed += len + (nl != NULL);
		if (len < 6 || strncmp(line, "data: ", 6) != 0)
			continue; /* blank lines, ": keep-alive" */
		char *payload = strndup(line + 6, len - 6);
		struct json *chunk = json_parse(payload);
		free(payload);
		const struct json *choices = json_get(chunk, "choices");
		const struct json *delta = json_get(choices ? choices->child : NULL, "delta");

		size_t before = ai_cv.reply_len;
		ai_append(&ai_cv.reply, &ai_cv.reply_len, json_str(json_get(delta, "content"), ""));
		changed |= ai_cv.reply_len != before;
		const char *reasoning = json_str(json_get(delta, "reasoning_content"), NULL);
		if (reasoning) {
			ai_append(&ai_cv.reasoning, &ai_cv.reasoning_len, reasoning);
			if (!ai_cv.reply_len)
				lv_label_set_text_static(ai_cv.reply_time, "Thinking...");
		}
		/* Calls come in pieces: the id and name first, then the arguments */
		const struct json *calls = json_get(delta, "tool_calls");
		for (const struct json *c = calls ? calls->child : NULL; c; c = c->next) {
			long long i = json_int(json_get(c, "index"), 0);
			if (i < 0 || i >= MAX_CALLS)
				continue;
			struct call *call = &ai_cv.calls[i];
			if (ai_cv.call_count <= i)
				ai_cv.call_count = i + 1;
			ai_append(&call->id, &call->id_len, json_str(json_get(c, "id"), ""));
			ai_append(&call->args, &call->args_len,
			       json_str(json_get(json_get(c, "function"), "arguments"), ""));
			lv_label_set_text_static(ai_cv.reply_time, "Searching the web...");
		}
		json_free(chunk);
	}
	if (!changed)
		return;
	bool end = ai_view_at_end();
	/* Leading new lines (after the reasoning) are not shown */
	const char *text = ai_cv.reply;
	while (*text == '\n')
		text++;
	ai_view_set_text(ai_cv.reply_label, text);
	lv_label_set_text_static(ai_cv.reply_time, "Writing...");
	if (end)
		ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_OFF);
}

static void stream_progress(const char *data, size_t size)
{
	stream_read(data, size, false);
}

static void reply_clear(void)
{
	free(ai_cv.reply);
	free(ai_cv.reasoning);
	ai_cv.reply = ai_cv.reasoning = NULL;
	ai_cv.reply_len = ai_cv.reasoning_len = 0;
	for (unsigned i = 0; i < MAX_CALLS; i++) {
		free(ai_cv.calls[i].id);
		free(ai_cv.calls[i].args);
	}
	memset(ai_cv.calls, 0, sizeof(ai_cv.calls));
	ai_cv.call_count = ai_cv.searching = 0;
}

/* The answer as a message, saved */
static void reply_keep(void)
{
	const char *text = ai_cv.reply ? ai_cv.reply : "";
	while (*text == '\n')
		text++;
	if (*text) {
		ai_turn_add(ASSISTANT, time(NULL), text, NULL);
		ai_save();
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
	const struct call *call = &ai_cv.calls[ai_cv.searching];
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
		ai_put_string(f, call->id ? call->id : "");
		fputs(",\"content\":", f);
		ai_put_string(f, result ? result : "");
		fputc('}', f);
		fclose(f);
	}
	free(result);

	char *query = call_query(call);
	char shown[192];
	snprintf(shown, sizeof(shown), status == 200 ? LV_SYMBOL_OK " Searched the web: %s" :
	         LV_SYMBOL_WARNING " Web search failed: %s", query ? query : "");
	free(query);
	ai_view_set_text(ai_cv.step_label, shown);
	ai_turn_add(STEP, time(NULL), shown, message ? message : "{}");
	free(message);

	ai_cv.searching++;
	search_next();
}

/* The calls one at a time, then the model again with their results */
static void search_next(void)
{
	if (ai_cv.searching == ai_cv.call_count) {
		reply_clear();
		ai_save();
		ai_ask();
		return;
	}
	const struct provider *p;
	ai_model_current(&p);
	char *query = call_query(&ai_cv.calls[ai_cv.searching]);
	char text[192];
	snprintf(text, sizeof(text), "Searching the web: %s", query ? query : "");
	ai_cv.step_label = ai_view_note(text, UI_COLOR_DIM);
	ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_ON);
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
		ai_put_string(f, query);
		fprintf(f, ",\"count\":%d}", SEARCH_RESULTS);
		fclose(f);
	}
	free(query);
	char auth[256];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ai_provider_key(p));
	const char *headers[] = { "Content-Type: application/json", auth, NULL };
	ui_fetch_post(&ai_cv.search, p->search_url, headers, body ? body : "{}", NULL, search_done);
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
	ai_put_string(f, ai_cv.reply ? ai_cv.reply : "");
	if (ai_cv.reasoning) {
		fputs(",\"reasoning_content\":", f);
		ai_put_string(f, ai_cv.reasoning);
	}
	fputs(",\"tool_calls\":[", f);
	for (unsigned i = 0; i < ai_cv.call_count; i++) {
		fprintf(f, "%s{\"id\":", i ? "," : "");
		ai_put_string(f, ai_cv.calls[i].id ? ai_cv.calls[i].id : "");
		fputs(",\"type\":\"function\",\"function\":{\"name\":\"web_search\",\"arguments\":", f);
		ai_put_string(f, ai_cv.calls[i].args ? ai_cv.calls[i].args : "{}");
		fputs("}}", f);
	}
	fputs("]}", f);
	fclose(f);

	lv_obj_delete(ai_cv.reply_group);
	ai_cv.reply_group = NULL;
	ai_cv.steps_start = ai_cv.count;
	ai_cv.rounds++;
	ai_turn_add(STEP, time(NULL), "", message);
	free(message);
	ai_cv.searching = 0;
	search_next();
}

static void stream_done(const char *data, long status)
{
	if (status == 200 && data)
		stream_read(data, strlen(data), true);
	if (status == 200 && ai_cv.call_count) {
		calls_run();
		return;
	}
	if (status == 200 && ai_cv.reply_len) {
		reply_keep();
		char buf[8];
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(ai_cv.reply_time, buf);
		return;
	}

	/* Failed: the error from the API ({"error":{"message":...}}) if any */
	lv_obj_delete(ai_cv.reply_group);
	ai_cv.reply_group = NULL;
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
	ai_view_status(text, UI_COLOR_RED, ai_retry_cb);
	ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_ON);
}

/* The conversation so far to the model, which answers the last message.
 * Messages of the same role in a row (a question not answered) are joined;
 * steps are sent as they are. */
void ai_ask(void)
{
	const struct provider *p;
	const struct model *model = ai_model_current(&p);
	const char *key = ai_provider_key(p);
	if (ai_busy() || !ai_cv.count || ai_cv.turns[ai_cv.count - 1].role == ASSISTANT)
		return;
	ai_view_status_clear();
	if (!key[0]) {
		char text[128];
		snprintf(text, sizeof(text), "No %s API key: add one in the settings (" LV_SYMBOL_SETTINGS
		         "), then tap here.", p->name);
		ai_view_status(text, UI_COLOR_DIM, ai_retry_cb);
		ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_ON);
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
	for (unsigned i = 0; i < ai_cv.count && !steps; i++)
		steps = ai_cv.turns[i].role == STEP;
	bool search = ai_search_on(p) && ai_cv.rounds < MAX_ROUNDS;
	if (search || steps)
		fprintf(f, "\"tools\":[" SEARCH_TOOL "],\"tool_choice\":\"%s\",", search ? "auto" : "none");

	char system[sizeof(SYSTEM_PROMPT) + 64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(system, sizeof(system), SYSTEM_PROMPT " Today is %04d-%02d-%02d.",
	         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	fputs("\"messages\":[{\"role\":\"system\",\"content\":", f);
	ai_put_string(f, system);
	fputc('}', f);
	bool open = false; /* a message's content not closed yet */
	enum role prev = STEP;
	for (unsigned i = 0; i < ai_cv.count; i++) {
		const struct turn *t = &ai_cv.turns[i];
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
			        ai_role_names[t->role], escaped);
		free(escaped);
		open = true;
		prev = t->role;
	}
	fputs(open ? "\"}]}" : "]}", f);
	fclose(f);

	char auth[256];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
	const char *headers[] = { "Content-Type: application/json", auth, NULL };
	ai_cv.parsed = 0;
	ui_fetch_post(&ai_cv.fetch, p->url, headers, body, stream_progress, stream_done);
	free(body);

	ai_cv.reply_label = ai_view_add(false, "...", 0, &ai_cv.reply_time);
	ai_cv.reply_group = lv_obj_get_parent(lv_obj_get_parent(ai_cv.reply_label));
	lv_label_set_text_static(ai_cv.reply_time, "Waiting...");
	ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_ON);
}

void ai_ask_stop(void)
{
	/* A call needs all its results: the searches go */
	if (ai_cv.search.curl) {
		ui_fetch_stop(&ai_cv.search);
		ai_turns_drop(ai_cv.steps_start);
		reply_clear();
		ai_save();
	}
	ui_fetch_stop(&ai_cv.fetch);
	reply_keep();
}

void ai_ask_cancel(void)
{
	ui_fetch_stop(&ai_cv.fetch);
	ui_fetch_stop(&ai_cv.search);
	reply_clear();
}
