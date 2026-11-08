/*
 * Conversations shared by the chat apps (Messages, Telegram, Assistant, GitHub).
 */

#include <stdlib.h>
#include <string.h>

#include "ui.h"

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
	ui_vibrate(UI_VIBRATE_TAP);
	chat->send(chat, trimmed);
	free(trimmed);
	ui_chat_reply(chat, 0, NULL, NULL);
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

/* The list shrinking (the field grew a line): what was at its bottom stays there */
static void chat_list_size_cb(lv_event_t *e)
{
	lv_obj_t *list = lv_event_get_current_target_obj(e);
	const lv_area_t *was = lv_event_get_param(e);
	int32_t shrunk = lv_area_get_height(was) - lv_obj_get_height(list);
	int32_t below = lv_obj_get_scroll_bottom(list);
	if (shrunk > 0 && below > 0)
		lv_obj_scroll_by(list, 0, -LV_MIN(shrunk, below), LV_ANIM_OFF);
}

static void chat_click_cb(lv_event_t *e)
{
	struct ui_chat *chat = lv_event_get_user_data(e);
	if (!lv_obj_is_hidden(chat->kb))
		chat_keyboard(chat, false);
}

/* Replies */

#define REPLY_SWIPE 120    /* px a message is swiped left to reply */
#define REPLY_TEXT_MAX 200 /* bytes of the message answered, on one line */

