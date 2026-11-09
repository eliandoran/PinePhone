/*
 * The sync, Trilium's sync.ts, on a thread of its own with its own connection to the
 * database, in rounds until the content hashes agree (at most MAX_ROUNDS):
 *
 * - login: POST /api/login/sync, the time signed with the document's secret
 *   (HMAC-SHA256, base64); the server's instanceId, its session's cookie
 * - push: this phone's own changes (edit.c, stamped with its instance, tr_db_instance()),
 *   PUT /api/sync/update a thousand at a time with their rows (a blob's content in
 *   base64), as sync.ts' pushChanges(); what was pulled is not pushed back
 *   (lastSyncedPush moved past it). A sync asked for while one runs runs after it
 * - pull: GET /api/sync/changed from lastSyncedPull, page after page, each applied
 *   (apply.c) in one transaction with the cursor moved: a sync stopped halfway goes
 *   on from there. The next page is asked for while one is applied (struct prefetch).
 *   The progress is what was pulled of all there was (what earlier syncs pulled,
 *   this answer's and what it says is left), as Trilium's setup shows it
 * - POST /api/sync/finished
 * - check: GET /api/sync/check, the server's hashes per table and sector (an ID's
 *   first character) against ours (content_hash.ts); a sector that differs is queued
 *   again on the server (POST /api/sync/queue-sector) and pulled again by the next
 *   round, up to MAX_SECTOR_ATTEMPTS times; then the sync fails rather than claim
 *   the document is whole
 *
 * Done, the document is initialized (the option), as sync.ts' setDbAsInitialized().
 * The UI reads the state (tr_sync_get()) under the lock. The CPU runs at its highest
 * while a sync does. Each page's sizes and times are logged ("trilium: pulled ...").
 */

#define _GNU_SOURCE /* strcasestr */
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "trilium.h"

#define MAX_ROUNDS 10
#define MAX_SECTOR_ATTEMPTS 3 /* sync.ts' MAX_SECTOR_RESYNC_ATTEMPTS */
#define MAX_SECTORS 512
#define PUSH_BATCH "1000" /* changes a request, as sync.ts */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct tr_sync_state state;
static bool running;
static bool rerun; /* asked while one ran: once more after it */

struct attempt {
	char name[32];
	char sector;
	int count;
};

struct sync {
	sqlite3 *db;
	struct tr_http http;
	char instance[16]; /* ours */
	char server[64];   /* the server's instanceId */
	bool initialized;
	long long applied;
	long status; /* of the failure */
	struct tr_applier *applier;
	struct attempt attempts[MAX_SECTORS];
	unsigned attempt_count;
	bool unresolvable;
};

static void step_set(enum tr_step step)
{
	pthread_mutex_lock(&lock);
	state.step = step;
	pthread_mutex_unlock(&lock);
}

static void progress_set(long long pulled, long long total)
{
	pthread_mutex_lock(&lock);
	state.pulled = pulled < 0 ? 0 : pulled > total ? total : pulled;
	state.total = total;
	pthread_mutex_unlock(&lock);
}

/* As Trilium's randomString(): letters and digits */
static long long option_ll(sqlite3 *db, const char *name)
{
	char value[32];
	tr_db_option(db, name, value, sizeof(value));
	return atoll(value);
}

