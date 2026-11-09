/*
 * Trilium (apps/trilium), the plugin app: trilium.so built for the host (TRILIUM_APP,
 * installed by each test) with the host's SQLite. Its database, a Trilium document.db:
 * created empty with Trilium's schema, kept once there, what it holds shown. Setting up
 * sync: the server's address and the password, the document's credentials asked of the
 * server (fake_curl.c) as Trilium asks them, kept in the database.
 */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define DB "/var/lib/trilium/document.db"
#define RELAY "https://relay.example/t/doc1"
#define SEED RELAY "/api/setup/sync-seed"
#define JSON "application/json"
/* What a Trilium server answers (a relay leaves out isSynced and the date) */
#define SEED_OK "{\"options\":[{\"name\":\"documentId\",\"value\":\"D0c1d\",\"isSynced\":false," \
	"\"utcDateModified\":\"2026-01-02 03:04:05.678Z\"},{\"name\":\"documentSecret\",\"value\":\"s3cr3t\"," \
	"\"isSynced\":false,\"utcDateModified\":\"2026-01-02 03:04:05.678Z\"}],\"syncVersion\":41}"

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


/* Sync set up as the form leaves it, without asking a server */
#define SET_UP "UPDATE options SET value = '" RELAY "' WHERE name = 'syncServerHost';" \
	"INSERT INTO options (name, value, isSynced, utcDateModified) VALUES" \
	" ('documentId', 'D0c1d', 0, ''), ('documentSecret', 's3cr3t', 0, '');"

/* The address and the password typed, Enter */
static bool connect(const char *address, const char *password)
{
	return ui_test_tap_field("Server address") && ui_test_type(address) && ui_test_tap_field("Password") &&
	       ui_test_type(password) && ui_test_type("\n");
}

static void net_wait(void)
{
	ui_test_wait(300);
	ui_test_settle();
}

TEST(creates_an_empty_database)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("Sync with Trilium"));
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
	CHECK(execute(SET_UP "INSERT INTO notes (noteId, title, type, mime, blobId, dateCreated, dateModified,"
	              " utcDateCreated, utcDateModified) VALUES ('root', 'root', 'text', 'text/html', 'b1',"
	              " '2026-11-09 10:00:00.000+0200', '2026-11-09 10:00:00.000+0200',"
	              " '2026-11-09 08:00:00.000Z', '2026-11-09 08:00:00.000Z')"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("1 note"));
	CHECK_STR(option("syncServerHost"), RELAY);
}

TEST(deleted_notes_not_counted)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_back());
	CHECK(execute(SET_UP "INSERT INTO notes (noteId, title, type, mime, blobId, isDeleted, dateCreated,"
	              " dateModified, utcDateCreated, utcDateModified) VALUES"
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
	CHECK(!ui_test_shows("Sync with Trilium"));
}

TEST(not_a_trilium_database)
{
	CHECK(ui_test_write(DB, "this is not SQLite, it is long enough to be read as a header of one........"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("The database cannot be opened"));
	/* Left as it was */
	CHECK_STR(table("notes"), "");
}

TEST(asks_for_the_server)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("Sync with Trilium"));
	CHECK(ui_test_tap_field("Server address"));
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_shows("Connect"));
	/* Asked for only when the server wants it */
	CHECK(!ui_test_shows("Two-factor code"));
	CHECK_INT((int)ui_test_request_count(), 0);
}

/* The document's credentials asked of the server with the password as Trilium sends it
 * (trilium-cred: base64 of "dummy:" and the password), kept; then the document's screen */
TEST(connects)
{
	ui_test_serve(SEED, 200, JSON, SEED_OK);
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	const struct ui_test_request *r = ui_test_requested(SEED);
	CHECK(r);
	if (!r)
		return;
	CHECK_STR(r->method, "GET");
	CHECK(strstr(r->headers, "trilium-cred: ZHVtbXk6aHVudGVyMg==\n"));
	CHECK(!strstr(r->headers, "trilium-totp"));
	CHECK(ui_test_shows("No notes yet"));
	CHECK(ui_test_shows_part("relay.example"));
	CHECK(!ui_test_shows("Sync with Trilium"));
	CHECK_STR(option("syncServerHost"), RELAY);
	CHECK_STR(option("documentId"), "D0c1d");
	CHECK_STR(option("documentSecret"), "s3cr3t");
	CHECK_STR(query("SELECT isSynced FROM options WHERE name = 'documentSecret'"), "0");
	/* The password is not kept */
	CHECK_STR(query("SELECT COUNT(*) FROM options WHERE value LIKE '%hunter2%'"), "0");
	CHECK_STR(option("initialized"), "false");

	/* Opened again, set up */
	CHECK(ui_test_back());
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("No notes yet"));
}

/* A relay's answer: names and values only */
TEST(connects_to_a_relay)
{
	ui_test_serve(SEED, 200, JSON,
	              "{\"options\":[{\"name\":\"documentId\",\"value\":\"D0c1d\"},"
	              "{\"name\":\"documentSecret\",\"value\":\"s3cr3t\"}],\"syncVersion\":41}");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, ""));
	net_wait();
	CHECK(ui_test_shows("No notes yet"));
	CHECK_STR(option("documentSecret"), "s3cr3t");
	const struct ui_test_request *r = ui_test_requested(SEED);
	CHECK(r && strstr(r->headers, "trilium-cred: ZHVtbXk6\n"));
}

