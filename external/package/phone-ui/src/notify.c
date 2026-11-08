/*
 * Notifications, see data.h: what came while the user was not looking, made from the
 * conversations (messages unread) and the call log (missed calls not seen in Recents) as
 * they are, again when data_version changes. Nothing of their own is kept: reading a
 * thread or opening Recents, wherever it happens, is what makes one go. Plugin apps post
 * theirs (ui_notify()), kept here (in memory: the app posts them again as it starts).
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

/* Plugin apps' (ui_notify()): their keys stay valid until withdrawn (a card's row keeps a
 * copy anyway) */
static struct posted {
	const struct ui_app *app;
	char *key, *title, *text;
	unsigned count;
	time_t time;
	void (*open)(const char *key);
} *posted;
static unsigned posted_count;

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

	for (unsigned i = 0; i < posted_count; i++) {
		const struct posted *p = &posted[i];
		struct notification *n = add(NOTIFICATION_APP, p->key, p->time);
		n->count = p->count;
		n->text = p->text;
		n->app = p->app;
		n->title = p->title;
		n->open = p->open;
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

unsigned notification_app_count(const struct ui_app *app)
{
	unsigned n = 0;
	for (unsigned i = 0; i < posted_count; i++)
		if (posted[i].app == app)
			n += posted[i].count;
	return n;
}

unsigned notification_apps(void)
{
	return posted_count;
}

static void posted_free(struct posted *p)
{
	free(p->key);
	free(p->title);
	free(p->text);
}

void ui_notify(const char *app, const char *key, const char *title, const char *text,
               unsigned count, time_t time, bool alert, void (*open)(const char *key))
{
	const struct ui_app *a = ui_app_find(app);
	if (!a || !key)
		return;
	struct posted *p = NULL;
	for (unsigned i = 0; i < posted_count && !p; i++)
		if (posted[i].app == a && !strcmp(posted[i].key, key))
			p = &posted[i];
	data_version++;
	if (!count) {
		if (p) {
			posted_free(p);
			*p = posted[--posted_count];
		}
		return;
	}
	if (!p) {
		struct posted *grown = realloc(posted, (posted_count + 1) * sizeof(*posted));
		if (!grown)
			return;
		posted = grown;
		p = &posted[posted_count++];
		*p = (struct posted){ a, strdup(key), NULL, NULL, 0, 0, NULL };
	} else {
		free(p->title);
		free(p->text);
	}
	p->title = strdup(title ? title : "");
	p->text = strdup(text ? text : "");
	p->count = count;
	p->time = time;
	p->open = open;
	if (!alert)
		return;
	ui_vibrate(400); /* as an SMS */
	struct notification n = { NOTIFICATION_APP, p->key, count, time, p->text, a, p->title, open };
	notification_banner_app(&n);
}

void notification_open(const struct notification *n)
{
	/* Copied: opening it changes the list */
	enum notification_kind kind = n->kind;
	const char *number = n->number;
	void (*open)(const char *) = n->open;
	char *key = kind == NOTIFICATION_APP ? strdup(number) : NULL;
	if (ui_locked())
		ui_unlock();
	if (kind == NOTIFICATION_MESSAGES)
		messages_open_thread(number);
	else if (kind == NOTIFICATION_MISSED_CALLS)
		phone_open(); /* on Recents, where they are seen */
	else if (open && key)
		open(key);
	free(key);
}

void notifications_clear(void)
{
	for (unsigned i = 0; i < conversation_count(); i++)
		conversation_read(conversation_get(i));
	call_missed_seen();
	for (unsigned i = 0; i < posted_count; i++)
		posted_free(&posted[i]);
	if (posted_count)
		data_version++;
	posted_count = 0;
}

/* The card */

struct tap {
	enum notification_kind kind;
	const char *number; /* an app's key: a copy */
	const struct ui_app *app;
	void (*open)(const char *key);
	bool (*may_open)(void);
	void (*opening)(void);
};

static void free_cb(lv_event_t *e)
{
	struct tap *t = lv_event_get_user_data(e);
	if (t->kind == NOTIFICATION_APP)
		free((char *)t->number);
	free(t);
}

static void row_cb(lv_event_t *e)
{
	const struct tap *t = lv_event_get_user_data(e);
	if (t->may_open && !t->may_open())
		return;
	/* As it is now: the one tapped may have changed since the card was made */
	const struct notification *n = NULL;
	for (unsigned i = 0; i < notification_count() && !n && t->kind != NOTIFICATION_APP; i++)
		if (list[i].kind == t->kind && list[i].number == t->number)
			n = &list[i];
	struct notification gone = { t->kind, t->number, 0, 0, NULL, t->app, NULL, t->open };
	ui_vibrate(UI_VIBRATE_TAP);
	if (t->opening)
		t->opening();
	notification_open(n ? n : &gone);
}

static const char *title(const struct notification *n)
{
	if (n->kind == NOTIFICATION_APP)
		return n->title;
	const struct contact *c = contact_find(n->number);
	return c ? c->name : n->number[0] ? n->number : _("Unknown number");
}

static void row_create(lv_obj_t *card, const struct notification *n, bool divider,
                       bool (*may_open)(void), void (*opening)(void))
{
	struct tap *t = malloc(sizeof(*t));
	*t = (struct tap){ n->kind, n->kind == NOTIFICATION_APP ? strdup(n->number) : n->number, n->app, n->open,
	                    may_open, opening };
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, t);
	lv_obj_add_event_cb(row, free_cb, LV_EVENT_DELETE, t);
	/* What it is on can be dragged (the lock screen): the presses go on to it */
	if (may_open)
		lv_obj_set_event_bubble(row, true);
	if (n->kind == NOTIFICATION_APP)
		ui_app_icon_create(row, n->app, 72);
	else
		avatar_create(row, contact_find(n->number), 72);

	struct ui_row_lines l = ui_row_lines(row, "", "", "");
	ui_set_text(l.title, title(n));
	ui_set_text(l.time, message_time_text(n->time, false));
	lv_obj_t *what = l.preview;
	lv_obj_set_style_text_color(what, lv_color_hex(UI_COLOR_TEXT), 0); /* unread, as in Messages */
	if (n->kind == NOTIFICATION_MISSED_CALLS) {
		if (n->count == 1)
			lv_label_set_text_static(what, _("Missed call"));
		else
			ui_set_text_fmt(what, P_("%u missed call", "%u missed calls", n->count), n->count);
		lv_obj_set_style_text_color(what, lv_color_hex(UI_COLOR_RED), 0);
		return;
	}
	/* A copy: opening an app's notification withdraws it (its text freed) while the card
	 * is still drawn, the shade closing */
	lv_label_set_text(what, n->text);
	if (n->count > 1)
		ui_count_badge(lv_obj_get_parent(what), n->count);
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
		lv_obj_t *more = ui_dim_label(box, "", UI_FONT_CAPTION);
		ui_set_text_fmt(more, P_("%u more", "%u more", n - shown), n - shown);
	}
	return box;
}
