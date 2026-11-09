/*
 * Trilium: a note's content, see trilium.h. Its blob as the sync stored it: a text
 * note's HTML (Trilium's editor's) laid out as a page on a card (ui_html_create(), the
 * light page Trilium's editor has, drawn dark in the dark theme, as Email's messages);
 * code and other text as written, in monospace. Its images are not fetched yet (they
 * are other notes and attachments); protected notes, encrypted, are not opened, nor
 * what only Trilium shows (canvas, relation maps, files that are not text...).
 * A note with children shows its content above them as Trilium does, the card cut to
 * PREVIEW_H of content and faded out there; tapped, the whole note.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"

#define MAX_HTML_H 16384 /* a note as a page: cut there */
#define MAX_TEXT 65536   /* bytes of code shown */
#define PREVIEW_H 432    /* the content on the card above a note's children */
#define PREVIEW_TEXT 4096
#define FADE_H 96

/* Shown as text: code, and files and notes of a text type */
static bool is_text(const char *type, const char *mime)
{
	/* NON-NLS-BEGIN: Trilium's types, MIME types */
	return !strcmp(type, "code") || !strcmp(type, "mermaid") ||
	       (!strcmp(type, "file") && (!strncmp(mime, "text/", 5) || !strcmp(mime, "application/json")));
	/* NON-NLS-END */
}

static lv_obj_t *html_show(lv_obj_t *body, const char *html, int32_t max_h)
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
	lv_obj_t *page = ui_html_create(card, html, NULL, w, max_h, css);
	ui_html_set_fit(page, true); /* a wide table: shrunk to the card */
	return card;
}

static lv_obj_t *text_show(lv_obj_t *body, char *text, size_t len, size_t max)
{
	/* Cut at a character's start */
	if (len > max) {
		len = max;
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
	if (max == PREVIEW_TEXT)
		lv_obj_set_style_max_height(label, PREVIEW_H, 0); /* cut there: drawn within it */
	return card;
}

/* The note's content into `body` on a card (returned), the start of it for a preview;
 * NULL without any to show, which a note's screen says (*message) */
static lv_obj_t *content_show(lv_obj_t *body, sqlite3 *db, const char *note_id, bool preview, const char **message)
{
	/* NON-NLS-BEGIN: SQL */
	static const char sql[] = "SELECT n.type, n.mime, n.isProtected, b.content FROM notes n"
	                          " LEFT JOIN blobs b ON b.blobId = n.blobId WHERE n.noteId = ?";
	/* NON-NLS-END */
	*message = _("The note cannot be read");
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot read a note: %s\n", sqlite3_errmsg(db));
		return NULL;
	}
	sqlite3_bind_text(st, 1, note_id, -1, SQLITE_STATIC);
	lv_obj_t *card = NULL;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *type = (const char *)sqlite3_column_text(st, 0), *mime = (const char *)sqlite3_column_text(st, 1);
		type = type ? type : "";
		mime = mime ? mime : "";
		bool text = !strcmp(type, "text") || is_text(type, mime); /* NON-NLS */
		/* Text or blob as the sync left it, as bytes */
		const char *content = sqlite3_column_blob(st, 3);
		size_t len = (size_t)sqlite3_column_bytes(st, 3);
		if (sqlite3_column_int(st, 2))
			*message = _("Protected notes cannot be opened yet");
		else if (!text)
			*message = _("This kind of note cannot be shown yet");
		else if (!content || !len)
			*message = _("This note is empty");
		else {
			char *copy = malloc(len + 1);
			memcpy(copy, content, len);
			copy[len] = '\0';
			if (!strcmp(type, "text")) /* NON-NLS */
				card = html_show(body, copy, preview ? PREVIEW_H : MAX_HTML_H);
			else
				card = text_show(body, copy, len, preview ? PREVIEW_TEXT : MAX_TEXT);
			free(copy);
		}
	}
	sqlite3_finalize(st);
	return card;
}

/* A note's screen: its content, an edit button for a text note */

struct screen {
	char id[48];
	char *title;
	lv_obj_t *body, *notice;
	unsigned saves; /* tr_note_saves as it was filled */
};

static void delete_cb(lv_event_t *e)
{
	struct screen *s = lv_event_get_user_data(e);
	free(s->title);
	free(s);
}

