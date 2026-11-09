/*
 * Trilium (apps/trilium), the plugin app: trilium.so built for the host (TRILIUM_APP,
 * installed by each test) with the host's SQLite. Its database, a Trilium document.db:
 * created empty with Trilium's schema, kept once there, its notes listed as Trilium's
 * tree has them (drilled into as Files' folders), a leaf's content (a text note's HTML
 * as a page, code in monospace), what it holds counted. Setting up
 * sync: the server's address and the password, the document's credentials asked of the
 * server (fake_curl.c) as Trilium asks them, kept in the database.
 * Syncing: a Trilium server served by the test (server()), the changes pulled into the
 * database, the content hashes compared, the progress shown, failures said.
 */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "test.h"
#include "ui_symbols.h"
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

/* The first sync done before: the document's screen */
#define INITIALIZED "UPDATE options SET value = 'true' WHERE name = 'initialized';"

/*
 * A Trilium server or relay for the sync (sync.ts' requests): login with the document's
 * secret (a session cookie), the changes in pages of `per_page`, finished, the content
 * hashes of what it holds, queue-sector. Its changes are numbered from 1.
 */
#define LOGIN RELAY "/api/login/sync"
#define CHANGED RELAY "/api/sync/changed?*"
#define FINISHED RELAY "/api/sync/finished"
#define CHECK_HASHES RELAY "/api/sync/check"
#define QUEUE RELAY "/api/sync/queue-sector/*"
#define SERVER_ID "srv0instance"
#define COOKIE "trilium_jwt=eyJ0eXAi.x.y"
#define WAIT 5000

struct change {
	const char *name, *id;
	bool erased;
	const char *date;   /* utcDateChanged */
	const char *entity; /* its row as JSON, NULL: none */
};

static const struct change *server_changes;
static unsigned server_count;

/* Its entity change's hash: anything, the same on both sides */
static void change_hash(const struct change *c, char *out, size_t size)
{
	snprintf(out, size, "h%s%s", c->name, c->id);
}

static void base64_sha1(const char *text, char *out, size_t size)
{
	unsigned char digest[SHA_DIGEST_LENGTH];
	SHA1((const unsigned char *)text, strlen(text), digest);
	EVP_EncodeBlock((unsigned char *)out, digest, sizeof(digest));
	(void)size;
}

static int change_cmp(const void *a, const void *b)
{
	return strcmp((*(const struct change *const *)a)->id, (*(const struct change *const *)b)->id);
}

/* As Trilium's content_hash.ts: per table and first character of the ID, the entity
 * changes' hashes and isErased in the IDs' order, SHA-1 in base64. `wrong`: that
 * table's sector `wrong_sector` differs */
static char *server_hashes(const char *wrong, char wrong_sector)
{
	char *json;
	size_t len;
	FILE *f = open_memstream(&json, &len);
	fputs("{\"entityHashes\":{", f);
	const struct change *sorted[64];
	bool table_first = true;
	for (unsigned t = 0; t < server_count; t++) {
		const char *name = server_changes[t].name;
		bool seen = !strcmp(name, "note_reordering");
		for (unsigned u = 0; u < t; u++)
			seen |= !strcmp(server_changes[u].name, name);
		if (seen)
			continue;
		unsigned n = 0;
		for (unsigned i = 0; i < server_count; i++)
			if (!strcmp(server_changes[i].name, name))
				sorted[n++] = &server_changes[i];
		qsort(sorted, n, sizeof(*sorted), change_cmp);
		fprintf(f, "%s\"%s\":{", table_first ? "" : ",", name);
		table_first = false;
		for (unsigned i = 0; i < n;) {
			char sector = sorted[i]->id[0], text[4096] = "", hash[64];
			bool first = i == 0;
			for (; i < n && sorted[i]->id[0] == sector; i++) {
				change_hash(sorted[i], hash, sizeof(hash));
				strcat(text, hash);
				strcat(text, sorted[i]->erased ? "1" : "0");
			}
			if (wrong && !strcmp(wrong, name) && sector == wrong_sector)
				strcat(text, "!");
			base64_sha1(text, hash, sizeof(hash));
			fprintf(f, "%s\"%c\":\"%s\"", first ? "" : ",", sector, hash);
		}
		fputs("}", f);
	}
	fprintf(f, "},\"maxEntityChangeId\":%u}", server_count);
	fclose(f);
	return json;
}

static void server_serve_hashes(const char *wrong, char wrong_sector, bool once)
{
	char *json = server_hashes(wrong, wrong_sector);
	if (once)
		ui_test_serve_once(CHECK_HASHES, 200, JSON, json);
	else
		ui_test_serve(CHECK_HASHES, 200, JSON, json);
	free(json);
}

