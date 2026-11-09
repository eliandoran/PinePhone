/*
 * Trilium: the note tree, see trilium.h. A note's children are the notes its branches
 * place under it, by their position, a branch's prefix before the title as Trilium shows
 * it; the hidden subtree (_hidden: Trilium's own notes) is left out. A note with children
 * opens them on a screen of its own, as Files' folders, so back goes up; the others
 * their content (note.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"
#include "ui_symbols.h"

#define ROW_H 112
#define MAX_NOTES 500 /* rows on a screen (a journal's year can have more) */

struct note {
	char id[48];
	char *title;
	bool folder; /* has children */
	const char *symbol;
};

/* What a listing's rows point into, freed with its card */
struct listing {
	unsigned count;
	struct note notes[];
};

/* A note's symbol by its type, as Trilium tells them apart */
static const char *note_symbol(const char *type, const char *mime)
{
	/* NON-NLS-BEGIN: Trilium's types */
	if (!strcmp(type, "code") || !strcmp(type, "mermaid"))
		return UI_SYMBOL_CODE;
	if (!strcmp(type, "image") || !strncmp(mime, "image/", 6))
		return LV_SYMBOL_IMAGE;
	if (!strncmp(mime, "audio/", 6))
		return LV_SYMBOL_AUDIO;
	if (!strncmp(mime, "video/", 6))
		return LV_SYMBOL_VIDEO;
	if (!strcmp(type, "book"))
		return LV_SYMBOL_LIST;
	/* NON-NLS-END */
	return LV_SYMBOL_FILE;
}

static const char *column(sqlite3_stmt *st, int i)
{
	const char *text = (const char *)sqlite3_column_text(st, i);
	return text ? text : "";
}

/* The children of `parent`, at most MAX_NOTES (*more: how many were left out); NULL if
 * they cannot be read (logged) */
static struct listing *listing_load(sqlite3 *db, const char *parent, unsigned *more)
{
	/* NON-NLS-BEGIN: SQL */
	static const char sql[] =
		"SELECT n.noteId, n.title, n.isProtected, n.type, n.mime, b.prefix,"
		" EXISTS (SELECT 1 FROM branches c JOIN notes cn ON cn.noteId = c.noteId"
		"  WHERE c.parentNoteId = n.noteId AND c.isDeleted = 0 AND cn.isDeleted = 0)"
		" FROM branches b JOIN notes n ON n.noteId = b.noteId"
		" WHERE b.parentNoteId = ? AND b.isDeleted = 0 AND n.isDeleted = 0 AND n.noteId != '_hidden'"
		" ORDER BY b.notePosition";
	/* NON-NLS-END */
	*more = 0;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot list notes: %s\n", sqlite3_errmsg(db));
		return NULL;
	}
	sqlite3_bind_text(st, 1, parent, -1, SQLITE_STATIC);

	unsigned size = 16;
	struct listing *l = malloc(sizeof(*l) + size * sizeof(l->notes[0]));
	l->count = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (l->count == MAX_NOTES) {
			(*more)++;
			continue;
		}
		if (l->count == size) {
			size *= 2;
			l = realloc(l, sizeof(*l) + size * sizeof(l->notes[0]));
		}
		struct note *n = &l->notes[l->count++];
		snprintf(n->id, sizeof(n->id), "%s", column(st, 0));
		/* Protected notes' titles are encrypted with a password this app does not ask yet */
		const char *title = sqlite3_column_int(st, 2) ? _("Protected note") : column(st, 1);
		const char *prefix = column(st, 5);
		size_t len = strlen(prefix) + strlen(title) + 4;
		n->title = malloc(len);
		if (prefix[0])
			snprintf(n->title, len, "%s - %s", prefix, title);
		else
			snprintf(n->title, len, "%s", title);
		n->folder = sqlite3_column_int(st, 6);
		n->symbol = sqlite3_column_int(st, 2) ? UI_SYMBOL_LOCK : note_symbol(column(st, 3), column(st, 4));
	}
	sqlite3_finalize(st);
	return l;
}

static void listing_free(struct listing *l)
{
	for (unsigned i = 0; i < l->count; i++)
		free(l->notes[i].title);
	free(l);
}

static void card_delete_cb(lv_event_t *e)
{
	listing_free(lv_event_get_user_data(e));
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

static void note_open(const struct note *note)
{
	struct screen *s = calloc(1, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", note->id);
	s->title = strdup(note->title);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(s->title, NULL, &body);
	lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, s);
	sqlite3 *db = tr_db_open();
	if (db) {
		tr_tree_fill(body, db, s->id);
		sqlite3_close(db);
	} else {
		ui_empty_label(body, _("The database cannot be opened"));
	}
	ui_push(scr);
}

static void row_cb(lv_event_t *e)
{
	const struct note *note = lv_event_get_user_data(e);
	if (note->folder)
		note_open(note);
	else
		tr_note_open(note->id, note->title);
}

static void row_create(lv_obj_t *card, const struct note *note, bool divider)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, (void *)note);

	lv_obj_t *icon = lv_label_create(row);
	lv_label_set_text_static(icon, note->folder ? LV_SYMBOL_DIRECTORY : note->symbol);
	lv_obj_set_width(icon, 48);
	lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(note->folder ? UI_COLOR_ACCENT : UI_COLOR_DIM), 0);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *title = ui_one_line_label(text, note->title, UI_FONT_TITLE);
	lv_obj_set_width(title, LV_PCT(100));

	if (note->folder) {
		lv_obj_t *arrow = lv_label_create(row);
		lv_label_set_text_static(arrow, LV_SYMBOL_RIGHT);
		lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
	}
}

void tr_tree_fill(lv_obj_t *parent, sqlite3 *db, const char *note_id)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(col, 16, 0);
	lv_obj_set_style_pad_top(col, 8, 0);
	lv_obj_set_style_pad_bottom(col, 32, 0);
	lv_obj_set_style_pad_row(col, 16, 0);

	unsigned more;
	struct listing *l = listing_load(db, note_id, &more);
	if (!l || !l->count) {
		ui_empty_label(col, l ? _("No notes yet") : _("The notes cannot be read"));
		if (l)
			listing_free(l);
		return;
	}
	lv_obj_t *card = ui_card_create(col);
	lv_obj_add_event_cb(card, card_delete_cb, LV_EVENT_DELETE, l);
	for (unsigned i = 0; i < l->count; i++)
		row_create(card, &l->notes[i], i + 1 < l->count);
	if (more) {
		lv_obj_t *label = ui_dim_label(col, "", UI_FONT_CAPTION);
		lv_obj_set_width(label, LV_PCT(100));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
		ui_set_text_fmt(label, P_("%u more not shown", "%u more not shown", more), more);
	}
}
