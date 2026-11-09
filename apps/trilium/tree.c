/*
 * Trilium: the note tree, see trilium.h. A note's children are the notes its branches
 * place under it, by their position, a branch's prefix before the title as Trilium shows
 * it; the hidden subtree (_hidden: Trilium's own notes) is left out. They are a virtual
 * list (ui_list_create(), its rows as Files'), rows made for those in view only: a
 * journal's thousands of days open at once. A note with children opens them on a screen
 * of its own, as Files' folders, so back goes up, its content above them (note.c's
 * preview, as Trilium shows a note's children under it); the others their content.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"
#include "ui_symbols.h"

#define ROW_H 112

struct note {
	char id[48];
	char *title;
	bool folder; /* has children */
	char icon[8]; /* in tr_icon_font(), or a symbol of the UI's fonts */
	bool symbol;  /* that */
};

/* A list's notes, freed with it */
struct listing {
	unsigned count;
	struct note *notes;
};

static const char *column(sqlite3_stmt *st, int i)
{
	const char *text = (const char *)sqlite3_column_text(st, i);
	return text ? text : "";
}

/* The children of `parent` into `l`; false if they cannot be read (logged) */
static bool listing_load(struct listing *l, sqlite3 *db, const char *parent)
{
	/* NON-NLS-BEGIN: SQL */
	static const char sql[] =
		"SELECT n.noteId, n.title, n.isProtected, n.type, n.mime, b.prefix,"
		" (SELECT a.value FROM attributes a WHERE a.noteId = n.noteId AND a.type = 'label'"
		"  AND a.name = 'iconClass' AND a.isDeleted = 0),"
		" EXISTS (SELECT 1 FROM branches c JOIN notes cn ON cn.noteId = c.noteId"
		"  WHERE c.parentNoteId = n.noteId AND c.isDeleted = 0 AND cn.isDeleted = 0)"
		" FROM branches b JOIN notes n ON n.noteId = b.noteId"
		" WHERE b.parentNoteId = ? AND b.isDeleted = 0 AND n.isDeleted = 0 AND n.noteId != '_hidden'"
		" ORDER BY b.notePosition";
	/* NON-NLS-END */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot list notes: %s\n", sqlite3_errmsg(db));
		return false;
	}
	sqlite3_bind_text(st, 1, parent, -1, SQLITE_STATIC);

	unsigned size = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (l->count == size) {
			size = size ? size * 2 : 32;
			l->notes = realloc(l->notes, size * sizeof(*l->notes));
		}
		struct note *n = &l->notes[l->count++];
		snprintf(n->id, sizeof(n->id), "%s", column(st, 0));
		/* Protected notes' titles are encrypted with a password this app does not ask yet */
		bool protected = sqlite3_column_int(st, 2);
		const char *title = protected ? _("Protected note") : column(st, 1);
		const char *prefix = column(st, 5);
		size_t len = strlen(prefix) + strlen(title) + 4;
		n->title = malloc(len);
		if (prefix[0])
			snprintf(n->title, len, "%s - %s", prefix, title);
		else
			snprintf(n->title, len, "%s", title);
		n->folder = sqlite3_column_int(st, 7);
		n->symbol = protected && !n->folder;
		if (n->symbol)
			snprintf(n->icon, sizeof(n->icon), "%s", UI_SYMBOL_LOCK);
		else
			tr_note_icon(n->icon, (const char *)sqlite3_column_text(st, 6), column(st, 3), column(st, 4), n->folder);
	}
	sqlite3_finalize(st);
	return true;
}

static void listing_delete_cb(lv_event_t *e)
{
	struct listing *l = lv_event_get_user_data(e);
	for (unsigned i = 0; i < l->count; i++)
		free(l->notes[i].title);
	free(l->notes);
	free(l);
}

/* A note's screen: its children */

struct screen {
	char id[48];
	char *title;
};

static void screen_delete_cb(lv_event_t *e)
{
	struct screen *s = lv_event_get_user_data(e);
	free(s->title);
	free(s);
}

void tr_tree_open(const char *note_id, const char *title)
{
	struct screen *s = calloc(1, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", note_id);
	s->title = strdup(title);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(s->title, NULL, &body);
	lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, s);
	sqlite3 *db = tr_db_open();
	if (db) {
		tr_tree_fill(body, db, s->id, s->title);
		sqlite3_close(db);
	} else {
		ui_empty_label(body, _("The database cannot be opened"));
	}
	ui_push(scr);
}

static void note_bind(lv_obj_t *row, unsigned index, void *user_data)
{
	const struct note *n = &((const struct listing *)user_data)->notes[index];
	ui_list_row_set(row, n->icon, n->folder ? UI_COLOR_ACCENT : UI_COLOR_DIM, n->title, NULL, n->folder);
	ui_list_row_set_symbol_font(row, n->symbol ? NULL : tr_icon_font());
}

static void note_click(unsigned index, void *user_data)
{
	const struct note *n = &((const struct listing *)user_data)->notes[index];
	if (n->folder)
		tr_tree_open(n->id, n->title);
	else
		tr_note_open(n->id, n->title);
}

void tr_tree_fill(lv_obj_t *parent, sqlite3 *db, const char *note_id, const char *title)
{
	/* A column of its own that scrolls, as the virtual list wants */
	lv_obj_t *list = ui_box(parent);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_top(list, 8, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_clickable(list, true); /* dragged beside the rows too */
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_row(list, 16, 0);
	if (title)
		tr_note_preview(list, db, note_id, title);

	struct listing *l = calloc(1, sizeof(*l));
	lv_obj_add_event_cb(list, listing_delete_cb, LV_EVENT_DELETE, l);
	bool read = listing_load(l, db, note_id);
	if (!read || !l->count) {
		ui_empty_label(list, read ? _("No notes yet") : _("The notes cannot be read"));
		return;
	}
	lv_obj_t *card = ui_list_create(list, ROW_H, ui_list_row_make, note_bind, note_click, l);
	ui_list_set_count(card, l->count);
}
