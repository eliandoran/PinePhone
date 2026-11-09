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
 * - trilium.c  the app's screen: the document, or setting up sync when it is not
 * - setup.c    setting up sync: the server's address and the password
 */

#ifndef TRILIUM_H
#define TRILIUM_H

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
/* Sync set up with the server at `host`: its seed's options (json.h's array of
 * {"name", "value", "isSynced"}), documentId and documentSecret among them, kept with
 * the address, all or none */
struct json;
bool tr_db_set_up(sqlite3 *db, const char *host, const struct json *options);

void tr_open(void);
/* The app's screen again: the document, or the form when sync is not set up */
void tr_show(void);
void tr_setup_create(lv_obj_t *body);

#endif