/* The page after change `after`, and what is left after it */
static void server_serve_page(unsigned after, unsigned per_page)
{
	char *json, url[200];
	size_t len;
	FILE *f = open_memstream(&json, &len);
	fputs("{\"entityChanges\":[", f);
	unsigned last = after;
	for (unsigned i = after; i < server_count && i < after + per_page; i++) {
		const struct change *c = &server_changes[i];
		char hash[64];
		change_hash(c, hash, sizeof(hash));
		fprintf(f,
		        "%s{\"entityChange\":{\"id\":%u,\"entityName\":\"%s\",\"entityId\":\"%s\",\"hash\":\"%s\","
		        "\"isErased\":%d,\"changeId\":\"chg%u\",\"componentId\":\"NA\",\"instanceId\":\"%s\","
		        "\"isSynced\":1,\"utcDateChanged\":\"%s\"}",
		        i > after ? "," : "", i + 1, c->name, c->id, hash, c->erased, i + 1, SERVER_ID,
		        c->date);
		if (c->entity)
			fprintf(f, ",\"entity\":%s", c->entity);
		fputs("}", f);
		last = i + 1;
	}
	fprintf(f, "],\"lastEntityChangeId\":%u,\"outstandingPullCount\":%u}", last, server_count - last);
	fclose(f);
	snprintf(url, sizeof(url), RELAY "/api/sync/changed?*lastEntityChangeId=%u&*", after);
	ui_test_serve(url, 200, JSON, json);
	free(json);
}

static void server(const struct change *changes, unsigned count, unsigned per_page)
{
	server_changes = changes;
	server_count = count;
	char login[200];
	snprintf(login, sizeof(login), "{\"instanceId\":\"%s\",\"maxEntityChangeId\":%u}", SERVER_ID, count);
	ui_test_serve(LOGIN, 200, JSON, login);
	ui_test_serve_headers("Set-Cookie: " COOKIE "; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600\n");
	for (unsigned after = 0; after < count; after += per_page)
		server_serve_page(after, per_page);
	server_serve_page(count, per_page); /* nothing more */
	ui_test_serve(FINISHED, 200, JSON, "{}");
	ui_test_serve(QUEUE, 200, JSON, "{}");
	ui_test_serve(RELAY "/api/sync/check-entity-changes", 200, JSON, "{}");
	server_serve_hashes(NULL, 0, false);
}

/* A document: the root note, a child with its text, a binary file, their branches, a
 * label, an option, the root's children reordered, a label erased */
#define DATE "\"2026-11-01 10:00:00.000Z\""
#define NOTE(id, title, blob) "{\"noteId\":\"" id "\",\"title\":\"" title "\",\"isProtected\":0,\"type\":\"text\"," \
	"\"mime\":\"text/html\",\"blobId\":\"" blob "\",\"isDeleted\":0,\"deleteId\":null,\"dateCreated\":" DATE \
	",\"dateModified\":" DATE ",\"utcDateCreated\":" DATE ",\"utcDateModified\":" DATE "}"
#define BRANCH(id, note, parent, pos) "{\"branchId\":\"" id "\",\"noteId\":\"" note "\",\"parentNoteId\":\"" parent \
	"\",\"notePosition\":" pos ",\"prefix\":null,\"isExpanded\":1,\"isDeleted\":0,\"deleteId\":null," \
	"\"utcDateModified\":" DATE "}"
#define BLOB(id, content) "{\"blobId\":\"" id "\",\"content\":" content ",\"textRepresentation\":null," \
	"\"dateModified\":" DATE ",\"utcDateModified\":" DATE "}"
static const struct change document[] = {
	{"notes", "root", false, "2026-11-01 10:00:00.000Z", NOTE("root", "root", "bRoot")},
	{"blobs", "bRoot", false, "2026-11-01 10:00:00.000Z", BLOB("bRoot", "\"\"")},
	{"branches", "none_root", false, "2026-11-01 10:00:00.000Z", BRANCH("none_root", "root", "none", "0")},
	{"notes", "n1", false, "2026-11-01 10:00:01.000Z", NOTE("n1", "Hello \\\"world\\\"", "bHello")},
	/* "<p>Hello</p>" */
	{"blobs", "bHello", false, "2026-11-01 10:00:01.000Z", BLOB("bHello", "\"PHA+SGVsbG88L3A+\"")},
	{"branches", "root_n1", false, "2026-11-01 10:00:01.000Z", BRANCH("root_n1", "n1", "root", "10")},
	{"notes", "n2", false, "2026-11-01 10:00:02.000Z", NOTE("n2", "Bytes", "bBytes")},
	/* 00 ff 10 80 */
	{"blobs", "bBytes", false, "2026-11-01 10:00:02.000Z", BLOB("bBytes", "\"AP8QgA==\"")},
	{"branches", "root_n2", false, "2026-11-01 10:00:02.000Z", BRANCH("root_n2", "n2", "root", "20")},
	{"attributes", "a1", false, "2026-11-01 10:00:03.000Z",
	 "{\"attributeId\":\"a1\",\"noteId\":\"n1\",\"type\":\"label\",\"name\":\"todo\",\"value\":\"\",\"position\":0,"
	 "\"utcDateModified\":" DATE ",\"isDeleted\":0,\"deleteId\":null,\"isInheritable\":0}"},
	{"options", "locale", false, "2026-11-01 10:00:04.000Z",
	 "{\"name\":\"locale\",\"value\":\"ro\",\"isSynced\":1,\"utcDateModified\":" DATE "}"},
	{"note_reordering", "root", false, "2026-11-01 10:00:05.000Z", "{\"root_n1\":30,\"root_n2\":5}"},
	{"attributes", "a2", true, "2026-11-01 10:00:06.000Z", NULL},
};
#define DOCUMENT document, sizeof(document) / sizeof(*document)
/* One of the root's notes, listed once synced */
#define SYNCED "Bytes"

