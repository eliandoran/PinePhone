/*
 * Trilium: a client of Trilium Notes (https://github.com/TriliumNext/Trilium) that
 * keeps a copy of a document and syncs it with a Trilium server or sync relay.
 * The copy is a Trilium document.db in DATA_DIR: Trilium's own schema (schema.sql,
 * copied from TriliumNext's packages/trilium-core/src/assets/schema.sql) and options,
 * so what sync sends goes in as rows, as Trilium's sync_update.ts puts them.
 *
 * A plugin app (plugin.c, `just app trilium`), carrying the image's SQLite in lib/.
 * Shared between its files (tr_: apart from phone-ui's symbols, which it sees):
 *
 * - plugin.c   what phone-ui looks for in trilium.so
 * - db.c       the database: opened, created empty when there is none
 * - trilium.c  the app's screen: the notes, the sync's progress, or setting up
 * - tree.c     the notes, as Trilium's tree has them
 * - setup.c    setting up sync: the server's address and the password
 * - sync.c     the sync, on a thread of its own: Trilium's sync.ts
 * - apply.c    what a sync pulls, into the database: Trilium's sync_update.ts
 * - http.c     the sync's requests
 */

#ifndef TRILIUM_H
#define TRILIUM_H

#include <curl/curl.h>
#include <sqlite3.h>
#include <stdbool.h>

#include "i18n.h"
#include "ui.h"

#define DATA_DIR "/var/lib/trilium"
#define DB_PATH DATA_DIR "/document.db"

/* Trilium's versions schema.sql goes with (TriliumNext v0.107.0): its database's,
 * the last migration's (the dbVersion option), and its sync protocol's */
#define TR_DB_VERSION 242
#define TR_SYNC_VERSION 41

/* schema.sql, made C by gen-schema.sh */
extern const char tr_schema[];

/* The database, created with Trilium's schema and the options of a client not
 * synced yet when it has no tables; NULL if it cannot be opened or is not
 * Trilium's (logged). Closed with sqlite3_close(). */
sqlite3 *tr_db_open(void);
/* The first column of the first row of `sql`, `fallback` without one */
long long tr_db_int(sqlite3 *db, const char *sql, long long fallback);
/* An option's value, "" if it has none */
void tr_db_option(sqlite3 *db, const char *name, char *buf, size_t size);
/* An option set (not synced), false if it could not be */
bool tr_db_option_set(sqlite3 *db, const char *name, const char *value);
/* Sync set up with the server at `host`: its seed's options (json.h's array of
 * {"name", "value", "isSynced"}), documentId and documentSecret among them, kept with
 * the address, all or none */
struct json;
bool tr_db_set_up(sqlite3 *db, const char *host, const struct json *options);
/* Now as Trilium's dateUtils.utcNowDateTime(): "2026-11-09 08:00:00.000Z" */
void tr_utc_now(char *buf, size_t size);

/* The sync's requests to the server at `host` (http.c) */
struct tr_http {
	CURL *curl;
	char host[600];
	char cookies[4096]; /* "name=value; name=value", the session's */
};
bool tr_http_init(struct tr_http *h, const char *host);
void tr_http_free(struct tr_http *h);
/* `method` to the host's `path` with a JSON `body` (NULL: none): the HTTP status, 0 if
 * the server could not be reached; *out the answer (free() it), NULL then */
long tr_http_request(struct tr_http *h, const char *method, const char *path, const char *body, char **out);

/* The sync (sync.c): one at a time, on its thread; its state read from the UI's */
enum tr_step {
	TR_SYNC_IDLE,
	TR_SYNC_CONNECTING,
	TR_SYNC_PULLING,
	TR_SYNC_CHECKING,
	TR_SYNC_DONE,
	TR_SYNC_FAILED,
};
struct tr_sync_state {
	enum tr_step step;
	long long pulled, total; /* changes: pulled of all there was to pull */
	const char *error;       /* failed: N_() text, its %ld `status` */
	long status;
};
/* Starts it unless one runs */
void tr_sync_start(void);
void tr_sync_get(struct tr_sync_state *out);

/* Applying pulled changes to `db` (apply.c), what they need kept for a sync: freed
 * before the database is closed */
struct tr_applier;
struct tr_applier *tr_applier_new(sqlite3 *db);
void tr_applier_free(struct tr_applier *a);
/* The pulled records (json.h's array of {"entityChange", "entity"}) into the database,
 * as Trilium's sync_update.ts does, in the caller's transaction; their changes stamped
 * with the server's `instance`. False with *error (an N_() text) if one could not be;
 * *applied counts those that were. */
bool tr_apply(struct tr_applier *a, const struct json *records, const char *instance, long long *applied,
              const char **error);

void tr_open(void);
/* The app's screen again: the notes, the sync's progress, or the form when sync is
 * not set up; a sync started when the document has not had one */
void tr_show(void);
/* The notes under `note_id` in `parent` (tree.c): a card of rows, those with children
 * opening a screen of them; a line saying there are none */
void tr_tree_fill(lv_obj_t *parent, sqlite3 *db, const char *note_id);
void tr_setup_create(lv_obj_t *body);

#endif
