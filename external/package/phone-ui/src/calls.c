/*
 * Call log, see data.h: /var/lib/phone/calls.jsonl, a JSON object per line,
 * oldest first. Calls are appended (a power cut loses at most the line being
 * written, which is then skipped); the file is rewritten only to drop the
 * oldest ones, and when missed calls were seen ("new": true until then).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "data.h"
#include "json.h"

#define PHONE "/var/lib/phone"
#define CALLS_FILE PHONE "/calls.jsonl"
#define MAX_CALLS 500
#define TRIM_AT (MAX_CALLS + 100) /* rewrite the file once it has this many */

static const char *const kinds[] = { "outgoing", "incoming", "missed" };

static struct call *calls; /* oldest first */
static unsigned count, size, in_file;
static bool loaded;

static void append(const char *number, enum call_kind kind, time_t start, unsigned duration, bool unseen)
{
	if (count == size) {
		size = size ? size * 2 : 64;
		calls = realloc(calls, size * sizeof(*calls));
	}
	calls[count++] = (struct call){ ui_intern(number), start, duration, kind, unseen };
}

static void load(void)
{
	if (loaded)
		return;
	loaded = true;

	FILE *f = fopen(CALLS_FILE, "r");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		in_file++;
		struct json *v = json_parse(line);
		const char *number = json_str(json_get(v, "number"), "");
		const char *kind = json_str(json_get(v, "kind"), "");
		unsigned k = 0;
		while (k < sizeof(kinds) / sizeof(kinds[0]) && strcmp(kind, kinds[k]) != 0)
			k++;
		if (number[0] && k < sizeof(kinds) / sizeof(kinds[0]))
			append(number, k, json_int(json_get(v, "time"), 0), json_int(json_get(v, "duration"), 0),
			       k == CALL_MISSED && json_bool(json_get(v, "new")));
		json_free(v);
	}
	fclose(f);

	if (count > MAX_CALLS) {
		memmove(calls, calls + count - MAX_CALLS, MAX_CALLS * sizeof(*calls));
		count = MAX_CALLS;
	}
}

static void put(FILE *f, const struct call *c)
{
	char number[6 * 64 + 1];
	json_escape(number, sizeof(number), c->number);
	fprintf(f, "{ \"number\": \"%s\", \"time\": %lld, \"kind\": \"%s\", \"duration\": %u%s }\n", number,
	        (long long)c->time, kinds[c->kind], c->duration, c->unseen ? ", \"new\": true" : "");
}

/* Synced: the battery can cut power at any time */
static bool finish(FILE *f, const char *path)
{
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok)
		fprintf(stderr, "phone-ui: calls: cannot write %s\n", path);
	return ok;
}

/* The calls in RAM only, through a temporary file */
static void rewrite(void)
{
	FILE *f = fopen(CALLS_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: calls");
		return;
	}
	for (unsigned i = 0; i < count; i++)
		put(f, &calls[i]);
	if (finish(f, CALLS_FILE ".new") && rename(CALLS_FILE ".new", CALLS_FILE) == 0)
		in_file = count;
	else
		unlink(CALLS_FILE ".new");
}

unsigned call_count(void)
{
	load();
	return count;
}

const struct call *call_get(unsigned i)
{
	load();
	return i < count ? &calls[count - 1 - i] : NULL;
}

void call_log_add(const char *number, enum call_kind kind, time_t start, unsigned duration)
{
	load();
	append(number, kind, start, duration, kind == CALL_MISSED);
	if (count > MAX_CALLS) {
		memmove(calls, calls + 1, (count - 1) * sizeof(*calls));
		count--;
	}
	data_version++;

	mkdir(PHONE, 0755);
	if (in_file + 1 >= TRIM_AT) {
		rewrite();
		return;
	}
	FILE *f = fopen(CALLS_FILE, "a");
	if (!f) {
		perror("phone-ui: calls");
		return;
	}
	put(f, &calls[count - 1]);
	if (finish(f, CALLS_FILE))
		in_file++;
}

unsigned call_missed_unseen(void)
{
	load();
	unsigned n = 0;
	for (unsigned i = 0; i < count; i++)
		n += calls[i].unseen;
	return n;
}

void call_missed_seen(void)
{
	if (!call_missed_unseen())
		return;
	for (unsigned i = 0; i < count; i++)
		calls[i].unseen = false;
	data_version++;
	mkdir(PHONE, 0755);
	rewrite();
}

void call_log_clear(void)
{
	load();
	count = 0;
	data_version++;
	if (unlink(CALLS_FILE) == 0 || access(CALLS_FILE, F_OK) != 0)
		in_file = 0;
	else
		perror("phone-ui: calls");
}