static bool sync_requested(void)
{
	return ui_test_requested(LOGIN) != NULL;
}

static bool logged_in_twice(void)
{
	unsigned logins = 0;
	for (unsigned i = 0; i < ui_test_request_count(); i++)
		logins += !strcmp(ui_test_request((int)i)->url, LOGIN);
	return logins >= 2;
}

/* Sync set up and started: its login asked for */
static bool sync_started(void)
{
	return ui_test_wait_until(sync_requested, WAIT);
}

/* Sync set up in the database (the form passed), the app opened */
static bool open_set_up(void)
{
	return ui_test_open("Trilium") && ui_test_back() && execute(SET_UP) && ui_test_open("Trilium");
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
	CHECK(execute(SET_UP INITIALIZED "INSERT INTO notes (noteId, title, type, mime, blobId, dateCreated, dateModified,"
	              " utcDateCreated, utcDateModified) VALUES ('root', 'root', 'text', 'text/html', 'b1',"
	              " '2026-11-09 10:00:00.000+0200', '2026-11-09 10:00:00.000+0200',"
	              " '2026-11-09 08:00:00.000Z', '2026-11-09 08:00:00.000Z')"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_tap_text(UI_SYMBOL_CIRCLE_INFO));
	CHECK(ui_test_shows("1 note"));
	CHECK_STR(option("syncServerHost"), RELAY);
}

TEST(deleted_notes_not_counted)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_back());
	CHECK(execute(SET_UP INITIALIZED "INSERT INTO notes (noteId, title, type, mime, blobId, isDeleted, dateCreated,"
	              " dateModified, utcDateCreated, utcDateModified) VALUES"
	              " ('a', 'A', 'text', 'text/html', 'b1', 0, '', '', '', ''),"
	              " ('b', 'B', 'text', 'text/html', 'b2', 0, '', '', '', ''),"
	              " ('c', 'C', 'text', 'text/html', 'b3', 1, '', '', '', '')"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_tap_text(UI_SYMBOL_CIRCLE_INFO));
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
	CHECK(sync_started());
	CHECK(!ui_test_shows("Sync with Trilium"));
	CHECK_STR(option("syncServerHost"), RELAY);
	CHECK_STR(option("documentId"), "D0c1d");
	CHECK_STR(option("documentSecret"), "s3cr3t");
	CHECK_STR(query("SELECT isSynced FROM options WHERE name = 'documentSecret'"), "0");
	/* The password is not kept */
	CHECK_STR(query("SELECT COUNT(*) FROM options WHERE value LIKE '%hunter2%'"), "0");
	CHECK_STR(option("initialized"), "false");

	/* Opened again, set up: not the form */
	CHECK(ui_test_back());
	CHECK(ui_test_open("Trilium"));
	CHECK(!ui_test_shows("Sync with Trilium"));
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
	CHECK(sync_started());
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
	CHECK(sync_started());
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
	CHECK(sync_started());
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

/* HMAC-SHA256 of the login's timestamp with the document's secret, in base64, as Trilium's
 * crypto_provider.ts makes it */
static bool login_signed(const char *body)
{
	struct json *root = json_parse(body);
	const char *timestamp = json_str(json_get(root, "timestamp"), "");
	unsigned char digest[32];
	unsigned len = 0;
	HMAC(EVP_sha256(), "s3cr3t", 6, (const unsigned char *)timestamp, strlen(timestamp), digest, &len);
	char expected[64];
	EVP_EncodeBlock((unsigned char *)expected, digest, (int)len);
	bool ok = timestamp[0] && json_int(json_get(root, "syncVersion"), 0) == 41 &&
	          !strcmp(json_str(json_get(root, "hash"), ""), expected);
	json_free(root);
	return ok;
}

/* A document as the sync leaves it: notes and their branches, written straight into
 * the database */
#define TREE_NOTE(id, title, protected, deleted) "INSERT INTO notes (noteId, title, isProtected, type, mime, blobId," \
	" isDeleted, dateCreated, dateModified, utcDateCreated, utcDateModified) VALUES ('" id "', '" title "', " \
	protected ", 'text', 'text/html', 'b', " deleted ", '', '', '', '');"
#define TREE_BRANCH(note, parent, pos, prefix, deleted) "INSERT INTO branches (branchId, noteId, parentNoteId," \
	" notePosition, prefix, isDeleted, utcDateModified) VALUES ('" parent "_" note "', '" note "', '" parent "', " \
	pos ", " prefix ", " deleted ", '');"
#define TREE \
	TREE_NOTE("root", "root", "0", "0") TREE_BRANCH("root", "none", "0", "NULL", "0") \
	TREE_NOTE("_hidden", "Hidden Notes", "0", "0") TREE_BRANCH("_hidden", "root", "999999999", "NULL", "0") \
	TREE_NOTE("p", "Projects", "0", "0") TREE_BRANCH("p", "root", "20", "NULL", "0") \
	TREE_NOTE("i", "Ideas", "0", "0") TREE_BRANCH("i", "root", "10", "'Draft'", "0") \
	TREE_NOTE("s", "c2VjcmV0", "1", "0") TREE_BRANCH("s", "root", "30", "NULL", "0") \
	TREE_NOTE("gone", "Gone", "0", "1") TREE_BRANCH("gone", "root", "40", "NULL", "1") \
	TREE_NOTE("moved", "Moved away", "0", "0") TREE_BRANCH("moved", "root", "50", "NULL", "1") \
	TREE_BRANCH("moved", "p", "30", "NULL", "0") \
	TREE_NOTE("phone", "Phone", "0", "0") TREE_BRANCH("phone", "p", "10", "NULL", "0") \
	TREE_NOTE("kernel", "Kernel", "0", "0") TREE_BRANCH("kernel", "phone", "10", "NULL", "0") \
	TREE_BRANCH("phone", "i", "10", "NULL", "0") /* cloned into Ideas */

static bool open_tree(void)
{
	return ui_test_open("Trilium") && ui_test_back() && execute(SET_UP INITIALIZED TREE) &&
	       ui_test_open("Trilium");
}

static int32_t top(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	lv_area_t a = {0};
	if (label)
		lv_obj_get_coords(label, &a);
	return a.y1;
}

/* Once synced, the root's notes in the tree's order (with their prefix); the hidden
 * ones and the deleted not, protected ones without their title */
TEST(lists_the_notes)
{
	CHECK(open_tree());
	CHECK(ui_test_shows("Draft - Ideas"));
	CHECK(ui_test_shows("Projects"));
	CHECK(ui_test_shows("Protected note"));
	CHECK(!ui_test_shows("c2VjcmV0"));
	CHECK(!ui_test_shows("Hidden Notes"));
	CHECK(!ui_test_shows("Gone"));
	CHECK(!ui_test_shows("Moved away"));
	CHECK(!ui_test_shows("Phone"));
	CHECK(top("Draft - Ideas") < top("Projects"));
	CHECK(top("Projects") < top("Protected note"));
}

/* A note with children opens them on a screen of its own, back goes up */
TEST(drills_down)
{
	CHECK(open_tree());
	CHECK(ui_test_tap_text("Projects"));
	CHECK(ui_test_shows("Phone"));
	CHECK(ui_test_shows("Moved away"));
	CHECK(!ui_test_shows("Draft - Ideas"));
	CHECK(ui_test_tap_text("Phone"));
	CHECK(ui_test_shows("Kernel"));
	/* A note without children: its content */
	CHECK(ui_test_tap_text("Kernel"));
	CHECK(ui_test_shows("This note is empty"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Kernel"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Moved away"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Draft - Ideas"));
	/* A clone, as where else it is */
	CHECK(ui_test_tap_text("Draft - Ideas"));
	CHECK(ui_test_tap_text("Phone"));
	CHECK(ui_test_shows("Kernel"));
}

/* A leaf of the tree, Kernel, with `type`, `mime` and the content `blob` (SQL), opened */
#define KERNEL(type, mime, blob) "UPDATE notes SET type = '" type "', mime = '" mime "', blobId = 'bk'" \
	" WHERE noteId = 'kernel'; INSERT INTO blobs (blobId, content, dateModified, utcDateModified)" \
	" VALUES ('bk', " blob ", '', '');"
static bool open_kernel(const char *sql)
{
	return ui_test_open("Trilium") && ui_test_back() && execute(SET_UP INITIALIZED TREE) && execute(sql) &&
	       ui_test_open("Trilium") && ui_test_tap_text("Projects") && ui_test_tap_text("Phone") &&
	       ui_test_tap_text("Kernel");
}

static bool page_shown(void)
{
	return ui_test_find_widget(&lv_3dtexture_class) != NULL;
}

/* A text note, HTML as Trilium's editor keeps it (a blob, as the sync stores it), laid
 * out as a page: its colors drawn */
TEST(opens_a_text_note)
{
	ui_test_write("/var/lib/phone-ui/settings", "light=1\n"); /* its colors as they are */
	CHECK(open_kernel(KERNEL("text", "text/html",
	                         "CAST('<p>Hello</p><div style=\"background: #ff0000; height: 60px\"></div>"
	                         "<div style=\"background: #00ff00; height: 60px\"></div>' AS BLOB)")));
	CHECK(ui_test_wait_until(page_shown, WAIT));
	ui_test_wait(100); /* a frame with it drawn */
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page && ui_test_shows_color(lv_obj_get_parent(page), 0xff0000));
	CHECK(page && ui_test_shows_color(lv_obj_get_parent(page), 0x00ff00));
	/* Its title as the screen's */
	CHECK(ui_test_shows("Kernel"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Kernel"));
	CHECK(!page_shown());
}

/* Code as it is written, in monospace */
TEST(opens_a_code_note)
{
	CHECK(open_kernel(KERNEL("code", "text/x-csrc", "'int main(void)\n{\n\treturn 0;\n}'")));
	CHECK(ui_test_shows("int main(void)\n{\n\treturn 0;\n}"));
	CHECK(ui_test_font("int main(void)\n{\n\treturn 0;\n}") == &ui_font_mono_20);
	CHECK(!page_shown());
}

/* A file of text the same */
TEST(opens_a_text_file)
{
	CHECK(open_kernel(KERNEL("file", "text/plain", "CAST('Shopping: milk' AS BLOB)")));
	CHECK(ui_test_shows("Shopping: milk"));
}

TEST(empty_note)
{
	CHECK(open_kernel(KERNEL("text", "text/html", "''")));
	CHECK(ui_test_shows("This note is empty"));
	CHECK(!page_shown());
}

/* Its content not synced (yet): the same */
TEST(content_missing)
{
	CHECK(open_kernel("UPDATE notes SET blobId = 'none' WHERE noteId = 'kernel'"));
	CHECK(ui_test_shows("This note is empty"));
}

TEST(protected_note_not_opened)
{
	CHECK(ui_test_open("Trilium") && ui_test_back());
	CHECK(execute(SET_UP INITIALIZED TREE KERNEL("text", "text/html", "CAST('c2VjcmV0' AS BLOB)")
	              "UPDATE notes SET isProtected = 1 WHERE noteId = 'kernel'"));
	CHECK(ui_test_open("Trilium") && ui_test_tap_text("Projects") && ui_test_tap_text("Phone"));
	CHECK(ui_test_tap_text("Protected note"));
	CHECK(ui_test_shows("Protected notes cannot be opened yet"));
	CHECK(!ui_test_shows_part("c2VjcmV0"));
}

/* What only Trilium shows (a canvas, a relation map...) */
TEST(other_kinds_not_shown)
{
	CHECK(open_kernel(KERNEL("canvas", "application/json", "'{\"elements\":[]}'")));
	CHECK(ui_test_shows("This kind of note cannot be shown yet"));
	CHECK(!ui_test_shows_part("elements"));
}

/* Projects, a note with children, with `type`, `mime` and the content `blob` (SQL) */
#define PROJECTS(type, mime, blob) "UPDATE notes SET type = '" type "', mime = '" mime "', blobId = 'bp'" \
	" WHERE noteId = 'p'; INSERT INTO blobs (blobId, content, dateModified, utcDateModified)" \
	" VALUES ('bp', " blob ", '', '');"
static bool open_projects(const char *sql)
{
	return ui_test_open("Trilium") && ui_test_back() && execute(SET_UP INITIALIZED TREE) && execute(sql) &&
	       ui_test_open("Trilium") && ui_test_tap_text("Projects");
}

/* The card above the children: the note's content, cut */
static lv_obj_t *preview_card(void)
{
	lv_obj_t *label = ui_test_find("Phone"), *list = label;
	for (int i = 0; list && i < 4; i++) /* its column, row, the list's card, the list */
		list = lv_obj_get_parent(list);
	return list && lv_obj_get_child_count(list) > 1 ? lv_obj_get_child(list, 0) : NULL;
}

static void tap_obj(lv_obj_t *obj)
{
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	ui_test_tap_at((a.x1 + a.x2) / 2, a.y1 + 40);
}

/* A note with children shows its content above them, as Trilium does, cut to a card's
 * height; tapped, the whole note */
TEST(folder_shows_its_content)
{
	ui_test_write("/var/lib/phone-ui/settings", "light=1\n"); /* its colors as they are */
	CHECK(open_projects(PROJECTS("text", "text/html",
	                             "CAST('<div style=\"background: #ff0000; height: 900px\"></div>"
	                             "<div style=\"background: #00ff00; height: 60px\"></div>' AS BLOB)")));
	CHECK(ui_test_wait_until(page_shown, WAIT));
	ui_test_wait(100);
	CHECK(ui_test_shows("Phone"));
	lv_obj_t *card = preview_card();
	CHECK(card && ui_test_shows_color(card, 0xff0000));
	CHECK(card && !ui_test_shows_color(card, 0x00ff00)); /* cut */
	CHECK(card && lv_obj_get_height(card) < 600);
	if (card)
		tap_obj(card);
	ui_test_wait(500);
	CHECK(!ui_test_shows("Phone"));
	CHECK(ui_test_shows("Projects"));
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page && lv_obj_get_height(lv_obj_get_parent(page)) > 900); /* all of it */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Phone"));
}

/* Code the same, its first lines */
TEST(folder_shows_its_code)
{
	CHECK(open_projects(PROJECTS("code", "text/plain", "'first line\n' || replace(hex(zeroblob(60)), '00', 'more\n')"
	                             " || 'last line'")));
	CHECK(ui_test_shows_part("first line"));
	lv_obj_t *card = preview_card();
	CHECK(card && lv_obj_get_height(card) < 600);
	CHECK(ui_test_shows("Phone"));
}

/* Nothing to show: no card, the children only */
TEST(folder_without_content)
{
	CHECK(open_projects(PROJECTS("text", "text/html", "''")));
	CHECK(ui_test_shows("Phone"));
	CHECK(!preview_card());
	CHECK(!ui_test_shows("This note is empty"));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	CHECK(execute("UPDATE notes SET type = 'canvas' WHERE noteId = 'p';"
	              "UPDATE blobs SET content = '{}' WHERE blobId = 'bp'"));
	CHECK(ui_test_open("Trilium") && ui_test_tap_text("Projects"));
	CHECK(ui_test_shows("Phone"));
	CHECK(!preview_card());
	CHECK(!ui_test_shows("This kind of note cannot be shown yet"));
}

/* Jump to note, as Trilium's: by the title, the words typed anywhere in the note's title
 * or path, case and diacritics aside */
static bool open_jump(const char *sql)
{
	return ui_test_open("Trilium") && ui_test_back() && execute(SET_UP INITIALIZED TREE) && execute(sql) &&
	       ui_test_open("Trilium") && ui_test_tap_field("Jump to note");
}

TEST(jump_to_a_note)
{
	CHECK(open_jump(""));
	CHECK(ui_test_type("kern"));
	CHECK(ui_test_shows("Kernel"));
	CHECK(ui_test_shows("Projects \xe2\x80\xba Phone")); /* where it is */
	CHECK(!ui_test_shows("Projects"));
	CHECK(ui_test_tap_text("Kernel"));
	CHECK(ui_test_shows("This note is empty"));
	/* Back to what was found */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Projects \xe2\x80\xba Phone"));
}

/* Deeper, where it is by its last two: its parent tells it apart */
TEST(jump_shows_the_end_of_a_deep_path)
{
	CHECK(open_jump(TREE_NOTE("d", "Drivers", "0", "0") TREE_BRANCH("d", "kernel", "10", "NULL", "0")));
	CHECK(ui_test_type("drivers"));
	CHECK(ui_test_shows("\xe2\x80\xa6 \xe2\x80\xba Phone \xe2\x80\xba Kernel"));
}

/* A note with children opens them */
TEST(jump_to_a_folder)
{
	CHECK(open_jump(""));
	CHECK(ui_test_type("projects"));
	CHECK(ui_test_tap_text("Projects"));
	CHECK(ui_test_shows("Phone"));
	CHECK(ui_test_shows("Moved away"));
}

/* The title matching best first: the same, then starting so, then containing it */
TEST(jump_ranks_by_title)
{
	CHECK(open_jump(TREE_NOTE("a", "My phone numbers", "0", "0") TREE_BRANCH("a", "root", "60", "NULL", "0")
	                TREE_NOTE("b", "Phone case", "0", "0") TREE_BRANCH("b", "root", "70", "NULL", "0")));
	CHECK(ui_test_type("phone"));
	CHECK(ui_test_shows("Phone"));
	CHECK(top("Phone") < top("Phone case"));
	CHECK(top("Phone case") < top("My phone numbers"));
	/* Every word, in the title or the path */
	CHECK(ui_test_type(" kern"));
	CHECK(ui_test_shows("Kernel"));
	CHECK(!ui_test_shows("Phone case"));
}

TEST(jump_ignores_diacritics)
{
	CHECK(open_jump(TREE_NOTE("r", "\xc8\x98" "edin\xc8\x9b\xc4\x83 lunar\xc4\x83", "0", "0")
	                TREE_BRANCH("r", "root", "60", "NULL", "0")));
	CHECK(ui_test_type("SEDINTA"));
	CHECK(ui_test_shows("\xc8\x98" "edin\xc8\x9b\xc4\x83 lunar\xc4\x83"));
}

/* Not Trilium's own notes, the deleted, nor the protected (their titles encrypted) */
TEST(jump_leaves_out_hidden_deleted_protected)
{
	CHECK(open_jump(TREE_NOTE("h", "Hidden thing", "0", "0") TREE_BRANCH("h", "_hidden", "10", "NULL", "0")));
	CHECK(ui_test_type("hidden"));
	CHECK(ui_test_shows("No notes found"));
	for (int i = 0; i < 6; i++)
		CHECK(ui_test_type("\b"));
	CHECK(ui_test_type("gone"));
	CHECK(ui_test_shows("No notes found"));
	for (int i = 0; i < 4; i++)
		CHECK(ui_test_type("\b"));
	CHECK(ui_test_type("c2vj"));
	CHECK(ui_test_shows("No notes found"));
}

/* Enter opens the first */
TEST(jump_enter_opens_the_first)
{
	CHECK(open_jump(""));
	CHECK(ui_test_type("kernel\n"));
	CHECK(ui_test_shows("This note is empty"));
}

/* All of a long list (a journal's days), its rows made for those in view only (a
 * virtual list): the last one there once scrolled to the end, and opened */
TEST(many_notes)
{
	CHECK(ui_test_open("Trilium") && ui_test_back());
	CHECK(execute(SET_UP INITIALIZED TREE_NOTE("root", "root", "0", "0")
	              "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 1999)"
	              " INSERT INTO notes (noteId, title, type, mime, blobId, dateCreated, dateModified, utcDateCreated,"
	              " utcDateModified) SELECT 'n' || i, printf('Day %04d', i), 'text', 'text/html', 'b', '', '', '', ''"
	              " FROM n;"
	              "INSERT INTO branches (branchId, noteId, parentNoteId, notePosition, utcDateModified)"
	              " SELECT 'root_' || noteId, noteId, 'root', CAST(substr(noteId, 2) AS INTEGER), '' FROM notes"
	              " WHERE noteId != 'root'"));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("Day 0000"));
	CHECK(!ui_test_shows("Day 1999"));
	lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(ui_test_find("Day 0000")));
	CHECK(lv_obj_get_child_count(lv_obj_get_parent(row)) < 100);
	lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(row)), LV_COORD_MAX, LV_ANIM_OFF);
	ui_test_wait(100);
	CHECK(ui_test_shows("Day 1999"));
	CHECK(!ui_test_shows("Day 0000"));
	CHECK(ui_test_tap_text("Day 1999"));
	CHECK(ui_test_shows("This note is empty"));
}

