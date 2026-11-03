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
	struct ui_chat chat; /* first: send() gets it */
	struct conversation *conv;
	lv_obj_t *hint;
	/* Messages are grouped while they go the same way at the same time, with the
	 * time under the group */
	lv_obj_t *group;
	const char *time;
	bool outgoing, unsent;
};

static void thread_add(struct thread *t, const struct message *m)
{
	if (!t->group || t->outgoing != m->outgoing || t->unsent != m->unsent ||
	    strcmp(t->time, m->time) != 0) {
		lv_obj_t *time;
		t->group = ui_chat_group_create(&t->chat, m->outgoing, &time);
		if (m->unsent) {
			ui_set_text_fmt(time, "%s  " LV_SYMBOL_WARNING " Not sent: no SIM card", m->time);
			lv_obj_set_style_text_color(time, lv_color_hex(UI_COLOR_ORANGE), 0);
		} else {
			lv_label_set_text_static(time, m->time);
		}
		t->time = m->time;
		t->outgoing = m->outgoing;
		t->unsent = m->unsent;
	}
	lv_label_set_text_static(ui_chat_bubble_create(t->group, m->outgoing), m->text);
}

static void thread_send(struct ui_chat *chat, const char *text)
{
	struct thread *t = (struct thread *)chat;
	if (t->hint) {
		lv_obj_delete(t->hint);
		t->hint = NULL;
	}
	thread_add(t, conversation_send(t->conv, text));
	ui_chat_scroll_end(chat, LV_ANIM_ON);
}

static void call_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	phone_call(t->conv->number);
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
	t->chat.send = thread_send;
	ui_chat_create(&t->chat, body, MAX_LENGTH);

	if (conv->count == 0) {
		t->hint = lv_label_create(t->chat.list);
		lv_label_set_text_static(t->hint, "No messages yet");
		lv_obj_set_width(t->hint, LV_PCT(100));
		lv_obj_set_style_text_align(t->hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(t->hint, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(t->hint, 200, 0);
	}
	for (unsigned i = 0; i < conv->count; i++)
		thread_add(t, &conv->messages[i]);

	ui_chat_scroll_end(&t->chat, LV_ANIM_OFF);
	ui_push(scr);
}