static bool option_ll_set(sqlite3 *db, const char *name, long long value)
{
	char text[32];
	snprintf(text, sizeof(text), "%lld", value);
	return tr_db_option_set(db, name, text);
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* A request's answer: its JSON (NULL with nothing), or the error's text and status;
 * its size and the ms it took to come and to parse, for the log */
struct answer {
	struct json *root;
	const char *error;
	long status;
	size_t bytes;
	long fetch_ms, parse_ms;
};

static void request(struct tr_http *h, const char *method, const char *path, const char *body, struct answer *a)
{
	memset(a, 0, sizeof(*a));
	char *out;
	long start = now_ms();
	long status = tr_http_request(h, method, path, body, &out);
	a->fetch_ms = now_ms() - start;
	a->bytes = out ? strlen(out) : 0;
	if (!status) {
		a->error = N_("Cannot reach the server.");
		return;
	}
	if (status < 200 || status > 299) {
		fprintf(stderr, "trilium: %s %s: %ld %.200s\n", method, path, status, out);
		free(out);
		a->status = status;
		a->error = N_("The server answered with an error (%ld).");
		return;
	}
	bool empty = !out[strspn(out, " \t\r\n")];
	start = now_ms();
	a->root = empty ? NULL : json_parse(out);
	a->parse_ms = now_ms() - start;
	free(out);
	if (!empty && !a->root)
		a->error = N_("The server's answer could not be read.");
}

/* A request whose answer is JSON (or nothing): *root, NULL with nothing; the error's text */
static const char *call(struct sync *s, const char *method, const char *path, const char *body, struct json **root)
{
	struct answer a;
	request(&s->http, method, path, body, &a);
	*root = a.root;
	if (a.error)
		s->status = a.status;
	return a.error;
}

/* login.ts' loginSync(): the time, signed with the document's secret */
static const char *login(struct sync *s)
{
	char secret[256], timestamp[32], body[256];
	tr_db_option(s->db, "documentSecret", secret, sizeof(secret));
	tr_utc_now(timestamp, sizeof(timestamp));
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned len = 0;
	HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)timestamp, strlen(timestamp), digest, &len);
	char hash[EVP_MAX_MD_SIZE * 2];
	EVP_EncodeBlock((unsigned char *)hash, digest, (int)len);
	snprintf(body, sizeof(body), "{\"timestamp\":\"%s\",\"syncVersion\":%d,\"hash\":\"%s\"}", timestamp, /* NON-NLS */
	         TR_SYNC_VERSION, hash);

	char *out;
	long status = tr_http_request(&s->http, "POST", "/api/login/sync", body, &out);
	if (!status)
		return N_("Cannot reach the server.");
	struct json *root = json_parse(out);
	const char *error = NULL;
	if (status == 200) {
		const char *instance = json_str(json_get(root, "instanceId"), "");
		snprintf(s->server, sizeof(s->server), "%s", instance);
		long long max = json_int(json_get(root, "maxEntityChangeId"), -1);
		if (!instance[0] || max < 0)
			error = N_("The server's answer could not be read.");
		else if (option_ll(s->db, "lastSyncedPull") > max) /* a copy of the document made by hand */
			option_ll_set(s->db, "lastSyncedPull", max);
	} else {
		/* Trilium's and the relay's refusals: the clocks, the versions, the secret */
		const char *message = json_str(json_get(root, "message"), out);
		fprintf(stderr, "trilium: login: %ld %.200s\n", status, message);
		s->status = status;
		if ((status == 400 || status == 401) && strcasestr(message, "version"))
			error = N_("The server runs another version of Trilium.");
		else if ((status == 400 || status == 401) && strcasestr(message, "time"))
			error = N_("The phone's clock and the server's differ by more than 5 minutes.");
		else if (status == 400 || status == 401)
			error = N_("The server does not accept this phone's credentials for the document.");
		else
			error = N_("The server answered with an error (%ld).");
	}
	json_free(root);
	free(out);
	return error;
}

/* This phone's first change not pushed (made here: edit.c), 0 for none */
static long long first_local(struct sync *s)
{
	sqlite3_stmt *st;
	long long id = 0;
	if (sqlite3_prepare_v2(s->db, "SELECT MIN(id) FROM entity_changes WHERE isSynced = 1 AND instanceId = ? AND id > ?", /* NON-NLS */
	                       -1, &st, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, s->instance, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, option_ll(s->db, "lastSyncedPush"));
	if (sqlite3_step(st) == SQLITE_ROW)
		id = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return id;
}

/* Pushing: JSON as sync.ts' getEntityChangeRecords() sends it */

static void json_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			fprintf(f, "\\%c", *p);
		else if (*p < 0x20)
			fprintf(f, "\\u%04x", *p);
		else
			fputc(*p, f);
	}
	fputc('"', f);
}

/* A row as an object of its columns: numbers, strings, null; the `base64` column's
 * value (a blob's content) in base64, as sync.ts sends it */
