/*
 * Assistant: conversations with a language model over its chat completions API
 * (OpenAI's format, which DeepSeek's follows), answers streamed into the bubble
 * as they come. Each conversation is a JSON file in /var/lib/assistant (its
 * messages with their times); the API keys and the model are in the settings
 * (assistant.<provider>.key, assistant.model), set on the settings screen.
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
#define MODEL_DEFAULT "deepseek-chat"
/* The answers are shown as plain text with the fonts' characters */
#define SYSTEM_PROMPT "You are a helpful assistant on a small phone screen. Answer briefly, " \
	"in plain text: no Markdown (no **, #, tables or code fences), no emoji, no LaTeX."

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
} providers[] = {
	{ "DeepSeek", "https://api.deepseek.com/chat/completions", "assistant.deepseek.key",
	  "Create one on platform.deepseek.com, API keys.",
	  { { "deepseek-chat", "Fast answers" },
	    { "deepseek-reasoner", "Thinks before answering, slower" } } },
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

/* A message of a conversation */
struct turn {
	bool user;
	time_t time;
	char *text;
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
		        turns[i].user ? "user" : "assistant", (long long)turns[i].time);
		put_string(f, turns[i].text);
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
	lv_obj_t *reply_group, *reply_label, *reply_time;
} cv;

static bool turn_add(bool user, time_t when, const char *text)
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
	if (!copy)
		return false;
	cv.turns[cv.count++] = (struct turn){ user, when, copy };
	return true;
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
	for (const struct json *m = messages ? messages->child : NULL; m; m = m->next)
		turn_add(!strcmp(json_str(json_get(m, "role"), ""), "user"), json_int(json_get(m, "time"), 0),
		         json_str(json_get(m, "content"), ""));
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

/* Centered under the messages: tapping it calls cb (try again) */
static void view_status(const char *text, uint32_t color, lv_event_cb_t cb)
{
	if (cv.status)
		lv_obj_delete(cv.status);
	cv.status = lv_label_create(cv.chat.list);
	lv_label_set_text(cv.status, text);
	lv_obj_set_width(cv.status, LV_PCT(100));
	lv_label_set_long_mode(cv.status, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(cv.status, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(cv.status, &ui_font_20, 0);
	lv_obj_set_style_text_color(cv.status, lv_color_hex(color), 0);
	lv_obj_set_style_pad_ver(cv.status, 12, 0);
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
		const char *content = json_str(json_get(delta, "content"), "");
		size_t n = strlen(content);
		char *reply = n ? realloc(cv.reply, cv.reply_len + n + 1) : NULL;
		if (reply) {
			memcpy(reply + cv.reply_len, content, n + 1);
			cv.reply = reply;
			cv.reply_len += n;
			changed = true;
		} else if (!cv.reply_len && json_str(json_get(delta, "reasoning_content"), NULL)) {
			lv_label_set_text_static(cv.reply_time, "Thinking...");
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

/* The answer as a message, saved */
static void reply_keep(void)
{
	const char *text = cv.reply ? cv.reply : "";
	while (*text == '\n')
		text++;
	if (*text) {
		turn_add(false, time(NULL), text);
		conversation_save();
	}
	free(cv.reply);
	cv.reply = NULL;
	cv.reply_len = 0;
}

static void stream_done(const char *data, long status)
{
	if (status == 200 && data)
		stream_read(data, strlen(data), true);
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
	free(cv.reply);
	cv.reply = NULL;
	cv.reply_len = 0;
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
 * Messages of the same role in a row (a question not answered) are joined. */
static void ask(void)
{
	const struct provider *p;
	const struct model *model = model_current(&p);
	const char *key = provider_key(p);
	if (cv.fetch.curl || !cv.count || !cv.turns[cv.count - 1].user)
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
	fprintf(f, "{\"model\":\"%s\",\"stream\":true,\"messages\":[{\"role\":\"system\",\"content\":", model->id);
	put_string(f, SYSTEM_PROMPT);
	fputc('}', f);
	for (unsigned i = 0; i < cv.count; i++) {
		size_t size = strlen(cv.turns[i].text) * 6 + 1;
		char *escaped = malloc(size);
		if (!escaped)
			continue;
		json_escape(escaped, size, cv.turns[i].text);
		if (i && cv.turns[i].user == cv.turns[i - 1].user)
			fprintf(f, "\\n\\n%s", escaped);
		else
			fprintf(f, "%s{\"role\":\"%s\",\"content\":\"%s", i ? "\"}," : ",",
			        cv.turns[i].user ? "user" : "assistant", escaped);
		free(escaped);
	}
	fputs("\"}]}", f);
	fclose(f);

	char auth[256];
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
	const char *headers[] = { "Content-Type: application/json", auth, NULL };
	cv.parsed = 0;
	ui_fetch_post(&cv.fetch, p->url, headers, body, stream_progress, stream_done);
	free(body);

	cv.reply_group = NULL;
	cv.reply_label = view_add(false, "...", 0, &cv.reply_time);
	cv.reply_group = lv_obj_get_parent(lv_obj_get_parent(cv.reply_label));
	lv_label_set_text_static(cv.reply_time, "Waiting...");
	ui_chat_scroll_end(&cv.chat, LV_ANIM_ON);
}

static void conversation_send(struct ui_chat *chat, const char *text)
{
	/* One question at a time: back in the field until the answer is there */
	if (cv.fetch.curl || !turn_add(true, time(NULL), text)) {
		lv_textarea_set_text(chat->ta, text);
		return;
	}
	view_status_clear();
	view_add(true, text, cv.turns[cv.count - 1].time, NULL);
	conversation_save();
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
	free(cv.reply);
	cv.reply = NULL;
	cv.reply_len = 0;
	if (cv.path[0]) {
		unlink(cv.path);
		assistant_version++;
	}
	cv.count = 0; /* nothing left to save */
	ui_pop();
}

static void conversation_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* Left while an answer came: what came is kept */
	ui_fetch_stop(&cv.fetch);
	reply_keep();
	for (unsigned i = 0; i < cv.count; i++)
		free(cv.turns[i].text);
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
		view_add(cv.turns[i].user, cv.turns[i].text, cv.turns[i].time, NULL);
	if (!cv.count) {
		lv_obj_t *hint = ui_empty_label(cv.chat.list, "Ask a question below");
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_margin_top(hint, 200, 0);
		cv.status = hint;
	} else if (cv.turns[cv.count - 1].user) {
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
		const struct json *first = messages ? messages->child : NULL, *last = first;
		while (last && last->next)
			last = last->next;
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
