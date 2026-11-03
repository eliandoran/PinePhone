/*
 * Widgets and formatting shared by the apps.
 */

#include <stdio.h>
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

lv_obj_t *ui_button(lv_obj_t *parent, int32_t w, int32_t h, uint32_t color, uint32_t pressed,
                    const char *text, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *button = lv_obj_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, w, h);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	ui_button_set_color(button, color, pressed);
	lv_obj_set_scrollable(button, false);
	if (cb)
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_center(label);
	return button;
}

void ui_button_set_color(lv_obj_t *button, uint32_t color, uint32_t pressed)
{
	lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(pressed), LV_STATE_PRESSED);
	/* White on colors, the text color on grays */
	lv_obj_set_style_text_color(button, color == UI_COLOR_BUTTON || color == UI_COLOR_CARD ?
	                            lv_color_hex(UI_COLOR_TEXT) : lv_color_white(), 0);
}

lv_obj_t *ui_bar_create(lv_obj_t *parent)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_column(bar, 12, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);
	return bar;
}

lv_obj_t *ui_textarea_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *ta = lv_textarea_create(parent);
	lv_obj_set_style_text_color(ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(ta, 0, 0);
	lv_obj_set_style_border_width(ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(ta, 0, 0);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_CLICKED, user_data);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_READY, user_data);
	lv_obj_add_event_cb(ta, cb, LV_EVENT_CANCEL, user_data);
	return ta;
}

lv_obj_t *ui_text_field(lv_obj_t *parent, int32_t height, uint32_t max_length, lv_event_cb_t cb,
                        void *user_data)
{
	lv_obj_t *ta = ui_textarea_create(parent, cb, user_data);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_max_length(ta, max_length);
	lv_obj_set_height(ta, height);
	lv_obj_set_style_radius(ta, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_pad_hor(ta, 28, 0);
	/* The line of text centered */
	int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(ta, LV_PART_MAIN));
	lv_obj_set_style_pad_ver(ta, (height - line) / 2, 0);
	lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COLOR_CARD), 0);
	return ta;
}

lv_obj_t *ui_compose_bar(lv_obj_t *parent, uint32_t max_length, lv_event_cb_t ta_cb,
                         lv_event_cb_t send_cb, void *user_data)
{
	lv_obj_t *bar = ui_bar_create(parent);
	lv_obj_t *ta = ui_text_field(bar, 80, max_length, ta_cb, user_data);
	lv_textarea_set_placeholder_text(ta, "Message");
	lv_obj_set_flex_grow(ta, 1);

	lv_obj_t *send = ui_button(bar, 80, 80, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, LV_SYMBOL_UP,
	                           send_cb, user_data);
	lv_obj_set_style_text_font(lv_obj_get_child(send, 0), &ui_font_32, 0);
	return ta;
}

lv_obj_t *ui_confirm_bar(lv_obj_t *parent, const char *action, lv_event_cb_t confirm_cb,
                         lv_event_cb_t cancel_cb, void *user_data)
{
	lv_obj_t *bar = ui_bar_create(parent);
	lv_obj_t *cancel = ui_button(bar, LV_SIZE_CONTENT, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                             "Cancel", cancel_cb, user_data);
	lv_obj_set_flex_grow(cancel, 1);
	lv_obj_t *confirm = ui_button(bar, LV_SIZE_CONTENT, 80, UI_COLOR_RED, UI_COLOR_RED_PRESSED,
	                              action, confirm_cb, user_data);
	lv_obj_set_flex_grow(confirm, 1);
	lv_obj_set_hidden(bar, true);
	return bar;
}

void ui_size_format(char *buf, size_t size, unsigned long long bytes)
{
	static const char *const units[] = { "kB", "MB", "GB", "TB" };
	if (bytes < 1000) {
		snprintf(buf, size, "%llu B", bytes);
		return;
	}
	double v = bytes / 1000.0;
	unsigned u = 0;
	/* As rounded: 999.6 kB is 1.0 MB, not 1000 kB */
	while (v >= 999.5 && u + 1 < sizeof(units) / sizeof(units[0])) {
		v /= 1000;
		u++;
	}
	snprintf(buf, size, v < 9.95 ? "%.1f %s" : "%.0f %s", v, units[u]);
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
