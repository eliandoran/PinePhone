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
 *
 * Shared between its files (ai_: phone-ui's symbols are seen by plugins):
 *
 * - assistant.c     the app's screen: the conversations, newest first
 * - provider.c      providers, their models and API keys
 * - store.c         the open conversation's messages, its file
 * - conversation.c  the conversation's screen and its view
 * - ask.c           asking the model: the request, the streamed answer, searches
 * - settings.c      the settings screen
 */

#ifndef PHONE_UI_ASSISTANT_H
#define PHONE_UI_ASSISTANT_H

#include <limits.h>
#include <stdio.h>
#include <time.h>

#include "../ui.h"

#define DATA_DIR "/var/lib/assistant"
#define ROW_H 120
#define MAX_PROVIDERS 4
#define MAX_MODELS 4 /* per provider, with the empty one ending them */
#define MAX_CALLS 4  /* searches in one answer */
#define MAX_ROUNDS 3 /* answers with searches for one question */

/* provider.c */

struct model {
	const char *id, *info;
};

/* Providers with an OpenAI-style chat completions API */
struct provider {
	const char *name;
	const char *url;
	const char *key_setting;
	const char *key_hint;
	struct model models[MAX_MODELS]; /* the first is the default */
	bool thinking; /* takes "thinking": {"type": "enabled"} or "disabled" */
	/* Web search: POST {"query", "count"}, answers {"results": [{"title",
	 * "url", "summary", "date"}]}; NULL: none */
	const char *search_url;
};
extern const struct provider ai_providers[];
extern const unsigned ai_provider_count;

/* The model chosen in the settings, and its provider */
const struct model *ai_model_current(const struct provider **provider);
const char *ai_provider_key(const struct provider *p);
bool ai_search_on(const struct provider *p);

/* store.c */

/* A message of a conversation. Steps are the model's web searches: its call
 * (not shown) and each result (shown as the query), kept as the messages the
 * API takes, to be sent again with the rest. */
enum role { USER, ASSISTANT, STEP };
extern const char *const ai_role_names[];

struct turn {
	enum role role;
	time_t time;
	char *text;    /* shown, "" for none */
	char *message; /* a step: its message for the API (JSON), NULL otherwise */
};

/* A web search the model asked for, as it streams in */
struct call {
	char *id, *args;
	size_t id_len, args_len;
};

/* The open conversation: its screen, messages, and the answer on its way */
struct conversation {
	lv_obj_t *scr, *confirm;
	lv_obj_t *status; /* an error or a hint under the messages */
	struct ui_chat chat;
	char path[PATH_MAX]; /* "": not saved yet */
	struct turn *turns;
	unsigned count, cap;
	/* The answer on its way (ask.c) */
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
};
extern struct conversation ai_cv;

/* Bumped when a conversation is saved or deleted: the list reloads */
extern unsigned ai_version;

char *ai_file_read(const char *path); /* malloc'd, NULL if it cannot be read */
void ai_put_string(FILE *f, const char *text); /* as a JSON string */
void ai_append(char **s, size_t *len, const char *add);
bool ai_turn_add(enum role role, time_t when, const char *text, const char *message);
void ai_turns_drop(unsigned from); /* the turns from this one on */
void ai_save(void);
void ai_load(const char *path);

/* conversation.c */

void ai_conversation_open(const char *path); /* NULL: a new one */
bool ai_view_at_end(void);
void ai_view_set_text(lv_obj_t *label, const char *text); /* emoji composed */
/* A message: its bubble's label; answers take the width; when 0: no time yet */
lv_obj_t *ai_view_add(bool user, const char *text, time_t when, lv_obj_t **time_label);
lv_obj_t *ai_view_note(const char *text, uint32_t color); /* centered between them */
/* Under the messages: tapping it calls cb (try again), if any */
void ai_view_status(const char *text, uint32_t color, lv_event_cb_t cb);
void ai_view_status_clear(void);

/* ask.c */

/* The conversation so far to the model, which answers the last message */
void ai_ask(void);
bool ai_busy(void); /* an answer or a search on its way */
void ai_retry_cb(lv_event_t *e); /* asks again */
/* Leaving: searches not done are dropped, what came of an answer is kept */
void ai_ask_stop(void);
void ai_ask_cancel(void); /* deleting the conversation: nothing kept */

/* settings.c */

void ai_settings_open(void);

#endif
