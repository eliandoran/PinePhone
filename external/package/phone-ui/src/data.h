/*
 * Contacts and conversations shared by Phone, Contacts and Messages. Contacts are
 * an address book in /var/lib/contacts/contacts.json; conversations (SMS) are in
 * /var/lib/messages/messages.json, sent and received through the modem (modem.h).
 *
 * Every text here stays valid for the life of the program (static data or
 * ui_intern()), so labels can show it with lv_label_set_text_static().
 */

#ifndef PHONE_UI_DATA_H
#define PHONE_UI_DATA_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "ui.h"

struct contact {
	const char *name;
	const char *initials;
	const char *number; /* as displayed, e.g. "+44 7700 900123" */
	uint32_t color;     /* avatar */
	bool favorite;      /* listed first, in Favorites */
};

/* Sorted by name, letters first ("#" after Z). A contact stays valid after it is
 * changed or deleted (its texts are replaced, nothing is freed): screens showing
 * it keep working. Changes are saved at once; false if the file could not be
 * written (the change is kept in RAM). */
unsigned contact_count(void);
const struct contact *contact_get(unsigned i);
bool contact_add(const char *name, const char *number);
bool contact_update(const struct contact *c, const char *name, const char *number);
bool contact_delete(const struct contact *c);
bool contact_set_favorite(const struct contact *c, bool favorite);
bool contact_exists(const struct contact *c); /* not deleted */
/* The section of its first letter: 0-25 for A-Z, 26 ("#") for the rest */
unsigned contact_section(const struct contact *c);

/* Numbers match when their digits are the same ("+1 415-555 0132" is
 * "+14155550132"), or their last 9 when both have 7 at least: written with the
 * country code or without ("+40 744 555 210" is "0744555210"). Names (a sender
 * such as "Vodafone") match by their letters, in any case */
bool number_equal(const char *a, const char *b);
const struct contact *contact_find(const char *number);

enum message_state {
	MESSAGE_RECEIVED,
	MESSAGE_SENDING, /* given to the modem */
	MESSAGE_SENT,
	MESSAGE_FAILED,  /* not sent: tried again with conversation_retry() */
};

struct message {
	const char *text;
	time_t time;        /* written, or received (the service centre's) */
	bool outgoing;
	enum message_state state;
	const char *error;  /* failed: why ("No SIM card"), NULL not known */
	unsigned id;        /* sending: the modem's (modem_sms_send()) */
};

struct conversation {
	const char *number;
	const struct contact *contact; /* NULL: not in the contacts */
	struct message *messages;      /* oldest first; moves when one is added */
	unsigned count, cap;
	unsigned unread;               /* received since its thread was last open */
};

/* Most recent first. Saved at each change, conversations without messages left out.
 * A conversation stays valid for the life of the program. */
unsigned conversation_count(void);
struct conversation *conversation_get(unsigned i);
/* The conversation with `number`, a new empty one if `create` (else NULL) */
struct conversation *conversation_find(const char *number, bool create);
/* Appends an outgoing message (text copied), gives it to the modem (failed at once,
 * with why, without a modem that can send) and makes the conversation the most recent */
const struct message *conversation_send(struct conversation *c, const char *text);
/* Message i failed: given to the modem again, in its place; false if it did not fail */
bool conversation_retry(struct conversation *c, unsigned i);
/* Its thread was seen: nothing unread */
void conversation_read(struct conversation *c);
const char *conversation_title(const struct conversation *c); /* name or number */
/* From the modem (modem.c): a message received, kept unless it was already (the same
 * sender, time and text); false if it could not be saved. And the result of a send */
bool conversation_received(const char *sender, time_t time, const char *text);
void conversation_sent(unsigned id, bool ok);
/* When a message was written: "09:41" today, "Yesterday", "Mon" this week, "12 Oct",
 * "12 Oct 2024" in another year; `hour`: with the hour after the day ("Mon 09:41"),
 * not with the year */
const char *message_time_text(time_t time, bool hour);

/* Call log (calls.c): the latest 500 calls, in /var/lib/phone, logged by modem.c as
 * they end (by phone_call() when one cannot be made) */
enum call_kind { CALL_OUTGOING, CALL_INCOMING, CALL_MISSED };

struct call {
	const char *number; /* stays valid (ui_intern()) */
	time_t time;        /* when it started */
	unsigned duration;  /* seconds connected, 0: not answered */
	enum call_kind kind;
	bool unseen;        /* missed, not seen in Recents yet: a notification */
};

/* Most recent first; the pointer is valid until the next change */
unsigned call_count(void);
const struct call *call_get(unsigned i);
void call_log_add(const char *number, enum call_kind kind, time_t start, unsigned duration);
void call_log_clear(void);
/* Missed calls not seen yet; all seen (Recents shown, notifications cleared) */
unsigned call_missed_unseen(void);
void call_missed_seen(void);

/* Notifications (notify.c): what came while the user was not looking, until seen: a
 * conversation's messages unread, a number's missed calls not seen in Recents. Most
 * recent first; valid until data_version changes. */
enum notification_kind { NOTIFICATION_MESSAGES, NOTIFICATION_MISSED_CALLS };

struct notification {
	enum notification_kind kind;
	const char *number; /* the conversation's, the caller's ("" hidden); stays valid */
	unsigned count;     /* messages unread, calls missed */
	time_t time;        /* of the latest */
	const char *text;   /* the last message, NULL for calls */
};

unsigned notification_count(void);
const struct notification *notification_get(unsigned i);
/* What the apps' badges count: messages unread, missed calls not seen */
unsigned notification_messages(void);
unsigned notification_missed_calls(void);
/* All seen: conversations read, missed calls seen */
void notifications_clear(void);

/* Bumped on every change of contacts, conversations or calls, for lists that show them */
extern unsigned data_version;

/* Round avatar with the contact's initials, a gray one with a phone for unknown
 * numbers (contact NULL). Not clickable. */
lv_obj_t *avatar_create(lv_obj_t *parent, const struct contact *contact, int32_t size);

#endif
