/*
 * Assistant's store: the open conversation's messages, and its file in
 * /var/lib/assistant, {"messages": [{"role", "time", "content", "message"}]}.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../json.h"
#include "assistant.h"

#define MAX_FILE (1 << 20) /* bytes read from a conversation */

const char *const ai_role_names[] = { "user", "assistant", "step" };
struct conversation ai_cv;
unsigned ai_version;

char *ai_file_read(const char *path)
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

void ai_put_string(FILE *f, const char *text)
{
	size_t size = strlen(text) * 6 + 1;
	char *escaped = malloc(size);
	if (!escaped)
		return;
	json_escape(escaped, size, text);
	fprintf(f, "\"%s\"", escaped);
	free(escaped);
}


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
		        ai_role_names[turns[i].role], (long long)turns[i].time);
		ai_put_string(f, turns[i].text);
		if (turns[i].message) {
			fputs(",\"message\":", f);
			ai_put_string(f, turns[i].message);
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
	ai_version++;
}

void ai_append(char **s, size_t *len, const char *add)
{
	size_t n = strlen(add);
	char *p = n ? realloc(*s, *len + n + 1) : NULL;
	if (!p)
		return;
	memcpy(p + *len, add, n + 1);
	*s = p;
	*len += n;
}

bool ai_turn_add(enum role role, time_t when, const char *text, const char *message)
{
	if (ai_cv.count == ai_cv.cap) {
		unsigned cap = ai_cv.cap ? ai_cv.cap * 2 : 16;
		struct turn *turns = realloc(ai_cv.turns, cap * sizeof(*turns));
		if (!turns)
			return false;
		ai_cv.turns = turns;
		ai_cv.cap = cap;
	}
	char *copy = strdup(text);
	char *msg = message ? strdup(message) : NULL;
	if (!copy || (message && !msg)) {
		free(copy);
		free(msg);
		return false;
	}
	ai_cv.turns[ai_cv.count++] = (struct turn){ role, when, copy, msg };
	return true;
}

void ai_turns_drop(unsigned from)
{
	while (ai_cv.count > from) {
		ai_cv.count--;
		free(ai_cv.turns[ai_cv.count].text);
		free(ai_cv.turns[ai_cv.count].message);
	}
}

void ai_save(void)
{
	if (!ai_cv.count)
		return;
	if (!ai_cv.path[0])
		snprintf(ai_cv.path, sizeof(ai_cv.path), DATA_DIR "/%lld.json", (long long)ai_cv.turns[0].time);
	conversation_write(ai_cv.path, ai_cv.turns, ai_cv.count);
}

void ai_load(const char *path)
{
	char *text = ai_file_read(path);
	struct json *root = json_parse(text);
	free(text);
	const struct json *messages = json_get(root, "messages");
	for (const struct json *m = messages ? messages->child : NULL; m; m = m->next) {
		const char *role = json_str(json_get(m, "role"), "");
		const char *message = json_str(json_get(m, "message"), NULL);
		if (!strcmp(role, "step") && !message)
			continue;
		ai_turn_add(!strcmp(role, "user") ? USER : !strcmp(role, "step") ? STEP : ASSISTANT,
		         json_int(json_get(m, "time"), 0), json_str(json_get(m, "content"), ""),
		         !strcmp(role, "step") ? message : NULL);
	}
	json_free(root);
	snprintf(ai_cv.path, sizeof(ai_cv.path), "%s", path);
}