static void row_json(FILE *f, sqlite3_stmt *st, const char *base64)
{
	fputc('{', f);
	for (int i = 0; i < sqlite3_column_count(st); i++) {
		const char *name = sqlite3_column_name(st, i);
		if (i)
			fputc(',', f);
		json_string(f, name);
		fputc(':', f);
		int type = sqlite3_column_type(st, i);
		if (type == SQLITE_NULL) {
			fputs("null", f); /* NON-NLS */
		} else if (base64 && !strcmp(name, base64)) {
			int n = sqlite3_column_bytes(st, i);
			const unsigned char *bytes = sqlite3_column_blob(st, i);
			char *b64 = malloc((size_t)(n + 2) / 3 * 4 + 1);
			if (b64) {
				EVP_EncodeBlock((unsigned char *)b64, bytes, n);
				fprintf(f, "\"%s\"", b64);
				free(b64);
			}
		} else if (type == SQLITE_INTEGER) {
			fprintf(f, "%lld", sqlite3_column_int64(st, i));
		} else if (type == SQLITE_FLOAT) {
			fprintf(f, "%.17g", sqlite3_column_double(st, i));
		} else {
			json_string(f, (const char *)sqlite3_column_text(st, i));
		}
	}
	fputc('}', f);
}

/* An entity's row as JSON, by its table's primary key (entity_constructor.ts); NULL if
 * there is none (to be freed) */
static char *entity_json(struct sync *s, const char *name, const char *id)
{
	/* NON-NLS-BEGIN: Trilium's tables */
	static const char *const keys[][2] = {
		{"notes", "noteId"},         {"blobs", "blobId"},           {"branches", "branchId"},
		{"attributes", "attributeId"}, {"attachments", "attachmentId"}, {"revisions", "revisionId"},
		{"options", "name"},         {"etapi_tokens", "etapiTokenId"},
	};
	/* NON-NLS-END */
	const char *key = NULL;
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!strcmp(keys[i][0], name))
			key = keys[i][1];
	if (!key)
		return NULL;
	char sql[128];
	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s = ?", name, key); /* NON-NLS */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK)
		return NULL;
	sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
	char *json = NULL;
	size_t size = 0;
	if (sqlite3_step(st) == SQLITE_ROW) {
		FILE *f = open_memstream(&json, &size);
		if (f) {
			row_json(f, st, !strcmp(name, "blobs") ? "content" : NULL); /* NON-NLS */
			fclose(f);
		}
	}
	sqlite3_finalize(st);
	return json;
}

/* This phone's changes (edit.c: stamped with its instance) not pushed yet, PUSH_BATCH a
 * request (PUT /api/sync/update, as sync.ts' pushChanges()); then past the others,
 * pulled, which are not pushed back */
