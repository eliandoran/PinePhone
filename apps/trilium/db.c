/*
 * The database, see trilium.h. Created as Trilium creates one for a client set up
 * to sync from a server (sql_init.ts' createDatabaseForSync()): its schema, then
 * the options that are not synced (options_init.ts' initNotSyncedOptions()), not
 * initialized until a first sync has converged. The notes, the synced options and
 * root itself all come with that sync.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "json.h"
#include "trilium.h"

void tr_utc_now(char *buf, size_t size)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	gmtime_r(&ts.tv_sec, &tm);
	size_t n = strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
	snprintf(buf + n, size - n, ".%03dZ", (int)(ts.tv_nsec / 1000000) % 1000);
}

long long tr_db_int(sqlite3 *db, const char *sql, long long fallback)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return fallback;
	long long value = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : fallback;
	sqlite3_finalize(st);
	return value;
}

static bool option_create(sqlite3 *db, const char *name, const char *value, const char *now)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "INSERT INTO options (name, value, isSynced, utcDateModified) VALUES (?, ?, 0, ?)",
	                       -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, now, -1, SQLITE_STATIC);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

/* Trilium's schema and the options of a client that has not synced yet, in one transaction */
static bool create(sqlite3 *db)
{
	char now[32], version[16];
	tr_utc_now(now, sizeof(now));
	snprintf(version, sizeof(version), "%d", TR_DB_VERSION);
	const char *options[][2] = {
		{"openNoteContexts", "[{\"notePath\":\"root\",\"active\":true}]"},
		{"lastDailyBackupDate", now},
		{"lastWeeklyBackupDate", now},
		{"lastMonthlyBackupDate", now},
		{"dbVersion", version},
		{"initialized", "false"},
		{"lastSyncedPull", "0"},
		{"lastSyncedPush", "0"},
		{"theme", "next"},
		{"syncServerHost", ""},
		{"syncServerTimeout", "120"},
		{"syncProxy", ""},
		{"syncIncomplete", "false"},
		{"syncMaxBlobContentSize", "0"},
	};
	bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
	          sqlite3_exec(db, tr_schema, NULL, NULL, NULL) == SQLITE_OK;
	for (size_t i = 0; ok && i < sizeof(options) / sizeof(*options); i++)
		ok = option_create(db, options[i][0], options[i][1], now);
	if (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
		return true;
	fprintf(stderr, "trilium: cannot create %s: %s\n", DB_PATH, sqlite3_errmsg(db));
	sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
	return false;
}

sqlite3 *tr_db_open(void)
{
	if (mkdir(DATA_DIR, 0755) && errno != EEXIST)
		fprintf(stderr, "trilium: cannot make %s: %m\n", DATA_DIR);
	sqlite3 *db;
	if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot open %s: %s\n", DB_PATH, db ? sqlite3_errmsg(db) : "no memory");
		sqlite3_close(db);
		return NULL;
	}
	/* The UI and the sync each have a connection: one waits while the other writes */
	sqlite3_busy_timeout(db, 5000);
	/* Its first read: a file that is not SQLite fails here */
	long long tables = tr_db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table'", -1); /* NON-NLS */
	bool ok;
	if (tables < 0) {
		fprintf(stderr, "trilium: cannot read %s: %s\n", DB_PATH, sqlite3_errmsg(db));
		ok = false;
	} else if (tables == 0) {
		ok = create(db);
	} else {
		ok = tr_db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN ('notes', 'options')", /* NON-NLS */
		               0) == 2;
		if (!ok)
			fprintf(stderr, "trilium: %s is not a Trilium database\n", DB_PATH);
	}
	/* As Trilium's server and desktop app keep it */
	if (ok && sqlite3_exec(db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: %s: %s\n", DB_PATH, sqlite3_errmsg(db));
		ok = false;
	}
	if (!ok) {
		sqlite3_close(db);
		return NULL;
	}
	return db;
}

void tr_db_option(sqlite3 *db, const char *name, char *buf, size_t size)
{
	buf[0] = '\0';
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT value FROM options WHERE name = ?", -1, &st, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
		snprintf(buf, size, "%s", (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
}

/* As Trilium's createDatabaseForSync() saves them: as the server has them */
static bool option_put(sqlite3 *db, const char *name, const char *value, bool synced, const char *date)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO options (name, value, isSynced, utcDateModified) VALUES (?, ?, ?, ?)",
	                       -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 3, synced);
	sqlite3_bind_text(st, 4, date, -1, SQLITE_STATIC);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

bool tr_db_set_up(sqlite3 *db, const char *host, const struct json *options)
{
	char now[32];
	tr_utc_now(now, sizeof(now));
	bool id = false, secret = false;
	bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
	          option_put(db, "syncServerHost", host, false, now);
	for (const struct json *o = options ? options->child : NULL; ok && o; o = o->next) {
		const char *name = json_str(json_get(o, "name"), "");
		const char *value = json_str(json_get(o, "value"), "");
		if (!name[0])
			continue;
		id |= !strcmp(name, "documentId") && value[0];
		secret |= !strcmp(name, "documentSecret") && value[0];
		const struct json *synced = json_get(o, "isSynced"); /* true or 1 */
		ok = option_put(db, name, value, synced && synced->num != 0,
		                json_str(json_get(o, "utcDateModified"), now));
	}
	if (ok && id && secret && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
		return true;
	fprintf(stderr, "trilium: sync not set up: %s\n", ok ? "no documentId or documentSecret" : sqlite3_errmsg(db));
	sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
	return false;
}

bool tr_db_option_set(sqlite3 *db, const char *name, const char *value)
{
	char now[32];
	tr_utc_now(now, sizeof(now));
	return option_put(db, name, value, false, now);
}
