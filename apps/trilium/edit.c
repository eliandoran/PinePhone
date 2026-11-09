/*
 * Trilium: a text note edited, see trilium.h. In phone-ui's rich text editor (as Notes'
 * editor: its toolbar on the keys while typing), its document read from the note's HTML
 * (html.c, only when all of it is what the editor holds). Saved as the keyboard goes
 * and as the editor is left, when its HTML changed, as Trilium saves a note's content
 * (abstract_becca_entity.ts' setContent()): a blob named by the content's SHA-512, the
 * note pointing at it with its dates now, the old blob deleted when nothing else uses
 * it, and an entity change for each, stamped with this phone's instance
 * (tr_db_instance()), for the sync to push (sync.c: not yet). No revision is kept.
 */

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"

unsigned tr_note_saves;

/* Saving */

static void base64(const unsigned char *bytes, unsigned n, char *out)
{
	EVP_EncodeBlock((unsigned char *)out, bytes, (int)n);
}

/* Trilium's hash(): SHA-1, base64 */
static void sha1_base64(const char *text, char out[32])
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned n = 0;
	EVP_Digest(text, strlen(text), digest, &n, EVP_sha1(), NULL);
	base64(digest, n, out);
}

/* Trilium's hashedBlobId(): SHA-512, base64 without + and /, 20 characters */
static void blob_id(const char *content, char out[24])
{
	unsigned char digest[EVP_MAX_MD_SIZE];
	char b64[100];
	unsigned n = 0;
	EVP_Digest(content, strlen(content), digest, &n, EVP_sha512(), NULL);
	base64(digest, n, b64);
	for (char *c = b64; *c; c++)
		*c = *c == '+' ? 'X' : *c == '/' ? 'Y' : *c;
	snprintf(out, 24, "%.20s", b64);
}

static bool exec(sqlite3 *db, const char *sql, const char *const params[], int n)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: %s\n", sqlite3_errmsg(db));
		return false;
	}
	for (int i = 0; i < n; i++)
		sqlite3_bind_text(st, i + 1, params[i], -1, SQLITE_STATIC);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok)
		fprintf(stderr, "trilium: %s\n", sqlite3_errmsg(db));
	sqlite3_finalize(st);
	return ok;
}

/* As entity_changes.ts' putEntityChange(): replacing the entity's earlier one */
static bool entity_change(sqlite3 *db, const char *name, const char *id, const char *hash, const char *instance,
                          const char *utc)
{
	char change[16];
	tr_random_id(change, 12);
	const char *params[] = {name, id, hash, change, instance, utc};
	return exec(db,
	            "INSERT OR REPLACE INTO entity_changes (entityName, entityId, hash, isErased, changeId, componentId," /* NON-NLS */
	            " instanceId, isSynced, utcDateChanged) VALUES (?, ?, ?, 0, ?, 'NA', ?, 1, ?)", /* NON-NLS */
	            params, 6);
}

/* NON-NLS-BEGIN: SQL */
static bool note_save(sqlite3 *db, const char *note_id, const char *html)
{
	char instance[16], blob[24], utc[40], local[40];
	if (!tr_db_instance(db, instance))
		return false;
	blob_id(html, blob);
	tr_utc_now(utc, sizeof(utc));
	tr_local_now(local, sizeof(local));

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT title, type, mime, blobId FROM notes WHERE noteId = ?", -1, &st, NULL) !=
	    SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, note_id, -1, SQLITE_STATIC);
	char *title = NULL, *type = NULL, *mime = NULL, *old = NULL;
	if (sqlite3_step(st) == SQLITE_ROW) {
		title = strdup((const char *)sqlite3_column_text(st, 0));
		type = strdup((const char *)sqlite3_column_text(st, 1));
		mime = strdup((const char *)sqlite3_column_text(st, 2));
		const char *b = (const char *)sqlite3_column_text(st, 3);
		old = strdup(b ? b : "");
	}
	sqlite3_finalize(st);
	bool ok = title && type && mime && old;
	if (ok && strcmp(old, blob)) {
		ok = exec(db, "BEGIN IMMEDIATE", NULL, 0);
		/* The blob, when no other note has it already */
		if (ok && !tr_db_int_param(db, "SELECT COUNT(*) FROM blobs WHERE blobId = ?", blob, 0)) {
			const char *params[] = {blob, html, local, utc};
			size_t size = strlen(blob) + strlen(html) + 2;
			char *hashed = malloc(size), hash[32];
			ok = hashed && exec(db, "INSERT INTO blobs (blobId, content, textRepresentation, dateModified,"
			                        " utcDateModified) VALUES (?, ?, NULL, ?, ?)", params, 4);
			if (ok) {
				/* blob.ts' calculateContentHash() */
				snprintf(hashed, size, "%s|%s", blob, html);
				sha1_base64(hashed, hash);
				ok = entity_change(db, "blobs", blob, hash, instance, utc);
			}
			free(hashed);
		}
		/* The note, its hash of BNote's hashedProperties (not protected: edited) */
		if (ok) {
			const char *params[] = {blob, local, utc, note_id};
			ok = exec(db, "UPDATE notes SET blobId = ?, dateModified = ?, utcDateModified = ? WHERE noteId = ?",
			          params, 4);
		}
		if (ok) {
			size_t size = strlen(note_id) + strlen(title) + strlen(type) + strlen(mime) + strlen(blob) + 16;
			char *hashed = malloc(size), hash[32];
			if (hashed) {
				snprintf(hashed, size, "|%s|%s|false|%s|%s|%s", note_id, title, type, mime, blob);
				sha1_base64(hashed, hash);
				hash[10] = '\0';
			}
			ok = hashed && entity_change(db, "notes", note_id, hash, instance, utc);
			free(hashed);
		}
		/* The old blob, unless something else uses it; its change with it */
		if (ok && old[0] &&
		    !tr_db_int_param(db, "SELECT (SELECT COUNT(*) FROM notes WHERE blobId = ?1)"
		                         " + (SELECT COUNT(*) FROM attachments WHERE blobId = ?1)"
		                         " + (SELECT COUNT(*) FROM revisions WHERE blobId = ?1)", old, 1)) {
			const char *params[] = {old};
			ok = exec(db, "DELETE FROM blobs WHERE blobId = ?", params, 1) &&
			     exec(db, "DELETE FROM entity_changes WHERE entityName = 'blobs' AND entityId = ?", params, 1);
		}
		exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, 0);
	}
	free(title);
	free(type);
	free(mime);
	free(old);
	return ok;
}
/* NON-NLS-END */