static const char *push(struct sync *s)
{
	for (;;) {
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2(s->db,
		                       "SELECT id, entityName, entityId, hash, isErased, changeId, componentId, instanceId," /* NON-NLS */
		                       " isSynced, utcDateChanged FROM entity_changes WHERE isSynced = 1 AND instanceId = ?" /* NON-NLS */
		                       " AND id > ? ORDER BY id LIMIT " PUSH_BATCH, /* NON-NLS */
		                       -1, &st, NULL) != SQLITE_OK)
			return N_("The notes could not be saved on the phone.");
		sqlite3_bind_text(st, 1, s->instance, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 2, option_ll(s->db, "lastSyncedPush"));
		char *body = NULL;
		size_t size = 0;
		FILE *f = open_memstream(&body, &size);
		if (!f) {
			sqlite3_finalize(st);
			return N_("The notes could not be saved on the phone.");
		}
		fputs("{\"entities\":[", f); /* NON-NLS */
		unsigned n = 0;
		long long last = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(st, 1), *id = (const char *)sqlite3_column_text(st, 2);
			bool erased = sqlite3_column_int(st, 4);
			last = sqlite3_column_int64(st, 0);
			fprintf(f, "%s{\"entityChange\":", n++ ? "," : ""); /* NON-NLS */
			row_json(f, st, NULL);
			/* Its row, unless erased; gone since (deleted with its blob), the change only */
			char *entity = erased ? NULL : entity_json(s, name ? name : "", id ? id : "");
			if (entity)
				fprintf(f, ",\"entity\":%s", entity); /* NON-NLS */
			free(entity);
			fputc('}', f);
		}
		sqlite3_finalize(st);
		fputs("],\"instanceId\":", f); /* NON-NLS */
		json_string(f, s->instance);
		fputc('}', f);
		fclose(f);
		if (!n) {
			free(body);
			break;
		}
		char marker[16], path[96];
		tr_random_id(marker, 10);
		snprintf(path, sizeof(path), "/api/sync/update?logMarkerId=%s", marker); /* NON-NLS */
		struct json *root;
		const char *error = call(s, "PUT", path, body, &root);
		json_free(root);
		free(body);
		if (error)
			return error;
		fprintf(stderr, "trilium: pushed %u changes\n", n);
		if (!option_ll_set(s->db, "lastSyncedPush", last))
			return N_("The notes could not be saved on the phone.");
	}
	/* Nothing of its own left: past what was pulled */
	long long last = tr_db_int(s->db, "SELECT COALESCE(MAX(id), 0) FROM entity_changes WHERE isSynced = 1", 0); /* NON-NLS */
	if (last > option_ll(s->db, "lastSyncedPush") && !option_ll_set(s->db, "lastSyncedPush", last))
		return N_("The notes could not be saved on the phone.");
	return NULL;
}

static unsigned count(const struct json *array)
{
	unsigned n = 0;
	for (const struct json *c = array ? array->child : NULL; c; c = c->next)
		n++;
	return n;
}

/* One page applied with the cursor moved, all or nothing */
static const char *page_apply(struct sync *s, const struct json *changes, long long cursor)
{
	const char *error = NULL;
	if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
		return N_("The notes could not be saved on the phone.");
	long long applied = 0;
	bool ok = tr_apply(s->applier, changes, s->server, &applied, &error);
	if (ok && (!option_ll_set(s->db, "lastSyncedPull", cursor) ||
	           sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)) {
		ok = false;
		error = N_("The notes could not be saved on the phone.");
	}
	if (!ok) {
		fprintf(stderr, "trilium: page not kept: %s\n", sqlite3_errmsg(s->db));
		sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
		return error;
	}
	s->applied += applied;
	return NULL;
}

static void page_path(const struct sync *s, long long cursor, char *path, size_t size)
{
	char marker[12];
	tr_random_id(marker, 10);
	snprintf(path, size, "/api/sync/changed?instanceId=%s&lastEntityChangeId=%lld&logMarkerId=%s", s->instance,
	         cursor, marker);
}

/* The next page asked for while this one is applied, on a thread of its own (the
 * connection is not used meanwhile), as sync.ts' pullChanges() prefetches it: the
 * network and the database each wait for the other no more */
struct prefetch {
	struct tr_http *http;
	char path[256];
	struct answer answer;
	pthread_t thread;
	bool started;
};

static void *prefetch_run(void *arg)
{
	struct prefetch *p = arg;
	request(p->http, "GET", p->path, NULL, &p->answer);
	return NULL;
}

