/*
 * Widgets and formatting shared by the apps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

lv_obj_t *ui_section_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = ui_dim_label(parent, text, &ui_font_20);
	/* In line with the rows' content */
	lv_obj_set_style_pad_left(label, 24, 0);
	lv_obj_set_style_margin_top(label, 16, 0);
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

lv_obj_t *ui_slider_create(lv_obj_t *parent)
{
	lv_obj_t *slider = lv_slider_create(parent);
	lv_obj_set_height(slider, 14);
	lv_obj_set_flex_grow(slider, 1);
	lv_obj_set_ext_click_area(slider, 36);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	/* On the light theme's white cards and shade, the edge shows it */
	lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
	lv_obj_set_style_border_color(slider, lv_color_hex(UI_COLOR_KNOB_EDGE), LV_PART_KNOB);
	lv_obj_set_style_pad_all(slider, 16, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
	return slider;
}

lv_obj_t *ui_switch_create(lv_obj_t *parent, bool on)
{
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_size(sw, 100, 56);
	lv_obj_set_ext_click_area(sw, 16);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COLOR_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUSED);
	lv_obj_set_state(sw, LV_STATE_CHECKED, on);
	return sw;
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

char *ui_one_line(const char *prefix, const char *original, size_t max)
{
	char *buf = malloc(max + 1);
	if (!buf)
		return NULL;
	size_t n = snprintf(buf, max + 1, "%s", prefix);
	if (n > max)
		n = max;
	bool space = false;
	/* Emoji sequences first: not cut in the middle */
	char *composed = ui_emoji_compose(original);
	const char *text = composed ? composed : "";
	while (*text) {
		if (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r') {
			space = n > 0;
			text++;
			continue;
		}
		/* Whole characters only */
		unsigned char c = *text;
		size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		if (n + space + len > max || strnlen(text, len) < len)
			break;
		if (space)
			buf[n++] = ' ';
		space = false;
		memcpy(buf + n, text, len);
		n += len;
		text += len;
	}
	buf[n] = '\0';
	free(composed);
	return buf;
}

/* Conversations */

void ui_chat_scroll_end(struct ui_chat *chat, lv_anim_enable_t anim)
{
	lv_obj_update_layout(chat->list);
	lv_obj_scroll_to_y(chat->list, lv_obj_get_scroll_y(chat->list) + lv_obj_get_scroll_bottom(chat->list), anim);
}

static void chat_keyboard(struct ui_chat *chat, bool show)
{
	ui_keyboard_show(chat->kb, show);
	ui_chat_scroll_end(chat, LV_ANIM_OFF);
}

static void chat_send(struct ui_chat *chat)
{
	const char *text = lv_textarea_get_text(chat->ta);
	while (*text == ' ' || *text == '\n')
		text++;
	size_t n = strlen(text);
	while (n && (text[n - 1] == ' ' || text[n - 1] == '\n'))
		n--;
	/* Copied first: hiding the keyboard unfocuses the text area, whose label then
	 * sets its text again (reallocated), with Enter while it has the focus */
	char *trimmed = n ? strndup(text, n) : NULL;
	chat_keyboard(chat, false);
	if (!trimmed)
		return;
	lv_textarea_set_text(chat->ta, "");
	ui_vibrate(15);
	chat->send(chat, trimmed);
	free(trimmed);
}

static void chat_send_cb(lv_event_t *e)
{
	chat_send(lv_event_get_user_data(e));
}

static void chat_ta_cb(lv_event_t *e)
{
	struct ui_chat *chat = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		chat_keyboard(chat, true);
		break;
	case LV_EVENT_READY: /* the keyboard's Enter key */
		chat_send(chat);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		chat_keyboard(chat, false);
		break;
	default:
		break;
	}
}

static void chat_click_cb(lv_event_t *e)
{
	struct ui_chat *chat = lv_event_get_user_data(e);
	if (!lv_obj_is_hidden(chat->kb))
		chat_keyboard(chat, false);
}

