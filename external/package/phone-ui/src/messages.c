/*
 * Messages: conversation list and conversation threads with chat bubbles. Sent
 * messages are only kept in RAM (data.c) and marked as not sent: there is no
 * modem yet.
 */

#include <stdlib.h>
#include <string.h>

#include "data.h"

#define ROW_H 136
#define MAX_LENGTH 480 /* characters in a message */
#define COLOR_ORANGE 0xff9f0a

/* Conversation list */

static lv_obj_t *one_line_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	/* Clipped to one line: no dots, they would be written into the static text */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

static void row_cb(lv_event_t *e)
{
	struct conversation *c = lv_event_get_user_data(e);
	messages_open_thread(c->number);
}

static void row_create(lv_obj_t *card, struct conversation *c, bool divider)
{
	const struct message *last = &c->messages[c->count - 1];

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_set_style_pad_column(row, 20, 0);
	lv_obj_set_style_radius(row, 24, 0);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	if (divider) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	}
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, c);

	avatar_create(row, c->contact, 88);

	lv_obj_t *text = lv_obj_create(row);
	lv_obj_remove_style_all(text);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 6, 0);
	lv_obj_set_clickable(text, false);
	lv_obj_set_scrollable(text, false);

	lv_obj_t *top = lv_obj_create(text);
	lv_obj_remove_style_all(top);
	lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(top, 12, 0);
	lv_obj_set_clickable(top, false);
	lv_obj_set_scrollable(top, false);

	lv_obj_t *name = one_line_label(top, conversation_title(c), &lv_font_montserrat_26);
	lv_obj_set_flex_grow(name, 1);
	lv_obj_t *time = lv_label_create(top);
	lv_label_set_text_static(time, last->time);
	lv_obj_set_style_text_font(time, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(time, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *preview = one_line_label(text, last->text, &lv_font_montserrat_20);
	lv_obj_set_width(preview, LV_PCT(100));
	lv_obj_set_style_text_color(preview, lv_color_hex(UI_COLOR_DIM), 0);
}

static void list_fill(lv_obj_t *body)
{
	lv_obj_clean(body);
	lv_obj_set_user_data(body, (void *)(uintptr_t)data_version);

	/* Conversations opened without sending anything are left out */
	unsigned n = 0;
	for (unsigned i = 0; i < conversation_count(); i++)
		n += conversation_get(i)->count > 0;
	if (!n) {
		lv_obj_t *label = lv_label_create(body);
		lv_label_set_text_static(label, "No messages");
		lv_obj_set_width(label, LV_PCT(100));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(label, 200, 0);
		return;
	}

	lv_obj_t *card = lv_obj_create(body);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_clickable(card, false);
	lv_obj_set_scrollable(card, false);

	for (unsigned i = 0; i < conversation_count(); i++) {
		struct conversation *c = conversation_get(i);
		if (c->count)
			row_create(card, c, --n > 0);
	}
}

/* Back from a thread: something may have been sent */
static void list_load_cb(lv_event_t *e)
{
	lv_obj_t *body = lv_event_get_user_data(e);
	if ((uintptr_t)lv_obj_get_user_data(body) != data_version)
		list_fill(body);
}

static void new_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_open();
}

void messages_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Messages", &header, &body);
	ui_header_button(header, LV_SYMBOL_EDIT, new_cb, NULL);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
	list_fill(body);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, body);
	ui_push(scr);
}

/* Thread */

struct thread {
	struct conversation *conv;
	lv_obj_t *list, *ta, *kb, *hint;
	/* Messages are grouped while they go the same way at the same time, with the
	 * time under the group */
	lv_obj_t *group, *group_time;
	const char *time;
	bool outgoing, unsent;
};

static void thread_scroll_end(struct thread *t, lv_anim_enable_t anim)
{
	lv_obj_update_layout(t->list);
	lv_obj_scroll_to_y(t->list, lv_obj_get_scroll_y(t->list) + lv_obj_get_scroll_bottom(t->list), anim);
}

