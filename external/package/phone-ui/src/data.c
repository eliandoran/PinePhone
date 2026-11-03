/*
 * Made-up contacts and conversations, see data.h.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "data.h"

const struct contact contacts[] = {
	{ "Aisha Khan",      "AK", "+44 7700 900123",   0xff375f },
	{ "Ana Popescu",     "AP", "+40 721 555 014",   0xbf5af2 },
	{ "Ben Carter",      "BC", "+1 415 555 0132",   0x0a84ff },
	{ "Chen Wei",        "CW", "+86 138 0013 8000", 0xff9f0a },
	{ "Daniel Moreau",   "DM", "+33 6 12 34 56 78", 0x5e5ce6 },
	{ "Elena Rossi",     "ER", "+39 347 555 0198",  0x30d158 },
	{ "Grace Okafor",    "GO", "+234 803 555 0147", 0xff453a },
	{ "Hiroshi Tanaka",  "HT", "+81 90 5555 0123",  0x64d2ff },
	{ "Lukas Schmidt",   "LS", "+49 151 5555 0199", 0xac8e68 },
	{ "Maria Garcia",    "MG", "+34 612 555 019",   0xff2d55 },
	{ "Mom",             "M",  "+40 744 555 210",   0xc99a06 },
	{ "Noah Williams",   "NW", "+1 212 555 0187",   0x34c759 },
	{ "Sofia Lindqvist", "SL", "+46 70 555 01 23",  0x40c8e0 },
};
const unsigned contact_count = sizeof(contacts) / sizeof(contacts[0]);

/* The next digit or '+' of a number, skipping spaces, dashes, parentheses */
static const char *number_next(const char *s)
{
	while (*s && !(*s >= '0' && *s <= '9') && *s != '+' && *s != '*' && *s != '#')
		s++;
	return s;
}

bool number_equal(const char *a, const char *b)
{
	for (;;) {
		a = number_next(a);
		b = number_next(b);
		if (*a != *b)
			return false;
		if (!*a)
			return true;
		a++;
		b++;
	}
}

const struct contact *contact_find(const char *number)
{
	if (!*number_next(number))
		return NULL;
	for (unsigned i = 0; i < contact_count; i++)
		if (number_equal(contacts[i].number, number))
			return &contacts[i];
	return NULL;
}

/* Conversations, oldest message first */

#define IN(text, time) { text, time, false, false }
#define OUT(text, time) { text, time, true, false }
#define SEED(number, messages) { number, NULL, messages, sizeof(messages) / sizeof(messages[0]), 0 }

static struct message mom[] = {
	IN("Did you eat today?", "Yesterday"),
	OUT("Yes mom, I had soup", "Yesterday"),
	IN("Good. Call your grandmother, it's her birthday on Sunday", "Yesterday"),
	OUT("I know, I'll call her tomorrow", "Yesterday"),
	IN("Are you coming for lunch on Sunday?", "08:12"),
	IN("I'm making sarmale", "08:12"),
};

static struct message ben[] = {
	IN("Is the phone thing actually booting now?", "Yesterday"),
	OUT("It boots, it has a UI and it runs on the GPU", "Yesterday"),
	OUT("Calls next, once the modem is driven", "Yesterday"),
	IN("Nice! Bring it on Friday, I want to see it", "Yesterday"),
	IN("Running 10 min late, grab us a table", "09:41"),
};

static struct message elena[] = {
	OUT("Thanks again for dinner, the risotto was amazing", "Mon"),
	IN("Anytime! I'll send you the recipe", "Mon"),
	IN("Saffron, a good stock and patience. That's all", "Mon"),
	OUT("Patience is the hard part", "Mon"),
};

static struct message lukas[] = {
	IN("Train is delayed again. 40 minutes", "Sun"),
	OUT("Classic. I'll wait at the cafe by the station", "Sun"),
	IN("Perfect, see you there", "Sun"),
};

static struct message grace[] = {
	IN("Happy birthday!! Hope you have a great day", "Sat"),
	OUT("Thank you Grace!", "Sat"),
	IN("Drinks next week to celebrate?", "Sat"),
	OUT("Absolutely, Thursday?", "Sat"),
	IN("Thursday works", "Sat"),
};

static struct message parcel[] = {
	IN("Your parcel 4821 will be delivered today between 13:00 and 15:00. "
	   "Reply STOP to unsubscribe.", "Fri"),
};

#define MAX_CONVERSATIONS 64

static struct conversation pool[MAX_CONVERSATIONS] = {
	SEED("+1 415 555 0132", ben),
	SEED("+40 744 555 210", mom),
	SEED("+39 347 555 0198", elena),
	SEED("+49 151 5555 0199", lukas),
	SEED("+234 803 555 0147", grace),
	SEED("7726", parcel),
};
static struct conversation *order[MAX_CONVERSATIONS]; /* most recent first */
static unsigned count;
unsigned data_version;

static void conversations_init(void)
{
	if (count)
		return;
	while (count < MAX_CONVERSATIONS && pool[count].number) {
		pool[count].contact = contact_find(pool[count].number);
		order[count] = &pool[count];
		count++;
	}
}

unsigned conversation_count(void)
{
	conversations_init();
	return count;
}

struct conversation *conversation_get(unsigned i)
{
	conversations_init();
	return i < count ? order[i] : NULL;
}

struct conversation *conversation_find(const char *number, bool create)
{
	conversations_init();
	for (unsigned i = 0; i < count; i++)
		if (number_equal(order[i]->number, number))
			return order[i];
	if (!create || count == MAX_CONVERSATIONS)
		return NULL;

	/* New ones go last until something is sent */
	struct conversation *c = &pool[count];
	c->contact = contact_find(number);
	c->number = c->contact ? c->contact->number : ui_intern(number);
	order[count++] = c;
	data_version++;
	return c;
}

const struct message *conversation_send(struct conversation *c, const char *text)
{
	if (c->count == c->cap) {
		unsigned cap = c->cap ? c->cap * 2 : c->count + 16;
		struct message *m = malloc(sizeof(*m) * cap);
		if (c->count)
			memcpy(m, c->messages, sizeof(*m) * c->count);
		if (c->cap)
			free(c->messages);
		c->messages = m;
		c->cap = cap;
	}

	char buf[8];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);

	struct message *m = &c->messages[c->count++];
	*m = (struct message){ ui_intern(text), ui_intern(buf), true, true };

	unsigned i = 0;
	while (order[i] != c)
		i++;
	memmove(&order[1], &order[0], sizeof(order[0]) * i);
	order[0] = c;
	data_version++;
	return m;
}

const char *conversation_title(const struct conversation *c)
{
	return c->contact ? c->contact->name : c->number;
}

lv_obj_t *avatar_create(lv_obj_t *parent, const struct contact *contact, int32_t size)
{
	lv_obj_t *avatar = lv_obj_create(parent);
	lv_obj_remove_style_all(avatar);
	lv_obj_set_size(avatar, size, size);
	lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(avatar, lv_color_hex(contact ? contact->color : 0x636366), 0);
	lv_obj_set_clickable(avatar, false);
	lv_obj_set_scrollable(avatar, false);

	lv_obj_t *label = lv_label_create(avatar);
	lv_label_set_text_static(label, contact ? contact->initials : LV_SYMBOL_CALL);
	lv_obj_set_style_text_font(label, size >= 160 ? &lv_font_montserrat_48 :
	                           size >= 96 ? &lv_font_montserrat_32 :
	                           size >= 72 ? &lv_font_montserrat_26 : &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_center(label);
	return avatar;
}