/* Who wrote it in `color`, its text on one line under it, after a bar of that color */
static lv_obj_t *quote_create(lv_obj_t *parent, lv_color_t color, lv_color_t text_color, int32_t width,
                              lv_obj_t **name, lv_obj_t **text)
{
	lv_obj_t *quote = ui_box(parent);
	lv_obj_set_size(quote, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(quote, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_border_side(quote, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_border_width(quote, 4, 0);
	lv_obj_set_style_border_color(quote, color, 0);
	lv_obj_set_style_border_opa(quote, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_left(quote, 16, 0);

	lv_obj_t **labels[] = { name, text };
	for (unsigned i = 0; i < 2; i++) {
		lv_obj_t *label = *labels[i] = lv_label_create(quote);
		lv_label_set_text_static(label, "");
		lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
		lv_obj_set_style_text_color(label, i ? text_color : color, 0);
		/* One line, dots where it is cut */
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_max_width(label, width, 0);
		lv_obj_set_height(label, lv_font_get_line_height(UI_FONT_CAPTION));
	}
	return quote;
}

static void quote_set(lv_obj_t *name, lv_obj_t *text, const char *who, const char *what)
{
	lv_label_set_text(name, who ? who : "");
	char *line = ui_one_line("", what ? what : "", REPLY_TEXT_MAX);
	lv_label_set_text(text, line ? line : "");
	free(line);
}

static void reply_cancel_cb(lv_event_t *e)
{
	ui_chat_reply(lv_event_get_user_data(e), 0, NULL, NULL);
}

/* Above the compose bar, hidden until replying */
static lv_obj_t *reply_bar_create(struct ui_chat *chat, lv_obj_t *body)
{
	lv_obj_t *bar = ui_bar_create(body);
	lv_obj_set_height(bar, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(bar, 12, 0);
	/* The screen's width but the bar's padding, its gap and the button */
	int32_t width = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 16 - 12 - 80;
	lv_obj_t *quote = quote_create(bar, lv_color_hex(UI_COLOR_ACCENT), lv_color_hex(UI_COLOR_TEXT), width,
	                               &chat->reply_name, &chat->reply_text);
	lv_obj_set_flex_grow(quote, 1);
	lv_obj_t *cancel = ui_button(bar, 80, 80, UI_COLOR_BAR, UI_COLOR_PRESSED, LV_SYMBOL_CLOSE,
	                             reply_cancel_cb, chat);
	lv_obj_set_style_text_color(lv_obj_get_child(cancel, 0), lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_hidden(bar, true);
	return bar;
}

void ui_chat_reply(struct ui_chat *chat, long long id, const char *name, const char *text)
{
	chat->reply_to = id;
	lv_obj_set_hidden(chat->reply_bar, !id);
	if (!id)
		return;
	quote_set(chat->reply_name, chat->reply_text, name, text);
	chat_keyboard(chat, true);
}

lv_obj_t *ui_chat_quote_create(lv_obj_t *label, bool outgoing, const char *name, const char *text)
{
	lv_obj_t *bubble = lv_obj_get_parent(label);
	lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(bubble, 8, 0);
	/* White on an outgoing bubble (the accent color) */
	lv_color_t color = outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_ACCENT);
	lv_color_t text_color = outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT);
	lv_obj_t *who, *what;
	lv_obj_t *quote = quote_create(bubble, color, text_color, lv_obj_get_style_max_width(label, 0) - 20,
	                               &who, &what);
	lv_obj_move_to_index(quote, 0);
	quote_set(who, what, name, text);
	return quote;
}

/* A message swiped left follows the finger, the list held still meanwhile; let go
 * far enough, it is replied to */
struct replyable {
	struct ui_chat *chat;
	long long id;
	lv_point_t start;
	bool swiping;
};

static void replyable_cb(lv_event_t *e)
{
	struct replyable *r = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p = { 0, 0 };
	if (indev)
		lv_indev_get_point(indev, &p);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		r->start = p;
		r->swiping = false;
		break;
	case LV_EVENT_PRESSING: {
		int32_t dx = p.x - r->start.x, dy = p.y - r->start.y;
		if (!r->swiping) {
			/* Scrolling the list, or not clearly sideways yet */
			if (!indev || lv_indev_get_scroll_obj(indev) || dx > -16 || -dx < 2 * LV_ABS(dy))
				break;
			r->swiping = true;
			lv_obj_set_scroll_chain_ver(obj, false);
		}
		lv_obj_set_style_translate_x(obj, LV_CLAMP(-REPLY_SWIPE * 3 / 2, dx, 0), 0);
		break;
	}
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST: {
		if (!r->swiping)
			break;
		bool reply = lv_event_get_code(e) == LV_EVENT_RELEASED &&
		             lv_obj_get_style_translate_x(obj, 0) <= -REPLY_SWIPE;
		r->swiping = false;
		lv_obj_set_scroll_chain_ver(obj, true);
		lv_obj_set_style_translate_x(obj, 0, 0);
		/* Not a click as well (a photo opening, the keyboard hidden) */
		if (indev && lv_event_get_code(e) == LV_EVENT_RELEASED)
			lv_indev_reset(indev, obj);
		if (reply && r->chat->reply) {
			ui_vibrate(UI_VIBRATE_TAP);
			r->chat->reply(r->chat, r->id);
		}
		break;
	}
	case LV_EVENT_CLICKED:
		/* As a tap on the list: the keyboard hidden */
		lv_obj_send_event(r->chat->list, LV_EVENT_CLICKED, NULL);
		break;
	case LV_EVENT_DELETE:
		free(r);
		break;
	default:
		break;
	}
}

void ui_chat_replyable(struct ui_chat *chat, lv_obj_t *obj, long long id)
{
	struct replyable *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->chat = chat;
	r->id = id;
	lv_obj_set_clickable(obj, true);
	lv_obj_add_event_cb(obj, replyable_cb, LV_EVENT_ALL, r);
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
	lv_obj_add_event_cb(chat->list, chat_list_size_cb, LV_EVENT_SIZE_CHANGED, NULL);

	chat->reply_bar = reply_bar_create(chat, body);
	chat->ta = ui_compose_bar(body, max_length, chat_ta_cb, chat_send_cb, chat);
	/* Under the bar, shown while typing; the messages shrink above it */
	ui_textarea_set_prose(chat->ta);
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
	lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
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