static void thread_add(struct thread *t, const struct message *m)
{
	if (!t->group || t->outgoing != m->outgoing || t->unsent != m->unsent ||
	    strcmp(t->time, m->time) != 0) {
		t->group = lv_obj_create(t->list);
		lv_obj_remove_style_all(t->group);
		lv_obj_set_size(t->group, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(t->group, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(t->group, LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(t->group, 6, 0);
		lv_obj_set_clickable(t->group, false);
		lv_obj_set_scrollable(t->group, false);

		t->group_time = lv_label_create(t->group);
		if (m->unsent) {
			ui_set_text_fmt(t->group_time, "%s  " LV_SYMBOL_WARNING " Not sent: no SIM card", m->time);
			lv_obj_set_style_text_color(t->group_time, lv_color_hex(COLOR_ORANGE), 0);
		} else {
			lv_label_set_text_static(t->group_time, m->time);
			lv_obj_set_style_text_color(t->group_time, lv_color_hex(UI_COLOR_DIM), 0);
		}
		lv_obj_set_style_text_font(t->group_time, &lv_font_montserrat_16, 0);
		lv_obj_set_style_pad_hor(t->group_time, 12, 0);
		lv_obj_set_style_margin_top(t->group_time, 2, 0);

		t->time = m->time;
		t->outgoing = m->outgoing;
		t->unsent = m->unsent;
	}

	lv_obj_t *bubble = lv_obj_create(t->group);
	lv_obj_remove_style_all(bubble);
	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bubble, 24, 0);
	lv_obj_set_style_pad_ver(bubble, 16, 0);
	lv_obj_set_style_radius(bubble, 24, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
	lv_obj_set_clickable(bubble, false);
	lv_obj_set_scrollable(bubble, false);
	/* Before the time */
	lv_obj_move_to_index(bubble, (int32_t)lv_obj_get_child_count(t->group) - 2);

	/* Wraps at 75% of the screen (bubble padding included) */
	lv_obj_t *label = lv_label_create(bubble);
	lv_label_set_text_static(label, m->text);
	lv_obj_set_style_max_width(label, lv_display_get_horizontal_resolution(lv_display_get_default()) * 3 / 4 - 48, 0);
	lv_obj_set_style_text_color(label, m->outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
}

static void keyboard_show(struct thread *t, bool show)
{
	lv_obj_set_hidden(t->kb, !show);
	lv_obj_set_state(t->ta, LV_STATE_FOCUSED, show);
	thread_scroll_end(t, LV_ANIM_OFF);
}

static void thread_send(struct thread *t)
{
	char buf[MAX_LENGTH * 4 + 1];
	const char *text = lv_textarea_get_text(t->ta);
	while (*text == ' ')
		text++;
	size_t n = strlen(text);
	while (n && text[n - 1] == ' ')
		n--;
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	keyboard_show(t, false);
	if (!n)
		return;
	memcpy(buf, text, n);
	buf[n] = '\0';

	if (t->hint) {
		lv_obj_delete(t->hint);
		t->hint = NULL;
	}
	thread_add(t, conversation_send(t->conv, buf));
	lv_textarea_set_text(t->ta, "");
	ui_vibrate(15);
	thread_scroll_end(t, LV_ANIM_ON);
}

static void send_cb(lv_event_t *e)
{
	thread_send(lv_event_get_user_data(e));
}

static void ta_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		keyboard_show(t, true);
		break;
	case LV_EVENT_READY: /* Enter, or the keyboard's OK key */
		thread_send(t);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's close key */
		keyboard_show(t, false);
		break;
	default:
		break;
	}
}

static void list_click_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	if (!lv_obj_is_hidden(t->kb))
		keyboard_show(t, false);
}

static void call_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	phone_dial(t->conv->number);
}

static void thread_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

void messages_open_thread(const char *number)
{
	struct conversation *conv = conversation_find(number, true);
	if (!conv)
		return;

	struct thread *t = calloc(1, sizeof(*t));
	t->conv = conv;
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(conversation_title(conv), &header, &body);
	lv_obj_add_event_cb(scr, thread_delete_cb, LV_EVENT_DELETE, t);
	ui_header_button(header, LV_SYMBOL_CALL, call_cb, t);
	lv_obj_set_scrollable(body, false);

	t->list = lv_obj_create(body);
	lv_obj_remove_style_all(t->list);
	lv_obj_set_width(t->list, LV_PCT(100));
	lv_obj_set_flex_grow(t->list, 1);
	lv_obj_set_flex_flow(t->list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(t->list, 16, 0);
	lv_obj_set_style_pad_ver(t->list, 16, 0);
	lv_obj_set_style_pad_row(t->list, 24, 0);
	lv_obj_set_scroll_dir(t->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(t->list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(t->list, list_click_cb, LV_EVENT_CLICKED, t);

	if (conv->count == 0) {
		t->hint = lv_label_create(t->list);
		lv_label_set_text_static(t->hint, "No messages yet");
		lv_obj_set_width(t->hint, LV_PCT(100));
		lv_obj_set_style_text_align(t->hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(t->hint, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(t->hint, 200, 0);
	}
	for (unsigned i = 0; i < conv->count; i++)
		thread_add(t, &conv->messages[i]);

	/* Text field and send button */
	lv_obj_t *bar = lv_obj_create(body);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_column(bar, 12, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_clickable(bar, false);
	lv_obj_set_scrollable(bar, false);

	t->ta = lv_textarea_create(bar);
	lv_textarea_set_one_line(t->ta, true);
	lv_textarea_set_placeholder_text(t->ta, "Message");
	lv_textarea_set_max_length(t->ta, MAX_LENGTH);
	lv_obj_set_height(t->ta, 80);
	lv_obj_set_flex_grow(t->ta, 1);
	lv_obj_set_style_radius(t->ta, 40, 0);
	lv_obj_set_style_pad_hor(t->ta, 28, 0);
	lv_obj_set_style_pad_ver(t->ta, 24, 0);
	lv_obj_set_style_bg_opa(t->ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t->ta, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_text_color(t->ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(t->ta, 0, 0);
	lv_obj_set_style_border_width(t->ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(t->ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(t->ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(t->ta, 0, 0);
	lv_obj_add_event_cb(t->ta, ta_cb, LV_EVENT_CLICKED, t);
	lv_obj_add_event_cb(t->ta, ta_cb, LV_EVENT_READY, t);
	lv_obj_add_event_cb(t->ta, ta_cb, LV_EVENT_CANCEL, t);

	lv_obj_t *button = lv_button_create(bar);
	lv_obj_set_size(button, 80, 80);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, send_cb, LV_EVENT_CLICKED, t);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, LV_SYMBOL_UP);
	lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(icon, lv_color_white(), 0);
	lv_obj_center(icon);

	/* Under the bar, shown while typing; the thread shrinks above it */
	t->kb = lv_keyboard_create(body);
	lv_obj_set_size(t->kb, LV_PCT(100), 520);
	lv_obj_set_style_text_font(t->kb, &lv_font_montserrat_26, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(t->kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(t->kb, 0, 0);
	lv_keyboard_set_textarea(t->kb, t->ta);
	lv_obj_set_hidden(t->kb, true);

	thread_scroll_end(t, LV_ANIM_OFF);
	ui_push(scr);
}
