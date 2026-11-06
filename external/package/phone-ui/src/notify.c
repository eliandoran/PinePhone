/*
 * Notifications, see data.h: what came while the user was not looking, made from the
 * conversations (messages unread) and the call log (missed calls not seen in Recents) as
 * they are, again when data_version changes. Nothing of their own is kept: reading a
 * thread or opening Recents, wherever it happens, is what makes one go.
 */

#include <stdlib.h>
#include <string.h>

#include "data.h"

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

void notifications_clear(void)
{
	for (unsigned i = 0; i < conversation_count(); i++)
		conversation_read(conversation_get(i));
	call_missed_seen();
}