TEST(no_notes_yet)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_back());
	CHECK(execute(SET_UP INITIALIZED TREE_NOTE("root", "root", "0", "0")));
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("No notes yet"));
}

/* The first sync: everything pulled into the database as Trilium's sync_update.ts puts
 * it, the content hashes compared, the document initialized; then what it holds */
TEST(initial_sync)
{
	server(DOCUMENT, 100);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	CHECK(ui_test_shows("Hello \"world\""));
	CHECK(ui_test_tap_text(UI_SYMBOL_CIRCLE_INFO));
	CHECK(ui_test_shows("3 notes"));
	CHECK(ui_test_shows("1 attribute"));
	CHECK_STR(option("initialized"), "true");
	CHECK_STR(option("lastSyncedPull"), "13");

	/* The rows */
	CHECK_STR(query("SELECT title FROM notes WHERE noteId = 'n1'"), "Hello \"world\"");
	CHECK_STR(query("SELECT typeof(content) || ':' || CAST(content AS TEXT) FROM blobs WHERE blobId = 'bHello'"),
	          "blob:<p>Hello</p>");
	CHECK_STR(query("SELECT hex(content) FROM blobs WHERE blobId = 'bBytes'"), "00FF1080");
	CHECK_STR(query("SELECT typeof(content) || ':' || content FROM blobs WHERE blobId = 'bRoot'"), "text:");
	CHECK_STR(query("SELECT notePosition FROM branches WHERE branchId = 'root_n1'"), "30");
	CHECK_STR(query("SELECT notePosition FROM branches WHERE branchId = 'root_n2'"), "5");
	CHECK_STR(query("SELECT prefix IS NULL FROM branches WHERE branchId = 'root_n1'"), "1");
	CHECK_STR(query("SELECT group_concat(attributeId) FROM attributes"), "a1");
	CHECK_STR(query("SELECT value || isSynced FROM options WHERE name = 'locale'"), "ro1");
	/* Their entity changes, the server's */
	CHECK_STR(query("SELECT COUNT(*) FROM entity_changes WHERE instanceId = '" SERVER_ID "' AND isSynced = 1"), "13");
	CHECK_STR(query("SELECT changeId || hash || isErased FROM entity_changes WHERE entityId = 'a2'"),
	              "chg13hattributesa21");

	/* Logged in with the document's secret; the session's cookie on what followed */
	const struct ui_test_request *r = ui_test_requested(LOGIN);
	CHECK(r && !strcmp(r->method, "POST") && r->body && login_signed(r->body));
	r = ui_test_requested(CHANGED);
	CHECK(r && strstr(r->headers, "Cookie: " COOKIE "\n"));
	CHECK(r && strstr(r->url, "instanceId=") && !strstr(r->url, "instanceId=&"));
	r = ui_test_requested(FINISHED);
	CHECK(r && !strcmp(r->method, "POST") && strstr(r->headers, "Cookie: " COOKIE "\n"));
	CHECK(ui_test_requested(CHECK_HASHES));
	CHECK(!ui_test_requested(QUEUE));
}

