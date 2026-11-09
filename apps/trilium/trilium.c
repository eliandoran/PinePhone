/*
 * Trilium: the app's screen, see trilium.h. Setting up sync until the database has a
 * server and the document's secret (setup.c); then what the database holds: how many
 * notes, the server, its size and SQLite's version.
 */

#include <string.h>

#include "trilium.h"

static lv_obj_t *body;

static void document_show(sqlite3 *db)
{
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(body, 16, 0);

	char host[600];
	tr_db_option(db, "syncServerHost", host, sizeof(host));
	/* NON-NLS-BEGIN: SQL */
	long long notes = tr_db_int(db, "SELECT COUNT(*) FROM notes WHERE isDeleted = 0", 0);
	long long bytes = tr_db_int(db, "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()", 0);
	/* NON-NLS-END */

	lv_obj_t *count = ui_empty_label(body, "");
	lv_obj_set_style_text_color(count, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(count, UI_FONT_TITLE, 0);
	if (notes)
		ui_set_text_fmt(count, P_("%lld note", "%lld notes", notes), notes);
	else
		lv_label_set_text_static(count, _("No notes yet"));

	const char *scheme = strstr(host, "://");
	lv_obj_t *server = ui_dim_label(body, "", UI_FONT_BODY);
	lv_obj_set_width(server, LV_PCT(100));
	lv_label_set_long_mode(server, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(server, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_margin_top(server, 12, 0);
	ui_set_text_fmt(server, _("Syncs with %s"), scheme ? scheme + 3 : host);

	char size[16];
	ui_size_format(size, sizeof(size), bytes > 0 ? (unsigned long long)bytes : 0);
	lv_obj_t *details = ui_dim_label(body, "", UI_FONT_CAPTION);
	lv_obj_set_style_margin_top(details, 12, 0);
	ui_set_text_fmt(details, _("Database %s, SQLite %s"), size, sqlite3_libversion());
}

void tr_show(void)
{
	lv_obj_clean(body);
	sqlite3 *db = tr_db_open();
	if (!db) {
		ui_empty_label(body, _("The database cannot be opened"));
		return;
	}
	char secret[128];
	tr_db_option(db, "documentSecret", secret, sizeof(secret));
	if (secret[0])
		document_show(db);
	else
		tr_setup_create(body);
	sqlite3_close(db);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	body = NULL;
}

void tr_open(void)
{
	lv_obj_t *scr = ui_app_screen_create("Trilium", NULL, &body); /* NON-NLS */
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	tr_show();
	ui_push(scr);
}
