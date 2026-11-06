/*
 * Notifications, see data.h: what came while the user was not looking, made from the
 * conversations (messages unread) and the call log (missed calls not seen in Recents) as
 * they are, again when data_version changes. Nothing of their own is kept: reading a
 * thread or opening Recents, wherever it happens, is what makes one go.
 *
 * Shown by the lock screen (lock.c) and the quick settings panel (shade.c) as a card of
 * rows, by the status bar (ui.c) as icons and by the apps' icons (home.c) as badges.
 */

#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "i18n.h"

#define ROW_H 120

static struct notification *list; /* most recent first */
static unsigned count, size;
static unsigned messages;         /* unread, in all conversations */
static unsigned version;          /* data_version when built */
static bool built;

static int newest_first(const void *a, const void *b)
{
	const struct notification *x = a, *y = b;
	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static struct notification *add(enum notification_kind kind, const char *number, time_t time)
{
	if (count == size) {
		size = size ? size * 2 : 16;
		list = realloc(list, size * sizeof(*list));
	}
	struct notification *n = &list[count++];
	*n = (struct notification){ kind, number, 1, time, NULL };
	return n;
}

static void build(void)
{
	if (built && version == data_version)
		return;
	built = true;
	version = data_version;
	count = messages = 0;

	for (unsigned i = 0; i < conversation_count(); i++) {
		const struct conversation *c = conversation_get(i);
		if (!c->unread)
			continue;
		messages += c->unread;
		/* The last message received (one may have been sent since) */
		const struct message *last = NULL;
		for (unsigned k = c->count; k-- > 0 && !last;)
			if (!c->messages[k].outgoing)
				last = &c->messages[k];
		if (!last)
			continue;
		struct notification *n = add(NOTIFICATION_MESSAGES, c->number, last->time);
		n->count = c->unread;
		n->text = last->text;
	}

	/* A number's missed calls together, at the time of the latest (listed first) */
	for (unsigned i = 0; i < call_count(); i++) {
		const struct call *c = call_get(i);
		if (!c->unseen)
			continue;
		struct notification *same = NULL;
		for (unsigned k = 0; k < count && !same; k++)
			if (list[k].kind == NOTIFICATION_MISSED_CALLS && number_equal(list[k].number, c->number))
				same = &list[k];
		if (same)
			same->count++;
		else
			add(NOTIFICATION_MISSED_CALLS, c->number, c->time);
	}

	if (count > 1)
		qsort(list, count, sizeof(*list), newest_first);
}

unsigned notification_count(void)
{
	build();
	return count;
}

const struct notification *notification_get(unsigned i)
{
	build();
	return i < count ? &list[i] : NULL;
}

unsigned notification_messages(void)
{
	build();
	return messages;
}

unsigned notification_missed_calls(void)
{
	return call_missed_unseen();
}

void notification_open(const struct notification *n)
{
	/* Copied: opening it changes the list */
	enum notification_kind kind = n->kind;
	const char *number = n->number;
	if (ui_locked())
		ui_unlock();
	if (kind == NOTIFICATION_MESSAGES)
		messages_open_thread(number);
	else
		phone_open(); /* on Recents, where they are seen */
}

void notifications_clear(void)
{
	for (unsigned i = 0; i < conversation_count(); i++)
		conversation_read(conversation_get(i));
	call_missed_seen();
}

/* The card */

struct tap {
	enum notification_kind kind;
	const char *number;
	bool (*may_open)(void);
	void (*opening)(void);
};

static void free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void row_cb(lv_event_t *e)
{
	const struct tap *t = lv_event_get_user_data(e);
	if (t->may_open && !t->may_open())
		return;
	/* As it is now: the one tapped may have changed since the card was made */
	const struct notification *n = NULL;
	for (unsigned i = 0; i < notification_count() && !n; i++)
		if (list[i].kind == t->kind && list[i].number == t->number)
			n = &list[i];
	struct notification gone = { t->kind, t->number, 0, 0, NULL };
	ui_vibrate(UI_VIBRATE_TAP);
	if (t->opening)
		t->opening();
	notification_open(n ? n : &gone);
}

static const char *title(const struct notification *n)
{
	const struct contact *c = contact_find(n->number);
	return c ? c->name : n->number[0] ? n->number : _("Unknown number");
}

static void row_create(lv_obj_t *card, const struct notification *n, bool divider,
                       bool (*may_open)(void), void (*opening)(void))
{
	struct tap *t = malloc(sizeof(*t));
	*t = (struct tap){ n->kind, n->number, may_open, opening };
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, t);
	lv_obj_add_event_cb(row, free_cb, LV_EVENT_DELETE, t);
	/* What it is on can be dragged (the lock screen): the presses go on to it */
	if (may_open)
		lv_obj_set_event_bubble(row, true);
	avatar_create(row, contact_find(n->number), 72);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *top = ui_row_line(text);
	lv_obj_t *name = ui_one_line_label(top, "", &ui_font_26);
	ui_set_text(name, title(n));
	lv_obj_set_flex_grow(name, 1);
	ui_set_text(ui_dim_label(top, "", &ui_font_20), message_time_text(n->time, false));

	lv_obj_t *bottom = ui_row_line(text);
	lv_obj_t *what = ui_one_line_label(bottom, "", &ui_font_20);
	lv_obj_set_flex_grow(what, 1);
	if (n->kind == NOTIFICATION_MISSED_CALLS) {
		if (n->count == 1)
			lv_label_set_text_static(what, _("Missed call"));
		else
			ui_set_text_fmt(what, P_("%u missed call", "%u missed calls", n->count), n->count);
		lv_obj_set_style_text_color(what, lv_color_hex(UI_COLOR_RED), 0);
		return;
	}
	lv_label_set_text_static(what, n->text);
	if (n->count > 1) {
		/* How many in a blue pill, as in Messages */
		lv_obj_t *badge = lv_label_create(bottom);
		ui_set_text_fmt(badge, "%u", n->count);
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

lv_obj_t *notifications_card_create(lv_obj_t *parent, unsigned max, bool (*may_open)(void),
                                    void (*opening)(void))
{
	unsigned n = notification_count();
	if (!n)
		return NULL;
	lv_obj_t *box = ui_box(parent);
	lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(box, 12, 0);
	if (may_open)
		lv_obj_set_event_bubble(box, true);

	lv_obj_t *card = ui_card_create(box);
	if (may_open)
		lv_obj_set_event_bubble(card, true);
	unsigned shown = n < max ? n : max;
	for (unsigned i = 0; i < shown; i++)
		row_create(card, notification_get(i), i + 1 < shown, may_open, opening);
	if (n > shown) {
		lv_obj_t *more = ui_dim_label(box, "", &ui_font_20);
		ui_set_text_fmt(more, P_("%u more", "%u more", n - shown), n - shown);
	}
	return box;
}
