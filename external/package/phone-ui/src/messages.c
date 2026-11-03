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

/* Conversation list */

static void row_cb(lv_event_t *e)
{
	struct conversation *c = lv_event_get_user_data(e);
	messages_open_thread(c->number);
}

static void row_create(lv_obj_t *card, struct conversation *c, bool divider)
{
	const struct message *last = &c->messages[c->count - 1];

	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, c);
	avatar_create(row, c->contact, 88);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *top = ui_row_line(text);
	lv_obj_t *name = ui_one_line_label(top, conversation_title(c), &ui_font_26);
	lv_obj_set_flex_grow(name, 1);
	ui_dim_label(top, last->time, &ui_font_20);

	lv_obj_t *preview = ui_one_line_label(text, last->text, &ui_font_20);
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
		ui_empty_label(body, "No messages");
		return;
	}

	lv_obj_t *card = ui_card_create(body);

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
		t->group = ui_box(t->list);
		lv_obj_set_size(t->group, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(t->group, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(t->group, LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
		                      m->outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(t->group, 6, 0);

		t->group_time = lv_label_create(t->group);
		if (m->unsent) {
			ui_set_text_fmt(t->group_time, "%s  " LV_SYMBOL_WARNING " Not sent: no SIM card", m->time);
			lv_obj_set_style_text_color(t->group_time, lv_color_hex(UI_COLOR_ORANGE), 0);
		} else {
			lv_label_set_text_static(t->group_time, m->time);
			lv_obj_set_style_text_color(t->group_time, lv_color_hex(UI_COLOR_DIM), 0);
		}
		lv_obj_set_style_text_font(t->group_time, &ui_font_16, 0);
		lv_obj_set_style_pad_hor(t->group_time, 12, 0);
		lv_obj_set_style_margin_top(t->group_time, 2, 0);

		t->time = m->time;
		t->outgoing = m->outgoing;
		t->unsent = m->unsent;
	}

	lv_obj_t *bubble = ui_box(t->group);
	lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bubble, 24, 0);
	lv_obj_set_style_pad_ver(bubble, 16, 0);
	lv_obj_set_style_radius(bubble, 24, 0);
	lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
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
	ui_keyboard_show(t->kb, show);
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
	case LV_EVENT_READY: /* the keyboard's Enter key */
		thread_send(t);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
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

	t->ta = ui_compose_bar(body, MAX_LENGTH, ta_cb, send_cb, t);

	/* Under the bar, shown while typing; the thread shrinks above it */
	t->kb = ui_keyboard_create(body, t->ta);

	thread_scroll_end(t, LV_ANIM_OFF);
	ui_push(scr);
}