void ui_chat_create(struct ui_chat *chat, lv_obj_t *body, uint32_t max_length)
{
	lv_obj_set_scrollable(body, false);
	chat->list = lv_obj_create(body);
	lv_obj_remove_style_all(chat->list);
	lv_obj_set_width(chat->list, LV_PCT(100));
	lv_obj_set_flex_grow(chat->list, 1);
	lv_obj_set_flex_flow(chat->list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(chat->list, 16, 0);
	lv_obj_set_style_pad_ver(chat->list, 16, 0);
	lv_obj_set_style_pad_row(chat->list, 24, 0);
	lv_obj_set_scroll_dir(chat->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(chat->list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(chat->list, chat_click_cb, LV_EVENT_CLICKED, chat);

	chat->ta = ui_compose_bar(body, max_length, chat_ta_cb, chat_send_cb, chat);
	/* Under the bar, shown while typing; the messages shrink above it */
	chat->kb = ui_keyboard_create(body, chat->ta);
}

lv_obj_t *ui_chat_group_create(struct ui_chat *chat, bool outgoing, lv_obj_t **time)
{
	lv_obj_t *group = ui_box(chat->list);
	lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START,
	                      outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
	                      outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(group, 6, 0);

	lv_obj_t *label = lv_label_create(group);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, &ui_font_16, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_hor(label, 12, 0);
	lv_obj_set_style_margin_top(label, 2, 0);
	if (time)
		*time = label;
	return group;
}

lv_obj_t *ui_chat_bubble_create(lv_obj_t *group, bool outgoing)
{
	lv_obj_t *bubble = ui_box(group);
	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bubble, 24, 0);
	lv_obj_set_style_pad_ver(bubble, 16, 0);
	lv_obj_set_style_radius(bubble, 24, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bubble, lv_color_hex(outgoing ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
	/* Before the time */
	lv_obj_move_to_index(bubble, (int32_t)lv_obj_get_child_count(group) - 2);

	/* Wraps at 75% of the screen (bubble padding included) */
	lv_obj_t *label = lv_label_create(bubble);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_max_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) * 3 / 4 - 48, 0);
	lv_obj_set_style_text_color(label, outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
	return label;
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

lv_obj_t *ui_avatar_create(lv_obj_t *parent, int32_t size)
{
	lv_obj_t *avatar = ui_box(parent);
	lv_obj_set_size(avatar, size, size);
	lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
	/* On the avatar, so that it can be changed there */
	lv_obj_set_style_text_font(avatar, size >= 160 ? &ui_font_48 :
	                           size >= 96 ? &ui_font_32 :
	                           size >= 72 ? &ui_font_26 : &ui_font_20, 0);
	lv_obj_set_style_text_color(avatar, lv_color_white(), 0);

	lv_obj_t *text = lv_label_create(avatar);
	lv_label_set_text_static(text, "");
	lv_obj_center(text);
#if LV_USE_3DTEXTURE
	lv_obj_t *photo = lv_3dtexture_create(avatar);
	lv_obj_set_size(photo, size, size);
	lv_obj_set_clickable(photo, false);
	lv_obj_set_hidden(photo, true);
#endif
	return avatar;
}

void ui_avatar_set(lv_obj_t *avatar, uint32_t color, const char *text)
{
	lv_obj_set_style_bg_color(avatar, lv_color_hex(color), 0);
	ui_set_text(lv_obj_get_child(avatar, 0), text);
}

void ui_avatar_set_photo(lv_obj_t *avatar, unsigned tex)
{
#if LV_USE_3DTEXTURE
	lv_obj_t *photo = lv_obj_get_child(avatar, 1);
	lv_obj_set_hidden(photo, !tex);
	if (tex)
		lv_3dtexture_set_src(photo, tex);
	/* Behind the photo's soft edge: no colored fringe */
	lv_obj_set_hidden(lv_obj_get_child(avatar, 0), tex);
	lv_obj_set_style_bg_opa(avatar, tex ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
#else
	LV_UNUSED(avatar);
	LV_UNUSED(tex);
#endif
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

void ui_offset_format(char *buf, size_t size, long seconds)
{
	int off = (int)(seconds / 60 % (24 * 60)); /* minutes */
	char sign = off < 0 ? '-' : '+';
	if (off < 0)
		off = -off;
	if (off == 0)
		snprintf(buf, size, "UTC");
	else if (off % 60)
		snprintf(buf, size, "UTC%c%d:%02d", sign, off / 60, off % 60);
	else
		snprintf(buf, size, "UTC%c%d", sign, off / 60);
}

void ui_tz_localtime(const char *tz, time_t t, struct tm *tm)
{
	const char *cur = getenv("TZ");
	char *saved = cur ? strdup(cur) : NULL;

	setenv("TZ", tz, 1);
	tzset();
	localtime_r(&t, tm);
	if (saved) {
		setenv("TZ", saved, 1);
		free(saved);
	} else {
		unsetenv("TZ");
	}
	tzset();
}

/* Glyphs: strokes a twelfth of the size */

static void dot_draw(lv_layer_t *layer, lv_color_t color, int32_t x, int32_t y, int32_t r)
{
	lv_draw_arc_dsc_t arc;
	lv_draw_arc_dsc_init(&arc);
	arc.color = color;
	arc.center = (lv_point_t){ x, y };
	arc.radius = r;
	arc.width = r;
	arc.start_angle = 0;
	arc.end_angle = 360;
	lv_draw_arc(layer, &arc);
}

static void glyph_draw_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	enum ui_glyph glyph = (enum ui_glyph)(uintptr_t)lv_obj_get_user_data(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t size = lv_area_get_width(&a);
	lv_point_t c = { a.x1 + size / 2, a.y1 + size / 2 };
	lv_color_t color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
	int32_t w = LV_MAX(size / 12, 2);
	int32_t r = size / 2 - w / 2 - 1; /* ring, to the middle of its stroke */

	if (glyph == UI_GLYPH_DIALPAD) {
		/* 3 x 3 keys and the 0 under the middle one */
		int32_t step = size * 6 / 25, dot = LV_MAX(size / 15, 2);
		for (int i = 0; i < 10; i++) {
			int col = i < 9 ? i % 3 : 1, row = i / 3;
			dot_draw(layer, color, c.x + (col - 1) * step, c.y + (2 * row - 3) * step / 2, dot);
		}
		return;
	}

	lv_draw_arc_dsc_t arc;
	if (glyph == UI_GLYPH_PERSON) {
		/* A head, and shoulders: the top half of a disc */
		dot_draw(layer, color, c.x, c.y - size / 5, size * 5 / 24);
		lv_draw_arc_dsc_init(&arc);
		arc.color = color;
		arc.center = (lv_point_t){ c.x, a.y1 + size * 24 / 25 };
		arc.radius = size * 2 / 5;
		arc.width = arc.radius;
		arc.start_angle = 180;
		arc.end_angle = 360;
		lv_draw_arc(layer, &arc);
		return;
	}

	lv_draw_arc_dsc_init(&arc);
	arc.color = color;
	arc.center = c;
	arc.radius = r + w / 2;
	arc.width = w;
	arc.start_angle = 0;
	arc.end_angle = 360;
	lv_draw_arc(layer, &arc);

	lv_draw_line_dsc_t line;
	lv_draw_line_dsc_init(&line);
	line.color = color;
	line.width = w;
	line.round_start = line.round_end = 1;
	if (glyph == UI_GLYPH_CLOCK) {
		/* The hands at three o'clock */
		line.p1 = (lv_point_precise_t){ c.x, c.y };
		line.p2 = (lv_point_precise_t){ c.x, c.y - r * 3 / 5 };
		lv_draw_line(layer, &line);
		line.p2 = (lv_point_precise_t){ c.x + r * 2 / 5, c.y };
		lv_draw_line(layer, &line);
	} else {
		/* An "i": its dot, then its stem */
		dot_draw(layer, color, c.x, c.y - r * 9 / 20, w * 3 / 4);
		line.p1 = (lv_point_precise_t){ c.x, c.y - r / 10 };
		line.p2 = (lv_point_precise_t){ c.x, c.y + r / 2 };
		lv_draw_line(layer, &line);
	}
}

lv_obj_t *ui_glyph_create(lv_obj_t *parent, int32_t size, enum ui_glyph glyph)
{
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_size(obj, size, size);
	lv_obj_set_user_data(obj, (void *)(uintptr_t)glyph);
	lv_obj_add_event_cb(obj, glyph_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	return obj;
}
