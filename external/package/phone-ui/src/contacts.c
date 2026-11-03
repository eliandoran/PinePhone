/*
 * Contacts: alphabetical list and a detail screen with Call and Message.
 */

#include "data.h"

#define ROW_H 112

/* Section letters as static texts */
static const char letters[26][2] = {
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
	"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
};

static void call_cb(lv_event_t *e)
{
	const struct contact *c = lv_event_get_user_data(e);
	phone_dial(c->number);
}

static void message_cb(lv_event_t *e)
{
	const struct contact *c = lv_event_get_user_data(e);
	messages_open_thread(c->number);
}

/* Round colored button with a caption under it */
static void action_create(lv_obj_t *parent, const char *symbol, const char *caption, uint32_t color,
                          lv_event_cb_t cb, const struct contact *c)
{
	lv_obj_t *col = lv_obj_create(parent);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, 160, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	lv_obj_set_clickable(col, false);
	lv_obj_set_scrollable(col, false);

	lv_obj_t *button = lv_button_create(col);
	lv_obj_set_size(button, 120, 120);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)c);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);

	lv_obj_t *label = lv_label_create(col);
	lv_label_set_text_static(label, caption);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
}

static void detail_open(const struct contact *c)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(body, 24, 0);
	lv_obj_set_style_pad_top(body, 24, 0);
	lv_obj_set_scrollable(body, false);

	avatar_create(body, c, 224);

	lv_obj_t *name = lv_label_create(body);
	lv_label_set_text_static(name, c->name);
	lv_obj_set_width(name, LV_PCT(100));
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(name, &ui_font_48, 0);
	lv_obj_set_style_margin_top(name, 32, 0);

	lv_obj_t *actions = lv_obj_create(body);
	lv_obj_remove_style_all(actions);
	lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(actions, 48, 0);
	lv_obj_set_style_margin_top(actions, 48, 0);
	lv_obj_set_clickable(actions, false);
	lv_obj_set_scrollable(actions, false);
	action_create(actions, LV_SYMBOL_CALL, "Call", 0x34c759, call_cb, c);
	action_create(actions, LV_SYMBOL_ENVELOPE, "Message", UI_COLOR_ACCENT, message_cb, c);

	/* The number, tapping it calls too */
	lv_obj_t *card = lv_obj_create(body);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(card, 32, 0);
	lv_obj_set_style_pad_ver(card, 24, 0);
	lv_obj_set_style_pad_row(card, 6, 0);
	lv_obj_set_style_margin_top(card, 56, 0);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_add_event_cb(card, call_cb, LV_EVENT_CLICKED, (void *)c);

	lv_obj_t *kind = lv_label_create(card);
	lv_label_set_text_static(kind, "mobile");
	lv_obj_set_style_text_font(kind, &ui_font_20, 0);
	lv_obj_set_style_text_color(kind, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_t *number = lv_label_create(card);
	lv_label_set_text_static(number, c->number);
	lv_obj_set_style_text_font(number, &ui_font_32, 0);
	lv_obj_set_style_text_color(number, lv_color_hex(UI_COLOR_ACCENT), 0);

	ui_push(scr);
}

static void row_cb(lv_event_t *e)
{
	detail_open(lv_event_get_user_data(e));
}

static lv_obj_t *section_create(lv_obj_t *list, char letter)
{
	lv_obj_t *label = lv_label_create(list);
	int i = letter >= 'A' && letter <= 'Z' ? letter - 'A' : letter >= 'a' && letter <= 'z' ? letter - 'a' : -1;
	lv_label_set_text_static(label, i >= 0 ? letters[i] : "#");
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_pad_left(label, 24, 0);
	lv_obj_set_style_margin_top(label, 16, 0);

	lv_obj_t *card = lv_obj_create(list);
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

static void row_create(lv_obj_t *card, const struct contact *c, bool divider)
{
	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_set_style_pad_column(row, 24, 0);
	lv_obj_set_style_radius(row, 24, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	if (divider) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	}
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)c);

	avatar_create(row, c, 72);
	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text_static(name, c->name);
	lv_obj_set_style_text_font(name, &ui_font_26, 0);
}

void contacts_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Contacts", NULL, &body);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_style_pad_row(body, 8, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

	/* contacts[] is sorted: a section starts where the first letter changes */
	lv_obj_t *card = NULL;
	for (unsigned i = 0; i < contact_count; i++) {
		const struct contact *c = &contacts[i];
		if (!card || c->name[0] != contacts[i - 1].name[0])
			card = section_create(body, c->name[0]);
		bool last = i + 1 == contact_count || contacts[i + 1].name[0] != c->name[0];
		row_create(card, c, !last);
	}

	lv_obj_t *count = lv_label_create(body);
	ui_set_text_fmt(count, "%u contacts", contact_count);
	lv_obj_set_width(count, LV_PCT(100));
	lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(count, &ui_font_20, 0);
	lv_obj_set_style_text_color(count, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(count, 24, 0);

	ui_push(scr);
}
