/*
 * Trilium: the app's screen, see trilium.h. For now what the database holds: how
 * many notes, its size and SQLite's version.
 */

#include "trilium.h"

void tr_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Trilium", NULL, &body); /* NON-NLS */
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	sqlite3 *db = tr_db_open();
	if (!db) {
		ui_empty_label(body, _("The database cannot be opened"));
		ui_push(scr);
		return;
	}
	/* NON-NLS-BEGIN: SQL */
	long long notes = tr_db_int(db, "SELECT COUNT(*) FROM notes WHERE isDeleted = 0", 0);
	long long bytes = tr_db_int(db, "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()", 0);
	/* NON-NLS-END */
	sqlite3_close(db);

	lv_obj_t *count = ui_empty_label(body, "");
	lv_obj_set_style_text_color(count, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(count, UI_FONT_TITLE, 0);
	if (notes)
		ui_set_text_fmt(count, P_("%lld note", "%lld notes", notes), notes);
	else
		lv_label_set_text_static(count, _("No notes yet"));

	char size[16];
	ui_size_format(size, sizeof(size), bytes > 0 ? (unsigned long long)bytes : 0);
	lv_obj_t *details = ui_dim_label(body, "", UI_FONT_CAPTION);
	lv_obj_set_style_margin_top(details, 12, 0);
	ui_set_text_fmt(details, _("Database %s, SQLite %s"), size, sqlite3_libversion());
	ui_push(scr);
}