/* Opened again once synced: what it holds, no sync of its own yet */
TEST(synced_opened_again)
{
	server(DOCUMENT, 100);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	CHECK(ui_test_back());
	unsigned requests = ui_test_request_count();
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows(SYNCED));
	CHECK_INT((int)ui_test_request_count(), (int)requests);
}

/* The bar: what was pulled of all there was to pull */
TEST(progress_shown)
{
	server(DOCUMENT, 5);
	ui_test_network_hold_at(RELAY "/api/sync/changed?*lastEntityChangeId=5&*");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("38%", WAIT)); /* 5 of 13 */
	CHECK(ui_test_shows("Sync in progress"));
	CHECK(ui_test_shows("Connecting to the server"));
	CHECK(ui_test_shows("Syncing data"));
	CHECK(ui_test_shows("Checking the notes"));
	lv_obj_t *bar = ui_test_find_widget(&lv_bar_class);
	CHECK(bar && lv_bar_get_value(bar) * 100 / lv_bar_get_max_value(bar) == 38);
	ui_test_network_hold(false);
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	CHECK(!ui_test_shows("Sync in progress"));
}

/* Left while syncing: it goes on, the document is there when opened again */
TEST(left_while_syncing)
{
	server(DOCUMENT, 5);
	ui_test_network_hold_at(RELAY "/api/sync/changed?*lastEntityChangeId=5&*");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("38%", WAIT));
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	CHECK_STR(option("initialized"), "true");
}