static void body_fill(struct screen *s)
{
	lv_obj_clean(s->body);
	s->notice = NULL;
	s->saves = tr_note_saves;
	sqlite3 *db = tr_db_open();
	const char *message = _("The database cannot be opened");
	if (!db || !content_show(s->body, db, s->id, false, &message)) {
		lv_obj_t *label = ui_empty_label(s->body, message);
		lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	}
	sqlite3_close(db);
}

/* Back from the editor: what it saved */
static void loaded_cb(lv_event_t *e)
{
	struct screen *s = lv_event_get_user_data(e);
	if (s->saves != tr_note_saves)
		body_fill(s);
}

/* What it says above the note when it cannot be edited */
static void notice_show(struct screen *s, const char *text)
{
	if (!s->notice) {
		s->notice = ui_dim_label(s->body, "", UI_FONT_BODY);
		lv_obj_set_width(s->notice, LV_PCT(100));
		lv_label_set_long_mode(s->notice, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(s->notice, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_ver(s->notice, 12, 0);
		lv_obj_move_to_index(s->notice, 0);
	}
	lv_label_set_text_static(s->notice, text);
	lv_obj_scroll_to_y(s->body, 0, LV_ANIM_ON);
}

/* Edited in the rich text editor when all of its HTML is what the editor holds */
static void edit_cb(lv_event_t *e)
{
	struct screen *s = lv_event_get_user_data(e);
	sqlite3 *db = tr_db_open();
	char *html = db ? tr_note_html(db, s->id) : NULL;
	sqlite3_close(db);
	struct rich_doc *doc = html ? tr_html_to_doc(html) : NULL;
	free(html);
	if (doc)
		tr_edit_open(s->id, s->title, doc);
	else
		notice_show(s, _("This note has formatting the editor cannot keep yet, so it can only be read here."));
}

void tr_note_open(const char *note_id, const char *title)
{
	struct screen *s = calloc(1, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", note_id);
	s->title = strdup(title);

	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create(s->title, &header, &s->body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, s);
	lv_obj_add_event_cb(scr, loaded_cb, LV_EVENT_SCREEN_LOADED, s);
	lv_obj_set_style_pad_hor(s->body, 16, 0);
	lv_obj_set_style_pad_top(s->body, 8, 0);
	lv_obj_set_style_pad_bottom(s->body, 32, 0);
	/* A text note not protected: its editor */
	sqlite3 *db = tr_db_open();
	if (db && tr_db_int_param(db, "SELECT type = 'text' AND isProtected = 0 FROM notes WHERE noteId = ?", /* NON-NLS */
	                          s->id, 0))
		ui_header_button(header, LV_SYMBOL_EDIT, edit_cb, s);
	sqlite3_close(db);
	tr_sync_indicator(header);
	body_fill(s);
	ui_push(scr);
}

/* The preview */

struct preview {
	char id[48];
	char *title;
};

static void preview_cb(lv_event_t *e)
{
	struct preview *p = lv_event_get_user_data(e);
	tr_note_open(p->id, p->title);
}

static void preview_delete_cb(lv_event_t *e)
{
	struct preview *p = lv_event_get_user_data(e);
	free(p->title);
	free(p);
}

void tr_note_preview(lv_obj_t *parent, sqlite3 *db, const char *note_id, const char *title)
{
	const char *message;
	lv_obj_t *card = content_show(parent, db, note_id, true, &message);
	if (!card)
		return;
	struct preview *p = malloc(sizeof(*p));
	if (!p)
		return;
	snprintf(p->id, sizeof(p->id), "%s", note_id);
	p->title = strdup(title);
	lv_obj_add_event_cb(card, preview_delete_cb, LV_EVENT_DELETE, p);
	lv_obj_add_event_cb(card, preview_cb, LV_EVENT_CLICKED, p);
	lv_obj_set_clickable(card, true);
	lv_obj_set_scrollable(card, false); /* dragged, the list scrolls */

	/* Cut (the page or the text at PREVIEW_H, within the card's padding, clear of its
	 * round corners): faded out into the card's color at the content's bottom */
	lv_obj_update_layout(card);
	lv_obj_t *content = lv_obj_get_child(card, 0);
	if (lv_obj_get_height(content) >= PREVIEW_H) {
		lv_obj_t *fade = ui_box(card);
		lv_obj_set_ignore_layout(fade, true);
		lv_obj_set_size(fade, lv_obj_get_content_width(card), FADE_H);
		lv_obj_align(fade, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(fade, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_style_bg_grad_color(fade, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_grad_opa(fade, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
	}
}
