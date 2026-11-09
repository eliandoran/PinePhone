/*
 * The rich text editor (rich_edit.c): a rich_doc drawn in a view that scrolls, typed into
 * through a text area of its own, out of sight, which the keyboard takes
 * (ui_keyboard_create()): its edits are applied to the document, what is typed taking
 * the marks of the toolbar (ui_rich_toolbar_create()).
 */

#ifndef RICH_EDIT_H
#define RICH_EDIT_H

#include "lvgl.h"
#include "ui/rich_text.h"

extern const lv_obj_class_t ui_rich_edit_class;

/* `cb` gets LV_EVENT_CLICKED (the text tapped: the cursor put there), and READY and
 * CANCEL from the keyboard, as ui_textarea_create()'s. The text's font is the view's. */
lv_obj_t *ui_rich_edit_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data);
/* The text area typed into: for the keyboard, its text and cursor */
lv_obj_t *ui_rich_edit_textarea(lv_obj_t *edit);
struct rich_doc *ui_rich_edit_doc(lv_obj_t *edit);
/* The text, the blocks joined by '\n' */
const char *ui_rich_edit_text(lv_obj_t *edit);
void ui_rich_edit_set_placeholder(lv_obj_t *edit, const char *text);
/* Scrolled to show the cursor (after the view shrank) */
void ui_rich_edit_show_cursor(lv_obj_t *edit);

/* Bold, italic and underline for what is typed next, checked as at the cursor; a bullet
 * or numbered list for the cursor's line */
lv_obj_t *ui_rich_toolbar_create(lv_obj_t *parent, lv_obj_t *edit);

#endif