/* Stopped halfway: the next sync goes on from the last page kept */
TEST(resumes_where_it_stopped)
{
	server(DOCUMENT, 5);
	ui_test_serve(RELAY "/api/sync/changed?*lastEntityChangeId=5&*", 500, JSON, "{\"message\":\"boom\"}");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The server answered with an error (500)."));
	CHECK_STR(option("lastSyncedPull"), "5");
	CHECK_STR(option("initialized"), "false");
	CHECK_STR(query("SELECT COUNT(*) FROM entity_changes"), "5");

	server_serve_page(5, 5);
	unsigned before = ui_test_request_count();
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	/* Not from the start again */
	for (unsigned i = before; i < ui_test_request_count(); i++)
		CHECK(!strstr(ui_test_request((int)i)->url, "lastEntityChangeId=0&"));
	CHECK_STR(option("lastSyncedPull"), "13");
}

/* A sector whose hashes differ is queued again on the server, pulled again, then checked */
TEST(differing_sector_synced_again)
{
	server(DOCUMENT, 100);
	server_serve_hashes("notes", 'n', true);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	const struct ui_test_request *r = ui_test_requested(RELAY "/api/sync/queue-sector/notes/n");
	CHECK(r && !strcmp(r->method, "POST"));
	CHECK(ui_test_requested(RELAY "/api/sync/check-entity-changes"));
	CHECK(!ui_test_requested(RELAY "/api/sync/queue-sector/notes/r"));
	CHECK_STR(option("initialized"), "true");
}