static const char *pull(struct sync *s)
{
	long long total = -1;
	long long cursor = option_ll(s->db, "lastSyncedPull");
	char path[256];
	page_path(s, cursor, path, sizeof(path));
	struct answer page;
	request(&s->http, "GET", path, NULL, &page);
	for (;;) {
		if (page.error) {
			s->status = page.status;
			return page.error;
		}
		const struct json *changes = json_get(page.root, "entityChanges");
		long long next = json_int(json_get(page.root, "lastEntityChangeId"), cursor);
		long long outstanding = json_int(json_get(page.root, "outstandingPullCount"), 0);
		if (!changes || changes->kind != JSON_ARRAY) {
			json_free(page.root);
			return N_("The server's answer could not be read.");
		}
		unsigned n = count(changes);
		if (total < 0) {
			/* All there is: what earlier syncs pulled (the first one's not done yet), this, the rest */
			long long earlier = s->initialized ? 0 : tr_db_int(s->db, "SELECT COUNT(*) FROM entity_changes WHERE isSynced = 1", 0); /* NON-NLS */
			total = earlier + n + outstanding;
			step_set(TR_SYNC_PULLING);
		}
		if (!n && next == cursor) {
			json_free(page.root);
			break;
		}

		/* The next while this one is applied, unless this was the last */
		struct prefetch p = {.http = &s->http};
		if (n) {
			page_path(s, next, p.path, sizeof(p.path));
			p.started = pthread_create(&p.thread, NULL, prefetch_run, &p) == 0;
		}
		long start = now_ms();
		const char *error = page_apply(s, changes, next);
		long apply_ms = now_ms() - start;
		json_free(page.root);
		if (!error)
			progress_set(total - outstanding, total);
		if (p.started)
			pthread_join(p.thread, NULL);
		else if (n && !error)
			request(&s->http, "GET", p.path, NULL, &p.answer);
		fprintf(stderr, "trilium: pulled %u changes (%zu kB) in %ld ms, parsed in %ld ms, applied in %ld ms\n", n,
		        page.bytes / 1024, page.fetch_ms, page.parse_ms, apply_ms);
		if (error) {
			json_free(p.answer.root);
			return error;
		}
		if (!n)
			break;
		cursor = next;
		page = p.answer;
	}
	return NULL;
}

/* A sector's hash, as the server gives it: "" if it has none */
static const char *remote_hash(const struct json *hashes, const char *name, char sector)
{
	char key[2] = {sector, '\0'};
	return json_str(json_get(json_get(hashes, name), key), "");
}

struct sector {
	char name[32];
	char sector;
	char hash[32]; /* base64 of SHA-1: 28 */
};

/* content_hash.ts' getEntityHashes(): per table and sector, the changes' hashes and
 * isErased in the IDs' order, SHA-1 in base64; *n of them */
static struct sector *local_hashes(sqlite3 *db, unsigned *n)
{
	*n = 0;
	struct sector *sectors = calloc(MAX_SECTORS, sizeof(*sectors));
	sqlite3_stmt *st;
	if (!sectors || sqlite3_prepare_v2(db,
	                                   "SELECT entityName, entityId, hash, isErased FROM entity_changes WHERE isSynced = 1"
	                                   " AND entityName != 'note_reordering' ORDER BY entityName, entityId",
	                                   -1, &st, NULL) != SQLITE_OK) {
		free(sectors);
		return NULL;
	}
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	struct sector *cur = NULL;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(st, 0);
		const char *id = (const char *)sqlite3_column_text(st, 1);
		const char *hash = (const char *)sqlite3_column_text(st, 2);
		if (!name || !id || !id[0] || !hash)
			continue;
		if (!cur || strcmp(cur->name, name) || cur->sector != id[0]) {
			unsigned char digest[EVP_MAX_MD_SIZE];
			unsigned len;
			if (cur) {
				EVP_DigestFinal_ex(md, digest, &len);
				EVP_EncodeBlock((unsigned char *)cur->hash, digest, (int)len);
			}
			if (*n == MAX_SECTORS)
				break;
			cur = &sectors[(*n)++];
			snprintf(cur->name, sizeof(cur->name), "%s", name);
			cur->sector = id[0];
			EVP_DigestInit_ex(md, EVP_sha1(), NULL);
		}
		EVP_DigestUpdate(md, hash, strlen(hash));
		EVP_DigestUpdate(md, sqlite3_column_int(st, 3) ? "1" : "0", 1);
	}
	if (cur) {
		unsigned char digest[EVP_MAX_MD_SIZE];
		unsigned len;
		EVP_DigestFinal_ex(md, digest, &len);
		EVP_EncodeBlock((unsigned char *)cur->hash, digest, (int)len);
	}
	EVP_MD_CTX_free(md);
	sqlite3_finalize(st);
	return sectors;
}

