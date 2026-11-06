/*
 * The banner of a message received while the phone is in use (see data.h): a card that
 * slides down from under the status bar with the sender and the message, and back up
 * after a few seconds. Tapped, it opens the conversation. Another message while it shows
 * takes its place, for as long again.
 *
 * Not while the conversation's thread is on screen (the message shows there), the phone
 * is locked or the quick settings are open (both list the notifications).
 *
 * It goes on the top layer, under the status bar, over the screens and the lock screen.
 */

#include <stdlib.h>

#include "data.h"

#define SHOW_MS 4000
#define GAP 8    /* px below the status bar */
#define SIDE 16  /* px from the display's sides */

static struct {
	lv_obj_t *card;     /* NULL: none */
	lv_timer_t *timer;  /* its time up */
	const char *number; /* the conversation's */
	int32_t h;
} bn;

static void y_exec_cb(void *var, int32_t v)
{
	lv_obj_set_y(var, v);
}

static void gone_cb(lv_anim_t *a)
{
	lv_obj_delete_async(lv_anim_get_user_data(a));
}

/* Away: slid up and deleted (asynchronously: tapped, it goes from its own event) */
static void hide(bool slide)
{
	if (!bn.card)
		return;
	lv_obj_t *card = bn.card;
	lv_timer_delete(bn.timer);
	bn.card = NULL;
	bn.timer = NULL;
	lv_anim_delete(card, y_exec_cb);
	if (!slide || !ui_anim_ms) {
		lv_obj_delete_async(card);
		return;
	}
	lv_obj_set_clickable(card, false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, card);
	lv_anim_set_user_data(&a, card);
	lv_anim_set_exec_cb(&a, y_exec_cb);
	lv_anim_set_values(&a, lv_obj_get_y(card), -bn.h);
	lv_anim_set_duration(&a, ui_anim_ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
	lv_anim_set_completed_cb(&a, gone_cb);
	lv_anim_start(&a);
}

static void time_up_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	hide(true);
}

static void tap_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	const char *number = bn.number;
	ui_vibrate(UI_VIBRATE_TAP);
	hide(false);
	messages_open_thread(number);
}

/* The last message received in it, NULL without */
static const struct message *last_received(const struct conversation *c)
{
	for (unsigned i = c->count; i-- > 0;)
		if (!c->messages[i].outgoing)
			return &c->messages[i];
	return NULL;
}

void notification_banner(const char *sender)
{
	struct conversation *c = conversation_find(sender, false);
	const struct message *m = c ? last_received(c) : NULL;
	if (!m || ui_locked() || ui_shade_shown() || messages_thread_shown(c->number))
		return;
	hide(false);

	lv_obj_t *card = bn.card = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * SIDE,
	                LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_radius(card, 28, 0);
	lv_obj_set_style_shadow_width(card, 32, 0);
	lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
	lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_column(card, 20, 0);
	lv_obj_set_style_text_color(card, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_set_gesture_bubble(card, false);
	lv_obj_add_event_cb(card, tap_cb, LV_EVENT_CLICKED, NULL);

	avatar_create(card, c->contact, 72);
	lv_obj_t *text = ui_box(card);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 6, 0);
	lv_obj_t *top = ui_box(text);
	lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *name = ui_one_line_label(top, conversation_title(c), &ui_font_26);
	lv_obj_set_flex_grow(name, 1);
	ui_set_text(ui_dim_label(top, "", &ui_font_20), message_time_text(m->time, false));
	lv_obj_t *message = ui_one_line_label(text, m->text, &ui_font_24);
	lv_obj_set_width(message, LV_PCT(100));

	bn.number = c->number;
	lv_obj_update_layout(card);
	bn.h = lv_obj_get_height(card);
	lv_obj_set_x(card, SIDE);
	/* Down from above the display, ending under the status bar */
	int32_t y = UI_STATUS_H + GAP;
	lv_obj_set_y(card, ui_anim_ms ? -bn.h : y);
	if (ui_anim_ms) {
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, card);
		lv_anim_set_exec_cb(&a, y_exec_cb);
		lv_anim_set_values(&a, -bn.h, y);
		lv_anim_set_duration(&a, ui_anim_ms);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_start(&a);
	}
	bn.timer = lv_timer_create(time_up_cb, SHOW_MS, NULL);
	ui_status_bar_foreground(); /* the bar over it, as it comes from under it */
}