/* Still differing after the attempts: not initialized, said so */
TEST(never_converges)
{
	server(DOCUMENT, 100);
	server_serve_hashes("notes", 'n', false);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The notes on the phone and on the server still differ after syncing."));
	CHECK_STR(option("initialized"), "false");
}

TEST(credentials_refused)
{
	server(DOCUMENT, 100);
	ui_test_serve_once(LOGIN, 400, JSON,
	                   "{\"message\":\"Sync login credentials are incorrect. It looks like you're trying to sync two "
	                   "different initialized documents which is not possible.\"}");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The server does not accept this phone's credentials for the document."));
	/* Tried again, accepted */
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_wait_for(SYNCED, WAIT));
}

/* Trilium refuses a login more than 5 minutes off its clock (401), a relay too (400) */
TEST(clocks_differ)
{
	ui_test_serve(LOGIN, 401, JSON,
	              "{\"message\":\"Auth request time is out of sync, please check that both client and server have "
	              "correct time. The difference between clocks has to be smaller than 5 minutes.\"}");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The phone's clock and the server's differ by more than 5 minutes."));

	ui_test_serve(LOGIN, 400, JSON, "{\"message\":\"Timestamp is too far from server time\"}");
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_wait_until(logged_in_twice, WAIT));
	CHECK(ui_test_wait_for("The phone's clock and the server's differ by more than 5 minutes.", WAIT));
}