/* The editor */

struct editor {
	char id[48];
	char *title;
	char *saved; /* the HTML as last saved, or as it was read */
	lv_obj_t *edit, *ta, *toolbar, *kb;
};

static void save(struct editor *ed)
{
	char *html = tr_html_from_doc(ui_rich_edit_doc(ed->edit));
	if (!html || !strcmp(html, ed->saved)) {
		free(html);
		return;
	}
	sqlite3 *db = tr_db_open();
	bool ok = db && note_save(db, ed->id, html);
	sqlite3_close(db);
	if (!ok) {
		fprintf(stderr, "trilium: note %s not saved\n", ed->id);
		free(html);
		return;
	}
	free(ed->saved);
	ed->saved = html;
	tr_note_saves++;
}

static void keyboard_show(struct editor *ed, bool show)
{
	ui_keyboard_show(ed->kb, show);
	lv_obj_set_hidden(ed->toolbar, !show);
	if (show) {
		/* The text shrank: the cursor back in view */
		lv_obj_update_layout(ed->edit);
		ui_rich_edit_show_cursor(ed->edit);
	} else {
		save(ed);
	}
}

static void ta_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		keyboard_show(ed, true);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		keyboard_show(ed, false);
		break;
	default:
		break;
	}
}

/* Left (back): saved */
static void unload_cb(lv_event_t *e)
{
	save(lv_event_get_user_data(e));
}

static void delete_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	free(ed->title);
	free(ed->saved);
	free(ed);
}

void tr_edit_open(const char *note_id, const char *title, struct rich_doc *doc)
{
	struct editor *ed = calloc(1, sizeof(*ed));
	char *saved = tr_html_from_doc(doc);
	if (!ed || !saved) {
		free(ed);
		free(saved);
		rich_doc_free(doc);
		return;
	}
	snprintf(ed->id, sizeof(ed->id), "%s", note_id);
	ed->title = strdup(title);
	/* As it would be written unchanged: an older note's HTML, written otherwise, is
	 * only written once edited */
	ed->saved = saved;

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(ed->title ? ed->title : "", NULL, &body);
	lv_obj_add_event_cb(scr, unload_cb, LV_EVENT_SCREEN_UNLOAD_START, ed);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, ed);
	lv_obj_set_scrollable(body, false);

	ed->edit = ui_rich_edit_create(body, ta_cb, ed);
	ed->ta = ui_rich_edit_textarea(ed->edit);
	lv_obj_set_style_text_font(ed->edit, UI_FONT_BODY_LARGE, 0);
	ui_rich_edit_set_placeholder(ed->edit, _("Note"));
	ui_rich_edit_set_doc(ed->edit, doc);
	lv_obj_set_width(ed->edit, LV_PCT(100));
	lv_obj_set_flex_grow(ed->edit, 1);
	lv_obj_set_style_pad_hor(ed->edit, 28, 0);
	lv_obj_set_style_pad_ver(ed->edit, 16, 0);
	lv_obj_set_scrollbar_mode(ed->edit, LV_SCROLLBAR_MODE_OFF);

	/* Under the text, shown while typing (the toolbar on the keys); the text shrinks above */
	ed->toolbar = ui_rich_toolbar_create(body, ed->edit);
	ui_textarea_set_prose(ed->ta);
	ed->kb = ui_keyboard_create(body, ed->ta);
	/* Opened to write: the keyboard up, the cursor at the end */
	keyboard_show(ed, true);
	ui_push(scr);
}
