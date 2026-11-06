/*
 * Messages: conversation list and conversation threads with chat bubbles, the SMS of
 * data.c's conversations. Both follow changes as they come (a message received,
 * sent, failed): a timer compares data_version. A thread on screen marks its
 * conversation read; a message not sent is sent again when tapped.
 */

#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "i18n.h"

#define ROW_H 136
#define MAX_LENGTH 480   /* characters in a message: 7 parts in UCS-2, 4 in GSM 7-bit */
#define WATCH_MS 250     /* data_version checked */

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
	lv_obj_t *time = ui_dim_label(top, message_time_text(last->time, false), &ui_font_20);

	lv_obj_t *bottom = ui_row_line(text);
	lv_obj_t *preview = ui_one_line_label(bottom, last->text, &ui_font_20);
	lv_obj_set_flex_grow(preview, 1);
	lv_obj_set_style_text_color(preview, lv_color_hex(UI_COLOR_DIM), 0);

	if (c->unread) {
		/* Its count in a blue pill, the time blue too */
		lv_obj_set_style_text_color(time, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_text_color(preview, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_t *badge = lv_label_create(bottom);
		ui_set_text_fmt(badge, "%u", c->unread);
		lv_obj_set_style_text_font(badge, &ui_font_16, 0);
		lv_obj_set_style_text_color(badge, lv_color_white(), 0);
		lv_obj_set_style_bg_color(badge, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_pad_hor(badge, 10, 0);
		lv_obj_set_style_pad_ver(badge, 2, 0);
		lv_obj_set_style_min_width(badge, 32, 0);
		lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_left(badge, 12, 0);
	}
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
		ui_empty_label(body, _("No messages"));
		return;
	}

	lv_obj_t *card = ui_card_create(body);

	for (unsigned i = 0; i < conversation_count(); i++) {
		struct conversation *c = conversation_get(i);
		if (c->count)
			row_create(card, c, --n > 0);
	}
}

/* Back from a thread, or something received, sent: filled again */
static void list_update(lv_obj_t *body)
{
	if ((uintptr_t)lv_obj_get_user_data(body) != data_version)
		list_fill(body);
}

static void list_load_cb(lv_event_t *e)
{
	list_update(lv_event_get_user_data(e));
}

static void list_watch_cb(lv_timer_t *timer)
{
	lv_obj_t *body = lv_timer_get_user_data(timer);
	if (lv_screen_active() == lv_obj_get_screen(body))
		list_update(body);
}

static void timer_delete_cb(lv_event_t *e)
{
	lv_timer_delete(lv_event_get_user_data(e));
}

static void new_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_open();
}

void messages_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Messages"), &header, &body);
	ui_header_button(header, LV_SYMBOL_EDIT, new_cb, NULL);
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
	list_fill(body);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, body);
	lv_timer_t *watch = lv_timer_create(list_watch_cb, WATCH_MS, body);
	lv_obj_add_event_cb(scr, timer_delete_cb, LV_EVENT_DELETE, watch);
	ui_push(scr);
}

/* Thread */

struct thread {
	struct ui_chat chat; /* first: send() gets it */
	struct conversation *conv;
	lv_obj_t *scr;
	lv_timer_t *watch;
	unsigned version, shown; /* data_version and the messages when last filled */
	/* Messages are grouped while they go the same way, at the same time, in the same
	 * state, with the time (or the state) under the group */
	lv_obj_t *group;
	const char *time;
	bool outgoing;
	enum message_state state;
	const char *error;
	struct thread *next; /* threads */
};

static void retry_cb(lv_event_t *e);

/* The text under a group: its time, or what happened to it */
static void group_time_set(lv_obj_t *label, const struct message *m)
{
	if (m->state == MESSAGE_SENDING) {
		lv_label_set_text_static(label, _("Sending..."));
	} else if (m->state == MESSAGE_FAILED) {
		if (m->error)
			ui_set_text_fmt(label, _("Not sent: %s. Tap to try again"), _(m->error)); /* error: English */
		else
			lv_label_set_text_static(label, _("Not sent. Tap to try again"));
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ORANGE), 0);
	} else {
		lv_label_set_text_static(label, message_time_text(m->time, true));
	}
}

