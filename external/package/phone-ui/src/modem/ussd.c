/*
 * USSD (modem.h): a code dialed in Phone (*111#) and the network's replies, as a
 * conversation: what was sent on the right, the replies on the left, the reply field
 * (a dial pad) while the network wants an answer (a menu). Leaving the screen ends the
 * session. What the network sends unasked goes to the screen open, or opens one.
 * *#06# shows the IMEI, without the network.
 */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "modem.h"
#include "ui.h"

#define MAX_LENGTH 64 /* an answer: codes and menu choices are short */

struct ussd {
	struct ui_chat chat;  /* first: the chat's callbacks get it */
	lv_obj_t *scr;
	lv_obj_t *sending;    /* "Sending..." under what was sent, NULL none */
	bool waiting, open;   /* a reply awaited; the network wants an answer */
};

static struct ussd *current;

bool ussd_is_code(const char *number)
{
	size_t n = strlen(number);
	return n >= 2 && (number[0] == '*' || number[0] == '#') && number[n - 1] == '#';
}

/* The reply field, while the network wants an answer and none is on its way */
static void field_update(struct ussd *u)
{
	bool shown = u->open && !u->waiting;
	lv_obj_t *bar = lv_obj_get_parent(u->chat.ta);
	if (!shown && !lv_obj_is_hidden(u->chat.kb))
		ui_keyboard_show(u->chat.kb, false);
	lv_obj_set_hidden(bar, !shown);
}

/* A bubble with the text (copied); its group's dim label in *status */
static void bubble(struct ussd *u, bool outgoing, const char *text, lv_obj_t **status)
{
	lv_obj_t *group = ui_chat_group_create(&u->chat, outgoing, status);
	lv_label_set_text(ui_chat_bubble_create(group, outgoing), text);
}

/* Why there is no reply, or the session's end (static, in English: translated here), centered */
static void note(struct ussd *u, const char *text)
{
	lv_obj_t *label = lv_label_create(u->chat.list);
	lv_label_set_text_static(label, _(text));
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
}

/* A code or an answer: shown sent, or why it cannot be */
static void send(struct ussd *u, const char *text)
{
	bubble(u, true, text, &u->sending);
	const char *why = modem_ussd_unavailable();
	if (!why && !modem_ussd_send(text))
		why = N_("Not sent");
	if (why) {
		note(u, why);
		u->sending = NULL;
		u->open = false;
	} else {
		lv_label_set_text_static(u->sending, _("Sending..."));
		u->waiting = true;
	}
	field_update(u);
	ui_chat_scroll_end(&u->chat, LV_ANIM_ON);
}

static void send_cb(struct ui_chat *chat, const char *text)
{
	send((struct ussd *)chat, text);
}

static void delete_cb(lv_event_t *e)
{
	struct ussd *u = lv_event_get_user_data(e);
	if (u->waiting || u->open)
		modem_ussd_cancel();
	if (current == u)
		current = NULL;
	free(u);
}

/* The screen titled `title` (static text), pushed */
static struct ussd *screen(const char *title)
{
	struct ussd *u = calloc(1, sizeof(*u));
	lv_obj_t *body;
	u->scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_add_event_cb(u->scr, delete_cb, LV_EVENT_DELETE, u);
	u->chat.send = send_cb;
	ui_chat_create(&u->chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(u->chat.ta, _("Reply"));
	ui_textarea_set_phone(u->chat.ta);
	field_update(u);
	current = u;
	ui_push(u->scr);
	return u;
}

void ussd_open(const char *code)
{
	struct ussd *u = screen(ui_intern(code));
	if (strcmp(code, "*#06#") != 0) {
		send(u, code);
		return;
	}
	/* The IMEI: the modem's, no network */
	bubble(u, true, code, NULL);
	if (modem_imei()[0]) {
		char text[48];
		snprintf(text, sizeof(text), "IMEI\n%s", modem_imei());
		bubble(u, false, text, NULL);
	} else {
		note(u, modem_state() == MODEM_NONE || modem_state() == MODEM_ABSENT ? N_("No modem") :
		        N_("Not known yet"));
	}
}

void ussd_received(enum modem_ussd state, const char *text)
{
	/* Unasked, or after its screen was left: a reply opens one, an end does not */
	if (!current && !text[0])
		return;
	struct ussd *u = current ? current : screen("USSD");
	if (u->sending)
		lv_label_set_text_static(u->sending, "");
	u->sending = NULL;
	u->waiting = false;
	u->open = state == MODEM_USSD_MORE;
	if (text[0]) {
		bubble(u, false, text, NULL);
		ui_vibrate(60);
	}
	static const char *const notes[] = {
		[MODEM_USSD_ENDED] = N_("Ended by the network"),
		[MODEM_USSD_UNSUPPORTED] = N_("Not supported by the network"),
		[MODEM_USSD_TIMEOUT] = N_("No answer from the network"),
		[MODEM_USSD_REFUSED] = N_("Refused by the network"),
		[MODEM_USSD_FAILED] = N_("Not sent"),
	};
	if (notes[state] && !(state == MODEM_USSD_ENDED && text[0]))
		note(u, notes[state]);
	field_update(u);
	ui_chat_scroll_end(&u->chat, LV_ANIM_ON);
}