/* Typed without https://, with spaces and a slash at its end */
TEST(address_tidied)
{
	ui_test_serve(SEED, 200, JSON, SEED_OK);
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(" relay.example/t/doc1/ ", "hunter2"));
	net_wait();
	CHECK(ui_test_requested(SEED));
	CHECK_STR(option("syncServerHost"), RELAY);
}

/* Plain HTTP when typed so (a server at home) */
TEST(plain_http_kept)
{
	ui_test_serve("http://192.168.0.5:8080/api/setup/sync-seed", 200, JSON, SEED_OK);
	CHECK(ui_test_open("Trilium"));
	CHECK(connect("http://192.168.0.5:8080", "hunter2"));
	net_wait();
	CHECK_STR(option("syncServerHost"), "http://192.168.0.5:8080");
}

TEST(address_missing)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_type("hunter2\n"));
	CHECK(ui_test_shows("Enter the server's address."));
	CHECK_INT((int)ui_test_request_count(), 0);
}

TEST(wrong_password)
{
	ui_test_serve(SEED, 401, "text/plain", "Incorrect password");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter3"));
	net_wait();
	CHECK(ui_test_shows("Wrong password."));
	CHECK(ui_test_shows("Sync with Trilium"));
	CHECK_STR(option("syncServerHost"), "");
	CHECK_STR(option("documentSecret"), "");
	/* Tried again */
	ui_test_serve(SEED, 200, JSON, SEED_OK);
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_type("\b\b\b\b\b\b\bhunter2\n"));
	net_wait();
	CHECK(ui_test_shows("No notes yet"));
}

/* The server wants a second factor: its field shown, the code sent as trilium-totp */
TEST(two_factor_code)
{
	ui_test_serve(SEED, 401, "text/plain", "TOTP token is required");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("Enter the code from your authenticator app."));
	CHECK(ui_test_shows("Sync with Trilium"));

	ui_test_serve(SEED, 401, "text/plain", "Incorrect TOTP token");
	CHECK(ui_test_tap_field("Two-factor code"));
	CHECK(ui_test_type("111111\n"));
	net_wait();
	CHECK(ui_test_shows("Wrong two-factor code."));

	ui_test_serve(SEED, 200, JSON, SEED_OK);
	CHECK(ui_test_tap_field("Two-factor code"));
	CHECK(ui_test_type("\b\b\b\b\b\b123456\n"));
	net_wait();
	const struct ui_test_request *r = ui_test_requested(SEED);
	CHECK(r && strstr(r->headers, "trilium-totp: 123456\n"));
	CHECK(r && strstr(r->headers, "trilium-cred: ZHVtbXk6aHVudGVyMg==\n"));
	CHECK(ui_test_shows("No notes yet"));
}

TEST(server_unreachable)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(connect("https://nowhere.example", "hunter2"));
	net_wait();
	CHECK(ui_test_shows("Cannot reach the server."));
	CHECK_STR(option("syncServerHost"), "");
}

TEST(not_a_trilium_server)
{
	ui_test_serve(SEED, 200, "text/html", "<html><body>Welcome to nginx!</body></html>");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("No Trilium server at this address."));

	ui_test_serve(SEED, 404, "text/html", "<html>Not found</html>");
	CHECK(ui_test_tap_text("Connect"));
	net_wait();
	CHECK(ui_test_shows("No Trilium server at this address."));
	CHECK_STR(option("syncServerHost"), "");
}

/* A relay no computer synced to yet, or a server not set up */
TEST(nothing_to_sync_yet)
{
	ui_test_serve(SEED, 400, JSON, "{\"message\":\"Not initialized\"}");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("There are no notes on this server yet: sync to it from Trilium on a computer first."));
	CHECK_STR(option("syncServerHost"), "");
}

TEST(other_sync_version)
{
	ui_test_serve(SEED, 200, JSON,
	              "{\"options\":[{\"name\":\"documentId\",\"value\":\"D0c1d\"},"
	              "{\"name\":\"documentSecret\",\"value\":\"s3cr3t\"}],\"syncVersion\":42}");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("The server runs another version of Trilium (sync version 42, this app needs 41)."));
	CHECK_STR(option("syncServerHost"), "");
}

TEST(too_many_attempts)
{
	ui_test_serve(SEED, 429, "text/plain", "Too many requests");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("Too many attempts. Try again in a few minutes."));
}

/* The server's errors otherwise: its status */
TEST(server_error)
{
	ui_test_serve(SEED, 500, JSON, "{\"message\":\"boom\"}");
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	net_wait();
	CHECK(ui_test_shows("The server answered with an error (500)."));
}

/* Left while connecting: nothing kept, nothing crashes when the answer would come */
TEST(left_while_connecting)
{
	ui_test_serve(SEED, 200, JSON, SEED_OK);
	ui_test_network_hold(true);
	CHECK(ui_test_open("Trilium"));
	CHECK(connect(RELAY, "hunter2"));
	ui_test_wait(100);
	CHECK(ui_test_shows("Wait..."));
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	net_wait();
	CHECK_STR(option("syncServerHost"), "");
}
