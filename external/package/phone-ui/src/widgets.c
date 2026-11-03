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
