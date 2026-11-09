/*
 * Trilium (apps/trilium), the plugin app: trilium.so built for the host (TRILIUM_APP,
 * installed by each test) with the host's SQLite. Its database, a Trilium document.db:
 * created empty with Trilium's schema, kept once there, what it holds shown.
 */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define DB "/var/lib/trilium/document.db"

static bool copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
	char buf[65536];
	size_t n;
	bool ok = in && out;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if (in)
		fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	return ok;
}

/* As `just app trilium` installs it */
#define APP "/var/lib/apps/trilium"
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "") || !copy(TRILIUM_APP "/app.json", APP "/app.json") ||
	    !copy(TRILIUM_APP "/trilium.so", APP "/trilium.so"))
		TEST_FAIL("cannot install %s", TRILIUM_APP);
}

/* The first column of the first row `sql` gives in the app's database, "" if none */
static const char *query(const char *sql)
{
	static char value[256];
	value[0] = '\0';
	sqlite3 *db;
	if (sqlite3_open_v2(DB, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
				snprintf(value, sizeof(value), "%s", (const char *)sqlite3_column_text(st, 0));
			sqlite3_finalize(st);
		}
	}
	sqlite3_close(db);
	return value;
}

static bool execute(const char *sql)
{
	sqlite3 *db;
	bool ok = sqlite3_open_v2(DB, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
	          sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
	sqlite3_close(db);
	return ok;
}

static const char *option(const char *name)
{
	char sql[200];
	snprintf(sql, sizeof(sql), "SELECT value FROM options WHERE name = '%s'", name);
	return query(sql);
}

static const char *table(const char *name)
{
	char sql[200];
	snprintf(sql, sizeof(sql), "SELECT name FROM sqlite_master WHERE type = 'table' AND name = '%s'", name);
	return query(sql);
}

TEST(creates_an_empty_database)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("No notes yet"));
	/* Trilium's tables */
	CHECK_STR(table("notes"), "notes");
	CHECK_STR(table("branches"), "branches");
	CHECK_STR(table("attributes"), "attributes");
	CHECK_STR(table("blobs"), "blobs");
	CHECK_STR(table("attachments"), "attachments");
	CHECK_STR(table("revisions"), "revisions");
	CHECK_STR(table("entity_changes"), "entity_changes");
	CHECK_STR(table("options"), "options");
	/* Not synced yet, at Trilium's database version, as a client set up for sync starts */
	CHECK_STR(option("initialized"), "false");
	CHECK_STR(option("dbVersion"), "242");
	CHECK_STR(option("lastSyncedPull"), "0");
	CHECK_STR(option("lastSyncedPush"), "0");
	CHECK_STR(option("syncServerHost"), "");
	CHECK_STR(query("SELECT isSynced FROM options WHERE name = 'dbVersion'"), "0");
	CHECK_STR(query("SELECT COUNT(*) FROM notes"), "0");
	CHECK_STR(query("PRAGMA journal_mode"), "wal");
}

TEST(keeps_the_database)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_back());
	CHECK(execute("UPDATE options SET value = 'https://sync.example' WHERE name = 'syncServerHost';"
	              "INSERT INTO notes (noteId, title, type, mime, blobId, dateCreated, dateModified,"
	              " utcDateCreated, utcDateModified) VALUES ('root', 'root', 'text', 'text/html', 'b1',"
	              " '2026-11-09 10:00:00.000+0200', '2026-11-09 10:00:00.000+0200',"
	              " '2026-11-09 08:00:00.000Z', '2026-11-09 08:00:00.000Z')"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("1 note"));
	CHECK_STR(option("syncServerHost"), "https://sync.example");
}

TEST(deleted_notes_not_counted)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_back());
	CHECK(execute("INSERT INTO notes (noteId, title, type, mime, blobId, isDeleted, dateCreated, dateModified,"
	              " utcDateCreated, utcDateModified) VALUES"
	              " ('a', 'A', 'text', 'text/html', 'b1', 0, '', '', '', ''),"
	              " ('b', 'B', 'text', 'text/html', 'b2', 0, '', '', '', ''),"
	              " ('c', 'C', 'text', 'text/html', 'b3', 1, '', '', '', '')"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("2 notes"));
}

TEST(database_cannot_be_opened)
{
	/* Its folder a file */
	CHECK(ui_test_write("/var/lib/trilium", "not a folder"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("The database cannot be opened"));
}

TEST(not_a_trilium_database)
{
	CHECK(ui_test_write(DB, "this is not SQLite, it is long enough to be read as a header of one........"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("The database cannot be opened"));
	/* Left as it was */
	CHECK_STR(table("notes"), "");
}
