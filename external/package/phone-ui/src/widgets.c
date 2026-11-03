/*
 * Widgets and formatting shared by the apps.
 */

#include <time.h>

#include "ui.h"

lv_obj_t *ui_one_line_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	/* Clipped to one line: no dots, they would be written into the static text */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

lv_obj_t *ui_empty_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(label, 200, 0);
	return label;
}

lv_obj_t *ui_card_create(lv_obj_t *parent)
{
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_clickable(card, false);
	lv_obj_set_scrollable(card, false);
	return card;
}

lv_obj_t *ui_box(lv_obj_t *parent)
{
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_remove_style_all(box);
	lv_obj_set_clickable(box, false);
	lv_obj_set_scrollable(box, false);
	return box;
}

lv_obj_t *ui_row_create(lv_obj_t *card, int32_t height, bool divider, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *row = ui_box(card);
	lv_obj_set_size(row, LV_PCT(100), height);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(row, 24, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	/* The divider is a border without radius, see ui.h */
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	ui_row_set_divider(row, divider);
	if (cb) {
		lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_clickable(row, true);
		lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
	}
	return row;
}

void ui_row_set_divider(lv_obj_t *row, bool divider)
{
	lv_obj_set_style_border_width(row, divider ? 1 : 0, 0);
}

lv_obj_t *ui_row_text(lv_obj_t *row)
{
	lv_obj_t *text = ui_box(row);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 6, 0);
	return text;
}

lv_obj_t *ui_row_line(lv_obj_t *text)
{
	lv_obj_t *line = ui_box(text);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 16, 0);
	return line;
}

lv_obj_t *ui_dim_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

void ui_when_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	const char *fmt = tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday ? "%H:%M" :
	                  now - t < 6 * 24 * 3600 ? "%a" :
	                  tm.tm_year == today.tm_year ? "%-d %b" : "%-d %b %Y";
	strftime(buf, size, fmt, &tm);
}