/* One more attempt at a sector; false once it has had them all */
static bool attempt(struct sync *s, const char *name, char sector)
{
	struct attempt *a = NULL;
	for (unsigned i = 0; i < s->attempt_count && !a; i++)
		if (!strcmp(s->attempts[i].name, name) && s->attempts[i].sector == sector)
			a = &s->attempts[i];
	if (!a && s->attempt_count < MAX_SECTORS) {
		a = &s->attempts[s->attempt_count++];
		snprintf(a->name, sizeof(a->name), "%s", name);
		a->sector = sector;
	}
	if (!a)
		return false;
	if (++a->count == MAX_SECTOR_ATTEMPTS) {
		fprintf(stderr, "trilium: %s sector %c still differs after %d attempts\n", name, sector, a->count);
		s->unresolvable = true;
	}
	return a->count < MAX_SECTOR_ATTEMPTS;
}

/* A sector to queue again (*queue grows), unless it had its attempts */
static void differs(struct sync *s, const char *name, char sector, char (*queue)[40], unsigned *queued)
{
	fprintf(stderr, "trilium: %s sector %c differs\n", name, sector);
	if (attempt(s, name, sector) && *queued < MAX_SECTORS)
		snprintf(queue[(*queued)++], 40, "%s/%c", name, sector);
}

/* sync.ts' checkContentHash(): *again when another round can achieve something */
static const char *check(struct sync *s, bool *again)
{
	*again = false;
	/* As Trilium: not while this phone's changes wait to be pushed, which the server's
	 * hashes do not have yet */
	if (first_local(s)) {
		fprintf(stderr, "trilium: this phone's changes wait to be pushed, the content not checked\n");
		return NULL;
	}
	struct json *root;
	const char *error = call(s, "GET", "/api/sync/check", NULL, &root);
	if (error)
		return error;
	const struct json *hashes = json_get(root, "entityHashes");
	if (!hashes || hashes->kind != JSON_OBJECT) {
		json_free(root);
		return N_("The server's answer could not be read.");
	}
	if (option_ll(s->db, "lastSyncedPull") < json_int(json_get(root, "maxEntityChangeId"), 0)) {
		json_free(root); /* more came meanwhile */
		*again = true;
		return NULL;
	}
	unsigned n;
	struct sector *local = local_hashes(s->db, &n);
	if (!local) {
		json_free(root);
		return N_("The notes could not be saved on the phone.");
	}
	char (*queue)[40] = calloc(MAX_SECTORS, sizeof(*queue));
	unsigned queued = 0;
	/* Ours against the server's, then the server's sectors of our tables we lack */
	for (unsigned i = 0; queue && i < n; i++)
		if (strcmp(local[i].hash, remote_hash(hashes, local[i].name, local[i].sector)))
			differs(s, local[i].name, local[i].sector, queue, &queued);
	for (unsigned i = 0; queue && i < n; i++) {
		if (i && !strcmp(local[i - 1].name, local[i].name))
			continue;
		const struct json *sectors = json_get(hashes, local[i].name);
		for (const struct json *r = sectors ? sectors->child : NULL; r; r = r->next) {
			bool ours = false;
			for (unsigned j = i; j < n && !strcmp(local[j].name, local[i].name) && !ours; j++)
				ours = local[j].sector == r->key[0];
			if (!ours && r->key[0])
				differs(s, local[i].name, r->key[0], queue, &queued);
		}
	}
	free(local);
	json_free(root);
	if (!queue)
		return N_("The notes could not be saved on the phone.");

	if (queued) {
		error = call(s, "POST", "/api/sync/check-entity-changes", NULL, &root);
		json_free(root);
	}
	for (unsigned i = 0; !error && i < queued; i++) {
		char path[80];
		snprintf(path, sizeof(path), "/api/sync/queue-sector/%s", queue[i]);
		error = call(s, "POST", path, NULL, &root);
		json_free(root);
	}
	free(queue);
	*again = queued > 0;
	return error;
}

static const char *finished(struct sync *s)
{
	struct json *root;
	const char *error = call(s, "POST", "/api/sync/finished", NULL, &root);
	json_free(root);
	return error;
}

