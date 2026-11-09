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
 * - note.c     a note's content
 * - search.c   jump to note
 * - icons.c    note icons: Boxicons, as Trilium draws them
 * - edit.c     a text note edited in phone-ui's rich text editor, and saved
 * - html.c     a text note's HTML to the editor's document and back
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
#include "ui/rich_edit.h"

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
/* The same with `param` bound to its ? */
long long tr_db_int_param(sqlite3 *db, const char *sql, const char *param, long long fallback);
/* A note's content as a string (a text note's HTML), "" for none; NULL if it cannot be
 * read. To be freed. */
char *tr_note_html(sqlite3 *db, const char *note_id);
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
/* Now as its localNowDateTime(), local time with its offset: "2026-11-09 10:00:00.000+0200" */
void tr_local_now(char *buf, size_t size);
/* `len` random letters and digits, as Trilium's randomString() */
void tr_random_id(char *out, size_t len);
/* This phone's instance of the document (an option kept, not synced): what its own
 * changes are stamped with and the sync tells the server it is */
bool tr_db_instance(sqlite3 *db, char out[16]);

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
	bool pending;            /* this phone's changes not pushed yet */
	const char *error;       /* failed: N_() text, its %ld `status` */
	long status;
};
/* Starts it; when one runs, another after it */
void tr_sync_start(void);
/* A change made on the phone (a note saved): pending, a sync started to push it */
void tr_sync_changed(void);
/* The sync indicator in a screen's header (trilium.c): the sync's state as Trilium shows it,
 * a star while this phone's changes wait to be pushed; tapped, a sync */
lv_obj_t *tr_sync_indicator(lv_obj_t *header);
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
 * opening a screen of them; a line saying there are none. With its `title` (NULL: the
 * root), the note's content above them (tr_note_preview()). */
void tr_tree_fill(lv_obj_t *parent, sqlite3 *db, const char *note_id, const char *title);
/* A screen of a note with children: them, its content above */
void tr_tree_open(const char *note_id, const char *title);
/* Note icons (icons.c): Boxicons, its font and its icons by name (gen-icons.py) */
struct tr_boxicon {
	const char *name; /* "bx-folder" */
	uint32_t cp;      /* its character in the font */
};
extern const uint8_t tr_boxicons_ttf[];
extern const size_t tr_boxicons_ttf_size;
extern const struct tr_boxicon tr_boxicons[];
extern const size_t tr_boxicons_count;
/* The font a row's icon is drawn in (made when first asked, kept) */
const lv_font_t *tr_icon_font(void);
/* The same at `px` */
const lv_font_t *tr_icon_font_px(int px);
/* A Boxicons icon by its name ("bx-wifi"), its character UTF-8 ("" if it has none) */
void tr_icon(char icon[8], const char *name);
/* A note's icon as Trilium picks it: its #iconClass (`icon_class`, NULL: none), else
 * for what it is (a folder: has children); its character in tr_icon_font(), UTF-8 */
void tr_note_icon(char icon[8], const char *icon_class, const char *type, const char *mime, bool folder);
/* Jump to note (search.c): a screen finding notes by their title as typed */
void tr_jump_open(void);
/* A screen of the note's content (note.c), `title` its */
void tr_note_open(const char *note_id, const char *title);
/* The start of the note's content on a card in `parent`, which opens it all; none
 * when it has nothing to show */
void tr_note_preview(lv_obj_t *parent, sqlite3 *db, const char *note_id, const char *title);
/* A text note's HTML (CKEditor's) as the rich text editor's document (html.c), NULL when
 * it has what the document cannot hold (a link, a heading...); the document as HTML.
 * Both to be freed (rich_doc_free(), free()). */
struct rich_doc *tr_html_to_doc(const char *html);
char *tr_html_from_doc(const struct rich_doc *doc);
/* A screen editing the note (edit.c), `doc` its content, which it then owns; saved as
 * the keyboard goes and as it is left */
void tr_edit_open(const char *note_id, const char *title, struct rich_doc *doc);
/* How many saves of a note there were: a note's screen shows it again after one */
extern unsigned tr_note_saves;
void tr_setup_create(lv_obj_t *body);

#endif
