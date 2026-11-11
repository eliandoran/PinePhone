/* docs/external/trilium.md: Trilium, the plugin app, its document written straight into its
 * database as a sync leaves it (as test_trilium.c does), synced with a server served from
 * here (fake_curl.c) that has nothing new: the sync indicator connected */

#include <sqlite3.h>

#include "ui/rich_edit.h"

#define TRILIUM_DB "/var/lib/trilium/document.db"
#define TRILIUM_SERVER "https://notes.example.org"

static void trilium_seed(void)
{
	plugin_install(TRILIUM_APP, "trilium", "trilium.so", NULL);
}

static bool trilium_execute(const char *sql)
{
	sqlite3 *db;
	bool ok = sqlite3_open_v2(TRILIUM_DB, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
	          sqlite3_busy_timeout(db, 5000) == SQLITE_OK && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
	sqlite3_close(db);
	return ok;
}

/* Sync set up and done, as the form and the first sync leave it */
#define TRILIUM_SET_UP "UPDATE options SET value = '" TRILIUM_SERVER "' WHERE name = 'syncServerHost';" \
	"INSERT INTO options (name, value, isSynced, utcDateModified) VALUES" \
	" ('documentId', 'D0c1d', 0, ''), ('documentSecret', 's3cr3t', 0, '');" \
	"UPDATE options SET value = 'true' WHERE name = 'initialized';"

/* A note: its row, its content's blob, its branch under `parent` */
#define TRILIUM_NOTE(id, title, type, mime, parent, pos, content) \
	"INSERT INTO notes (noteId, title, isProtected, type, mime, blobId, isDeleted, dateCreated, dateModified," \
	" utcDateCreated, utcDateModified) VALUES ('" id "', '" title "', 0, '" type "', '" mime "', 'b" id "', 0," \
	" '', '', '', '');" \
	"INSERT INTO blobs (blobId, content, dateModified, utcDateModified) VALUES ('b" id "', '" content "', '', '');" \
	"INSERT INTO branches (branchId, noteId, parentNoteId, notePosition, prefix, isDeleted, utcDateModified)" \
	" VALUES ('" parent "_" id "', '" id "', '" parent "', " pos ", NULL, 0, '');"
#define TRILIUM_TEXT(id, title, parent, pos, html) TRILIUM_NOTE(id, title, "text", "text/html", parent, pos, html)
#define TRILIUM_ICON(id, icon) "INSERT INTO attributes (attributeId, noteId, type, name, value, position," \
	" utcDateModified, isDeleted, isInheritable) VALUES ('i" id "', '" id "', 'label', 'iconClass', '" icon "'," \
	" 0, '', 0, 0);"

#define TRILIUM_FIRMWARE "<h2>Goals</h2><p>A feature phone on the <strong>PinePhone</strong>: calls, messages" \
	" and a battery that lasts.</p><ul><li>Ring within a second of a call</li><li>Two days on a charge</li>" \
	"</ul><h2>Status</h2><figure class=\"table\"><table><thead><tr><th>Part</th><th>State</th></tr></thead>" \
	"<tbody><tr><td>Calls</td><td>Working</td></tr><tr><td>Mobile data</td><td>Working</td></tr>" \
	"<tr><td>GPS</td><td>Not started</td></tr></tbody></table></figure>"
#define TRILIUM_TODO(text, checked) "<li><label class=\"todo-list__label\"><input type=\"checkbox\"" \
	checked " disabled=\"disabled\"><span class=\"todo-list__label__description\">" text "</span></label></li>"
#define TRILIUM_SHOPPING "<p>For the weekend:</p><ul class=\"todo-list\">" TRILIUM_TODO("Oat milk", " checked=\"checked\"") \
	TRILIUM_TODO("Tomatoes", "") TRILIUM_TODO("Sourdough bread", "") TRILIUM_TODO("Coffee beans", "") "</ul>"

#define TRILIUM_DOCUMENT \
	"INSERT INTO notes (noteId, title, isProtected, type, mime, blobId, isDeleted, dateCreated, dateModified," \
	" utcDateCreated, utcDateModified) VALUES ('root', 'root', 0, 'text', 'text/html', '', 0, '', '', '', '');" \
	"INSERT INTO branches (branchId, noteId, parentNoteId, notePosition, prefix, isDeleted, utcDateModified)" \
	" VALUES ('none_root', 'root', 'none', 0, NULL, 0, '');" \
	TRILIUM_TEXT("journal", "Journal", "root", "10", "") TRILIUM_ICON("journal", "bx bx-calendar") \
	TRILIUM_TEXT("j2026", "2026", "journal", "10", "") \
	TRILIUM_TEXT("projects", "Projects", "root", "20", \
	             "<p>What I am working on this year, a note each. Finished ones go to the archive.</p>") \
	TRILIUM_TEXT("firmware", "PinePhone firmware", "projects", "10", TRILIUM_FIRMWARE) \
	TRILIUM_TEXT("garden", "Vegetable garden", "projects", "20", "<p>Raised beds by the fence.</p>") \
	TRILIUM_TEXT("bike", "Bike repair", "projects", "30", "<p>New chain and brake pads.</p>") \
	TRILIUM_NOTE("backup", "backup.sh", "code", "text/x-sh", "projects", "40", \
	             "#!/bin/sh\nrsync -a ~/notes/ backup:/srv/notes/\n") \
	TRILIUM_TEXT("recipes", "Recipes", "root", "30", "") TRILIUM_ICON("recipes", "bx bx-bowl-hot") \
	TRILIUM_TEXT("soup", "Lentil soup", "recipes", "10", "<p>Red lentils, carrots, cumin.</p>") \
	TRILIUM_TEXT("reading", "Reading list", "root", "40", "<p>The Left Hand of Darkness</p>") \
	TRILIUM_ICON("reading", "bx bx-book") \
	TRILIUM_TEXT("shopping", "Shopping", "root", "50", TRILIUM_SHOPPING) \
	TRILIUM_ICON("shopping", "bx bx-cart") \
	"INSERT INTO notes (noteId, title, isProtected, type, mime, blobId, isDeleted, dateCreated, dateModified," \
	" utcDateCreated, utcDateModified) VALUES ('secret', 'c2VjcmV0', 1, 'text', 'text/html', '', 0, '', '', '', '');" \
	"INSERT INTO branches (branchId, noteId, parentNoteId, notePosition, prefix, isDeleted, utcDateModified)" \
	" VALUES ('root_secret', 'secret', 'root', 60, NULL, 0, '');"

/* The server: logged in, nothing new, the same content */
static void trilium_serve(void)
{
	ui_test_serve(TRILIUM_SERVER "/api/login/sync", 200, "application/json",
	              "{\"instanceId\":\"srv0instance\",\"maxEntityChangeId\":0}");
	ui_test_serve_headers("Set-Cookie: trilium_jwt=x; Path=/; HttpOnly\n");
	ui_test_serve(TRILIUM_SERVER "/api/sync/changed?*", 200, "application/json",
	              "{\"entityChanges\":[],\"lastEntityChangeId\":0,\"outstandingPullCount\":0}");
	ui_test_serve(TRILIUM_SERVER "/api/sync/finished", 200, "application/json", "{}");
	ui_test_serve(TRILIUM_SERVER "/api/sync/check", 200, "application/json",
	              "{\"entityHashes\":{},\"maxEntityChangeId\":0}");
}

/* Opened once for its database, the document written, opened again: the root's notes */
static bool trilium_open(void)
{
	trilium_serve();
	return ui_test_open("Trilium") && ui_test_back() && trilium_execute(TRILIUM_SET_UP TRILIUM_DOCUMENT) &&
	       ui_test_open("Trilium") && ui_test_wait_for("Shopping", 3000);
}

static bool trilium_page_shown(void)
{
	return ui_test_find_widget(&lv_3dtexture_class) != NULL;
}

/* trilium.md, Connecting to your server: the form, the address typed */
TEST(trilium_connect)
{
	CHECK(ui_test_open("Trilium"));
	CHECK(ui_test_shows("Sync with Trilium"));
	CHECK(ui_test_tap_field("Server address"));
	CHECK(ui_test_type(TRILIUM_SERVER));
	CHECK(ui_test_shows("Connect"));
	ui_test_settle();
	shot("trilium-connect");
}

/* trilium.md, Browsing your notes: the root's notes, the bar */
TEST(trilium_notes)
{
	CHECK(trilium_open());
	CHECK(ui_test_shows("Projects"));
	CHECK(ui_test_shows("Protected note"));
	CHECK(ui_test_shows("Search"));
	CHECK(ui_test_wait_for("\xee\xb1\xbc", 3000)); /* bx-wifi: synced */
	ui_test_settle();
	shot("trilium-notes");
}

/* trilium.md, Browsing your notes: a note with children, its text above them */
TEST(trilium_folder)
{
	CHECK(trilium_open());
	CHECK(ui_test_tap_text("Projects"));
	CHECK(ui_test_wait_until(trilium_page_shown, 3000));
	CHECK(ui_test_shows("PinePhone firmware"));
	CHECK(ui_test_shows("backup.sh"));
	ui_test_settle();
	shot("trilium-folder");
}

/* trilium.md, Reading a note: a text note as a page */
TEST(trilium_note)
{
	CHECK(trilium_open());
	CHECK(ui_test_tap_text("Projects"));
	CHECK(ui_test_tap_text("PinePhone firmware"));
	CHECK(ui_test_wait_until(trilium_page_shown, 3000));
	CHECK(ui_test_shows(LV_SYMBOL_EDIT));
	ui_test_wait(300);
	ui_test_settle();
	shot("trilium-note");
}

/* trilium.md, Searching: a word, the notes with where they are */
TEST(trilium_search)
{
	CHECK(trilium_open());
	CHECK(ui_test_tap_text("Search"));
	CHECK(ui_test_type("pro"));
	CHECK(ui_test_wait_for("PinePhone firmware", 5000));
	CHECK(ui_test_shows("Vegetable garden"));
	ui_test_settle();
	shot("trilium-search");
}

/* trilium.md, Writing and editing: a checklist edited, the toolbar over the keys */
TEST(trilium_edit)
{
	CHECK(trilium_open());
	CHECK(ui_test_tap_text("Shopping"));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_find_widget(&ui_rich_edit_class) != NULL);
	CHECK(ui_test_type("\nOlive oil"));
	CHECK(ui_test_shows_part("Olive oil"));
	ui_test_settle();
	shot("trilium-edit");
}
