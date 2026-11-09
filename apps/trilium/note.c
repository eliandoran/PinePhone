/*
 * Trilium: a note's content, see trilium.h. Its blob as the sync stored it: a text
 * note's HTML (Trilium's editor's) laid out as a page on a card (ui_html_create(), the
 * light page Trilium's editor has, drawn dark in the dark theme, as Email's messages);
 * code and other text as written, in monospace. Its images are not fetched yet (they
 * are other notes and attachments); protected notes, encrypted, are not opened, nor
 * what only Trilium shows (canvas, relation maps, files that are not text...).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"

#define MAX_HTML_H 16384 /* a note as a page: cut there */
#define MAX_TEXT 65536   /* bytes of code shown */

struct screen {
	char *title;
};

static void delete_cb(lv_event_t *e)
{
	struct screen *s = lv_event_get_user_data(e);
	free(s->title);
	free(s);
}

/* Shown as text: code, and files and notes of a text type */
static bool is_text(const char *type, const char *mime)
{
	/* NON-NLS-BEGIN: Trilium's types, MIME types */
	return !strcmp(type, "code") || !strcmp(type, "mermaid") ||
	       (!strcmp(type, "file") && (!strncmp(mime, "text/", 5) || !strcmp(mime, "application/json")));
	/* NON-NLS-END */
}

static void html_show(lv_obj_t *body, const char *html)
{
	lv_obj_t *card = ui_card_create(body);
	/* The page nearly fills its card: padded above and below to keep its corners round */
	lv_obj_set_style_pad_ver(card, 24, 0);
	lv_obj_set_style_pad_hor(card, 12, 0);
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 2 * 12;
	char css[1024];
	ui_html_css(css, sizeof(css), lv_color_black(), lv_color_white(), lv_color_hex(UI_COLOR_ACCENT));
	/* Trilium's editor puts tables and images in figures, without a figure's side margins */
	size_t n = strlen(css);
	snprintf(css + n, sizeof(css) - n, "figure { margin: 1em 0; }\n"); /* NON-NLS */
	lv_obj_t *page = ui_html_create(card, html, NULL, w, MAX_HTML_H, css);
	ui_html_set_fit(page, true); /* a wide table: shrunk to the card */
}

static void text_show(lv_obj_t *body, char *text, size_t len)
{
	/* Cut at a character's start */
	if (len > MAX_TEXT) {
		len = MAX_TEXT;
		while (len && ((unsigned char)text[len] & 0xc0) == 0x80)
			len--;
		text[len] = '\0';
	}
	lv_obj_t *card = ui_card_create(body);
	lv_obj_set_style_pad_all(card, 24, 0);
	lv_obj_t *label = lv_label_create(card);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, &ui_font_mono_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_label_set_text(label, text);
}

/* The note's content into `body`, or why there is none */
static void content_show(lv_obj_t *body, sqlite3 *db, const char *note_id)
{
	/* NON-NLS-BEGIN: SQL */
	static const char sql[] = "SELECT n.type, n.mime, n.isProtected, b.content FROM notes n"
	                          " LEFT JOIN blobs b ON b.blobId = n.blobId WHERE n.noteId = ?";
	/* NON-NLS-END */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot read a note: %s\n", sqlite3_errmsg(db));
		ui_empty_label(body, _("The note cannot be read"));
		return;
	}
	sqlite3_bind_text(st, 1, note_id, -1, SQLITE_STATIC);
	const char *message = NULL;
	if (sqlite3_step(st) != SQLITE_ROW) {
		message = _("The note cannot be read");
	} else {
		const char *type = (const char *)sqlite3_column_text(st, 0), *mime = (const char *)sqlite3_column_text(st, 1);
		type = type ? type : "";
		mime = mime ? mime : "";
		bool text = !strcmp(type, "text") || is_text(type, mime); /* NON-NLS */
		/* Text or blob as the sync left it, as bytes */
		const char *content = sqlite3_column_blob(st, 3);
		size_t len = (size_t)sqlite3_column_bytes(st, 3);
		if (sqlite3_column_int(st, 2))
			message = _("Protected notes cannot be opened yet");
		else if (!text)
			message = _("This kind of note cannot be shown yet");
		else if (!content || !len)
			message = _("This note is empty");
		else {
			char *copy = malloc(len + 1);
			memcpy(copy, content, len);
			copy[len] = '\0';
			if (!strcmp(type, "text")) /* NON-NLS */
				html_show(body, copy);
			else
				text_show(body, copy, len);
			free(copy);
		}
	}
	sqlite3_finalize(st);
	if (message) {
		lv_obj_t *label = ui_empty_label(body, message);
		lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	}
}

void tr_note_open(const char *note_id, const char *title)
{
	struct screen *s = calloc(1, sizeof(*s));
	s->title = strdup(title);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(s->title, NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, s);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	sqlite3 *db = tr_db_open();
	if (db) {
		content_show(body, db, note_id);
		sqlite3_close(db);
	} else {
		ui_empty_label(body, _("The database cannot be opened"));
	}
	ui_push(scr);
}