TEST(other_version_at_login)
{
	ui_test_serve(LOGIN, 400, JSON,
	              "{\"message\":\"Non-matching sync versions, local is version 42, remote is 41. It is recommended "
	              "to run same version of Trilium on both sides of sync.\"}");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The server runs another version of Trilium."));
}

TEST(sync_offline)
{
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("Cannot reach the server."));
}

/* A column this schema does not have (a newer Trilium): stopped, nothing of that page kept */
TEST(unknown_column)
{
	static const struct change newer[] = {
		{"notes", "root", false, "2026-11-01 10:00:00.000Z",
		 "{\"noteId\":\"root\",\"title\":\"root\",\"isProtected\":0,\"type\":\"text\",\"mime\":\"text/html\","
		 "\"blobId\":\"b\",\"isDeleted\":0,\"deleteId\":null,\"dateCreated\":\"\",\"dateModified\":\"\","
		 "\"utcDateCreated\":\"\",\"utcDateModified\":\"\",\"color\":\"red\"}"},
	};
	server(newer, 1, 100);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_shows("The server sent notes this app cannot store (a newer version of Trilium?)."));
	CHECK_STR(query("SELECT COUNT(*) FROM notes"), "0");
	CHECK_STR(option("lastSyncedPull"), "0");
}

#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0/"

static long max_mhz(void)
{
	long khz = 0;
	ui_read_long(CPUFREQ "scaling_max_freq", &khz);
	return khz / 1000;
}

static bool cpu_lowest(void)
{
	return max_mhz() == 648;
}

/* The CPU at its highest while syncing (applying changes is most of a sync's time), at
 * its lowest again once done */
TEST(sync_at_full_speed)
{
	server(DOCUMENT, 5);
	ui_test_network_hold_at(RELAY "/api/sync/changed?*lastEntityChangeId=10&*");
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("76%", WAIT)); /* 10 of 13 */
	CHECK_INT(max_mhz(), 1152);
	ui_test_network_hold(false);
	CHECK(ui_test_wait_for(SYNCED, WAIT));
	CHECK(ui_test_wait_until(cpu_lowest, WAIT));
}

static bool second_page_asked(void)
{
	return ui_test_requested(RELAY "/api/sync/changed?*lastEntityChangeId=1&*") != NULL;
}

/* The next page asked for while one is applied, as Trilium does: even one that fails */
TEST(next_page_while_applying)
{
	static const char bad[] = "{\"noteId\":\"root\",\"color\":\"red\"}";
	static const struct change newer[] = {{"notes", "root", false, "2026-11-01 10:00:00.000Z", bad}};
	server(newer, 1, 1);
	CHECK(open_set_up());
	CHECK(ui_test_wait_for("Sync failed", WAIT));
	CHECK(ui_test_wait_until(second_page_asked, WAIT));
	CHECK_STR(option("lastSyncedPull"), "0");
}