static void thread_add(struct thread *t, unsigned i)
{
	const struct message *m = &t->conv->messages[i];
	const char *time = message_time_text(m->time, true);
	if (!t->group || t->outgoing != m->outgoing || t->state != m->state || t->error != m->error ||
	    t->time != time) {
		lv_obj_t *label;
		t->group = ui_chat_group_create(&t->chat, m->outgoing, &label);
		group_time_set(label, m);
		t->time = time; /* interned: compared by address */
		t->outgoing = m->outgoing;
		t->state = m->state;
		t->error = m->error;
	}
	lv_obj_t *text = ui_chat_bubble_create(t->group, m->outgoing);
	lv_label_set_text_static(text, m->text);
	if (m->state == MESSAGE_FAILED) {
		lv_obj_t *bubble = lv_obj_get_parent(text);
		lv_obj_set_user_data(bubble, t);
		lv_obj_set_clickable(bubble, true);
		lv_obj_add_event_cb(bubble, retry_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
	}
}

/* The messages shown again, as they are now */
static void thread_fill(struct thread *t)
{
	lv_obj_clean(t->chat.list);
	t->group = NULL;
	if (t->conv->count == 0) {
		lv_obj_t *hint = lv_label_create(t->chat.list);
		lv_label_set_text_static(hint, _("No messages yet"));
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(hint, 200, 0);
	}
	for (unsigned i = 0; i < t->conv->count; i++)
		thread_add(t, i);
	t->version = data_version;
}

/* Anything new or changed: read if on screen, shown, at the end if it came */
static void thread_update(struct thread *t)
{
	if (lv_screen_active() == t->scr)
		conversation_read(t->conv);
	if (t->version == data_version)
		return;
	bool more = t->conv->count > t->shown;
	thread_fill(t);
	t->shown = t->conv->count;
	if (more)
		ui_chat_scroll_end(&t->chat, LV_ANIM_ON);
}

static void thread_watch_cb(lv_timer_t *timer)
{
	thread_update(lv_timer_get_user_data(timer));
}

/* Shown again by the timer: filling the list now would delete the bubble in its event */
static void retry_cb(lv_event_t *e)
{
	struct thread *t = lv_obj_get_user_data(lv_event_get_current_target(e));
	conversation_retry(t->conv, (unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void thread_send(struct ui_chat *chat, const char *text)
{
	struct thread *t = (struct thread *)chat;
	conversation_send(t->conv, text);
	thread_update(t);
}

static void call_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	phone_call(t->conv->number);
}

/* The threads open, the most recent first */
static struct thread *threads;

static void thread_delete_cb(lv_event_t *e)
{
	struct thread *t = lv_event_get_user_data(e);
	for (struct thread **p = &threads; *p; p = &(*p)->next)
		if (*p == t) {
			*p = t->next;
			break;
		}
	lv_timer_delete(t->watch);
	free(t);
}

bool messages_thread_shown(const char *number)
{
	for (struct thread *t = threads; t; t = t->next)
		if (lv_screen_active() == t->scr && number_equal(t->conv->number, number))
			return true;
	return false;
}

void messages_open_thread(const char *number)
{
	struct conversation *conv = conversation_find(number, true);
	if (!conv)
		return;

	struct thread *t = calloc(1, sizeof(*t));
	t->conv = conv;
	t->next = threads;
	threads = t;
	lv_obj_t *header, *body;
	t->scr = ui_app_screen_create(conversation_title(conv), &header, &body);
	lv_obj_add_event_cb(t->scr, thread_delete_cb, LV_EVENT_DELETE, t);
	ui_header_button(header, LV_SYMBOL_CALL, call_cb, t);
	t->chat.send = thread_send;
	ui_chat_create(&t->chat, body, MAX_LENGTH);

	conversation_read(conv);
	thread_fill(t);
	t->shown = conv->count;
	t->watch = lv_timer_create(thread_watch_cb, WATCH_MS, t);

	ui_chat_scroll_end(&t->chat, LV_ANIM_OFF);
	ui_push(t->scr);
}