static const char *run(struct sync *s)
{
	char host[600], initialized[8];
	tr_db_option(s->db, "syncServerHost", host, sizeof(host));
	tr_db_option(s->db, "initialized", initialized, sizeof(initialized));
	s->initialized = !strcmp(initialized, "true");
	if (!tr_http_init(&s->http, host))
		return N_("Cannot reach the server.");
	const char *error = NULL;
	bool again = true;
	for (int round = 1; !error && again; round++) {
		if (round > MAX_ROUNDS) {
			fprintf(stderr, "trilium: sync did not converge after %d rounds\n", MAX_ROUNDS);
			return N_("The notes on the phone and on the server still differ after syncing.");
		}
		if (!(error = login(s)) && !(error = push(s)) && !(error = pull(s)) && !(error = push(s)) &&
		    !(error = finished(s))) {
			step_set(TR_SYNC_CHECKING);
			error = check(s, &again);
		}
	}
	if (!error && s->unresolvable)
		error = N_("The notes on the phone and on the server still differ after syncing.");
	if (!error && !tr_db_option_set(s->db, "initialized", "true"))
		error = N_("The notes could not be saved on the phone.");
	return error;
}

static void *thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct sync *s = calloc(1, sizeof(*s));
		const char *error = N_("The database cannot be opened");
		bool pending = false;
		if (s) {
			s->db = tr_db_open();
			if (s->db && !tr_db_instance(s->db, s->instance))
				tr_random_id(s->instance, 12);
			s->applier = s->db ? tr_applier_new(s->db) : NULL;
			error = s->applier ? run(s) : error;
			pending = s->db && first_local(s);
			tr_http_free(&s->http);
			tr_applier_free(s->applier);
			sqlite3_close(s->db);
		}
		if (error)
			fprintf(stderr, "trilium: sync failed: %s (%ld)\n", error, s ? s->status : 0);
		else
			fprintf(stderr, "trilium: synced, %lld changes\n", s->applied);
		pthread_mutex_lock(&lock);
		/* Asked again meanwhile (a note saved): once more */
		bool more = rerun;
		rerun = false;
		if (more) {
			state.step = TR_SYNC_CONNECTING;
			state.pulled = state.total = 0;
		} else {
			state.step = error ? TR_SYNC_FAILED : TR_SYNC_DONE;
			state.error = error;
			state.status = s ? s->status : 0;
			state.pending = pending;
			running = false;
		}
		pthread_mutex_unlock(&lock);
		free(s);
		if (!more)
			return NULL;
	}
}

/* The CPU at its highest while a sync runs (cpu.c: at its lowest, applying a page of
 * changes took twice as long), back to its lowest when it ends, the app left or not.
 * From the UI's thread, as cpu.c wants it. */
static lv_timer_t *boost_timer;

static void boost_cb(lv_timer_t *t)
{
	pthread_mutex_lock(&lock);
	bool on = running;
	pthread_mutex_unlock(&lock);
	if (!on) {
		ui_cpu_boost(false);
		lv_timer_delete(t);
		boost_timer = NULL;
	}
}

void tr_sync_start(void)
{
	pthread_mutex_lock(&lock);
	if (running) {
		rerun = true;
		pthread_mutex_unlock(&lock);
		return;
	}
	ui_cpu_boost(true);
	if (!boost_timer)
		boost_timer = lv_timer_create(boost_cb, 500, NULL);
	bool pending = state.pending;
	memset(&state, 0, sizeof(state));
	state.pending = pending;
	state.step = TR_SYNC_CONNECTING;
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	running = pthread_create(&t, &attr, thread, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!running) {
		state.step = TR_SYNC_FAILED;
		state.error = N_("The notes could not be saved on the phone.");
	}
	pthread_mutex_unlock(&lock);
}

void tr_sync_get(struct tr_sync_state *out)
{
	pthread_mutex_lock(&lock);
	*out = state;
	pthread_mutex_unlock(&lock);
}

void tr_sync_changed(void)
{
	pthread_mutex_lock(&lock);
	state.pending = true;
	pthread_mutex_unlock(&lock);
	tr_sync_start();
}
