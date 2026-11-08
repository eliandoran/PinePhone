/*
 * Assistant's conversation screen: the messages as bubbles, searches as notes
 * between them, the compose bar, deleting it.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "assistant.h"

#define MAX_LENGTH 2000 /* characters typed in a message */

/* View */

bool ai_view_at_end(void)
{
	return lv_obj_get_scroll_bottom(ai_cv.chat.list) < 200 || lv_anim_get(ai_cv.chat.list, NULL);
}

void ai_view_set_text(lv_obj_t *label, const char *text)
{
	char *composed = ui_emoji_compose(text);
	lv_label_set_text(label, composed ? composed : text);
	free(composed);
}

/* A message as a group of one bubble with its time; answers in Markdown, the width */
lv_obj_t *ai_view_add(bool user, const char *text, time_t when, lv_obj_t **time_label)
{
	lv_obj_t *time, *bubble;
	lv_obj_t *group = ui_chat_group_create(&ai_cv.chat, user, &time);
	if (user) {
		lv_obj_t *label = ui_chat_bubble_create(group, user);
		ai_view_set_text(label, text);
		bubble = lv_obj_get_parent(label);
	} else {
		bubble = ui_markdown_bubble(group, false, text, NULL, NULL);
	}
	if (when) {
		char buf[8];
		struct tm tm;
		localtime_r(&when, &tm);
		strftime(buf, sizeof(buf), "%H:%M", &tm);
		ui_set_text(time, buf);
	}
	if (time_label)
		*time_label = time;
	return bubble;
}

/* Dim and centered between the messages */
lv_obj_t *ai_view_note(const char *text, uint32_t color)
{
	lv_obj_t *label = lv_label_create(ai_cv.chat.list);
	ai_view_set_text(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_set_style_pad_ver(label, 12, 0);
	return label;
}

static void view_turn(const struct turn *t)
{
	if (t->role == STEP) {
		if (t->text[0])
			ai_view_note(t->text, UI_COLOR_DIM);
	} else {
		ai_view_add(t->role == USER, t->text, t->time, NULL);
	}
}

/* Under the messages: tapping it calls cb (try again) */
void ai_view_status(const char *text, uint32_t color, lv_event_cb_t cb)
{
	if (ai_cv.status)
		lv_obj_delete(ai_cv.status);
	ai_cv.status = ai_view_note(text, color);
	if (cb) {
		lv_obj_set_clickable(ai_cv.status, true);
		lv_obj_add_event_cb(ai_cv.status, cb, LV_EVENT_CLICKED, NULL);
	}
}

void ai_view_status_clear(void)
{
	if (ai_cv.status)
		lv_obj_delete(ai_cv.status);
	ai_cv.status = NULL;
}

static void conversation_send(struct ui_chat *chat, const char *text)
{
	/* One question at a time: back in the field until the answer is there */
	if (ai_busy() || !ai_turn_add(USER, time(NULL), text, NULL)) {
		lv_textarea_set_text(chat->ta, text);
		return;
	}
	ai_view_status_clear();
	ai_view_add(true, text, ai_cv.turns[ai_cv.count - 1].time, NULL);
	ai_save();
	ai_cv.rounds = 0;
	ai_ask();
}

static void settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_keyboard_show(ai_cv.chat.kb, false);
	ai_settings_open();
}

static void trash_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_keyboard_show(ai_cv.chat.kb, false);
	lv_obj_set_hidden(ai_cv.confirm, !lv_obj_is_hidden(ai_cv.confirm));
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(ai_cv.confirm, true);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	ai_ask_cancel();
	if (ai_cv.path[0]) {
		unlink(ai_cv.path);
		ai_version++;
	}
	ai_turns_drop(0); /* nothing left to save */
	ui_pop();
}

static void conversation_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ai_ask_stop();
	ai_turns_drop(0);
	free(ai_cv.turns);
	memset(&ai_cv, 0, sizeof(ai_cv));
}

void ai_conversation_open(const char *path)
{
	if (ai_cv.scr)
		return;
	const struct provider *p;
	ai_model_current(&p);

	lv_obj_t *header, *body;
	ai_cv.scr = ui_app_screen_create(p->name, &header, &body);
	lv_obj_add_event_cb(ai_cv.scr, conversation_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, NULL);
	ui_header_button(header, LV_SYMBOL_SETTINGS, settings_cb, NULL);
	ai_cv.chat.send = conversation_send;
	ui_chat_create(&ai_cv.chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(ai_cv.chat.ta, _("Ask anything"));

	/* Delete confirmation, above the compose bar */
	char text[96];
	snprintf(text, sizeof(text), LV_SYMBOL_TRASH "  %s", _("Delete chat"));
	ai_cv.confirm = ui_confirm_bar(body, ui_intern(text), delete_cb, cancel_cb, NULL);
	lv_obj_move_to_index(ai_cv.confirm, 1);

	if (path)
		ai_load(path);
	for (unsigned i = 0; i < ai_cv.count; i++)
		view_turn(&ai_cv.turns[i]);
	if (!ai_cv.count) {
		lv_obj_t *hint = ui_empty_label(ai_cv.chat.list, _("Ask a question below"));
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_margin_top(hint, 200, 0);
		ai_cv.status = hint;
	} else if (ai_cv.turns[ai_cv.count - 1].role != ASSISTANT) {
		ai_view_status(_("Not answered. Tap to try again."), UI_COLOR_DIM, ai_retry_cb);
	}

	ui_chat_scroll_end(&ai_cv.chat, LV_ANIM_OFF);
	ui_push(ai_cv.scr);
	if (!ai_cv.count)
		ui_keyboard_show(ai_cv.chat.kb, true);
}
