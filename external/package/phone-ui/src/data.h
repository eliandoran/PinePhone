/*
 * Contacts and conversations shared by Phone, Contacts and Messages. Contacts are
 * an address book in /var/lib/contacts/contacts.json; conversations are made up
 * until there is a modem, messages sent at runtime are kept in RAM only.
 *
 * Every text here stays valid for the life of the program (static data or
 * ui_intern()), so labels can show it with lv_label_set_text_static().
 */

#ifndef PHONE_UI_DATA_H
#define PHONE_UI_DATA_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

struct contact {
	const char *name;
	const char *initials;
	const char *number; /* as displayed, e.g. "+44 7700 900123" */
	uint32_t color;     /* avatar */
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
bool contact_exists(const struct contact *c); /* not deleted */
/* The section of its first letter: 0-25 for A-Z, 26 ("#") for the rest */
unsigned contact_section(const struct contact *c);

/* Numbers match when their digits (and '+') are the same: "+1 415-555 0132"
 * is "+14155550132" */
bool number_equal(const char *a, const char *b);
const struct contact *contact_find(const char *number);

struct message {
	const char *text;
	const char *time; /* "09:41", "Yesterday", "Mon"... */
	bool outgoing;
	bool unsent;      /* typed here: there is no modem to send it */
};

struct conversation {
	const char *number;
	const struct contact *contact; /* NULL: not in the contacts */
	struct message *messages;      /* oldest first */
	unsigned count, cap;           /* cap 0: messages is the static seed */
};

/* Most recent first */
unsigned conversation_count(void);
struct conversation *conversation_get(unsigned i);
/* The conversation with `number`, a new empty one if `create` (else NULL) */
struct conversation *conversation_find(const char *number, bool create);
/* Appends an outgoing message (text copied) and makes the conversation the most recent */
const struct message *conversation_send(struct conversation *c, const char *text);
const char *conversation_title(const struct conversation *c); /* name or number */

/* Bumped on every change of contacts or conversations, for lists that show them */
extern unsigned data_version;

/* Round avatar with the contact's initials, a gray one with a phone for unknown
 * numbers (contact NULL). Not clickable. */
lv_obj_t *avatar_create(lv_obj_t *parent, const struct contact *contact, int32_t size);

#endif
