/*
 * Contacts in RAM and in /var/lib/contacts, conversations made up, see data.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "data.h"
#include "json.h"

#ifndef CONTACTS /* the tests' is elsewhere */
#define CONTACTS "/var/lib/contacts"
#endif
#define CONTACTS_FILE CONTACTS "/contacts.json"
#define MAX_FILE (1024 * 1024)

/* Avatar colors, picked by the name */
static const uint32_t avatar_colors[] = {
	0xff375f, 0xbf5af2, 0x0a84ff, 0xff9f0a, 0x5e5ce6, 0x30d158, 0xff453a,
	0x64d2ff, 0xac8e68, 0xff2d55, 0xc99a06, 0x34c759, 0x40c8e0,
};

static struct contact **book; /* sorted; deleted contacts leave it but are not freed */
static unsigned book_count, book_size;
static bool book_loaded;

static void conversations_relink(void);

unsigned contact_section(const struct contact *c)
{
	char ch = c->name[0];
	if (ch >= 'a' && ch <= 'z')
		return ch - 'a';
	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A';
	return 26;
}

static int contact_cmp(const void *a, const void *b)
{
	const struct contact *x = *(struct contact *const *)a, *y = *(struct contact *const *)b;
	unsigned sx = contact_section(x), sy = contact_section(y);
	if (sx != sy)
		return sx < sy ? -1 : 1;
	int r = strcasecmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

/* The UTF-8 character at s appended to out, in upper case for ASCII and Latin-1;
 * returns its length */
static size_t initial_put(char *out, const char *s)
{
	unsigned char c = s[0], c1 = s[1];
	size_t n = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
	n = strnlen(s, n);
	memcpy(out, s, n);
	if (n == 1 && c >= 'a' && c <= 'z')
		out[0] = c - 'a' + 'A';
	else if (n == 2 && c == 0xc3 && c1 >= 0xa0 && c1 <= 0xbe && c1 != 0xb7)
		out[1] = c1 - 0x20;
	return n;
}

/* `text` without the spaces around it, interned */
static const char *trimmed(const char *text)
{
	text += strspn(text, " \t\r\n");
	size_t n = strlen(text);
	while (n && strchr(" \t\r\n", text[n - 1]))
		n--;
	char *copy = strndup(text, n);
	const char *s = ui_intern(copy);
	free(copy);
	return s;
}

static void contact_set(struct contact *c, const char *name, const char *number)
{
	c->name = trimmed(name);
	c->number = trimmed(number);

	/* Initials: the first letters of the first and the last word */
	char buf[16];
	size_t n = c->name[0] ? initial_put(buf, c->name) : 0;
	const char *last = strrchr(c->name, ' ');
	if (last && last[1])
		n += initial_put(buf + n, last + 1);
	buf[n] = '\0';
	c->initials = ui_intern(buf);

	unsigned hash = 5381;
	for (const char *s = c->name; *s; s++)
		hash = hash * 33 + (unsigned char)*s;
	c->color = avatar_colors[hash % (sizeof(avatar_colors) / sizeof(avatar_colors[0]))];
}

static void book_append(const char *name, const char *number, bool favorite)
{
	if (book_count == book_size) {
		book_size = book_size ? book_size * 2 : 32;
		book = realloc(book, book_size * sizeof(*book));
	}
	struct contact *c = calloc(1, sizeof(*c));
	contact_set(c, name, number);
	c->favorite = favorite;
	book[book_count++] = c;
}

static void book_load(void)
{
	if (book_loaded)
		return;
	book_loaded = true;

	FILE *f = fopen(CONTACTS_FILE, "r");
	if (!f)
		return;
	char *text = malloc(MAX_FILE + 1);
	size_t n = fread(text, 1, MAX_FILE, f);
	fclose(f);
	text[n] = '\0';

	struct json *root = json_parse(text);
	free(text);
	if (!root || root->kind != JSON_ARRAY)
		printf("phone-ui: contacts: " CONTACTS_FILE " is not a JSON array, ignored\n");
	else
		for (struct json *v = root->child; v; v = v->next)
			book_append(json_str(json_get(v, "name"), ""), json_str(json_get(v, "number"), ""),
			            json_bool(json_get(v, "favorite")));
	json_free(root);
	if (book_count)
		qsort(book, book_count, sizeof(*book), contact_cmp);
}

static void json_put(FILE *f, const char *key, const char *text)
{
	size_t size = 6 * strlen(text) + 1;
	char *buf = malloc(size);
	json_escape(buf, size, text);
	fprintf(f, "\"%s\": \"%s\"", key, buf);
	free(buf);
}

/* Through a temporary file, synced: the battery can cut power at any time */
static bool book_save(void)
{
	mkdir(CONTACTS, 0755);
	FILE *f = fopen(CONTACTS_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: contacts");
		return false;
	}
	fputs("[", f);
	for (unsigned i = 0; i < book_count; i++) {
		fputs(i ? ",\n\t{ " : "\n\t{ ", f);
		json_put(f, "name", book[i]->name);
		fputs(", ", f);
		json_put(f, "number", book[i]->number);
		if (book[i]->favorite)
			fputs(", \"favorite\": true", f);
		fputs(" }", f);
	}
	fputs("\n]\n", f);
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(CONTACTS_FILE ".new", CONTACTS_FILE) != 0) {
		perror("phone-ui: contacts");
		unlink(CONTACTS_FILE ".new");
		return false;
	}
	return true;
}

static bool book_changed(void)
{
	qsort(book, book_count, sizeof(*book), contact_cmp);
	conversations_relink();
	data_version++;
	return book_save();
}

static int book_index(const struct contact *c)
{
	for (unsigned i = 0; i < book_count; i++)
		if (book[i] == c)
			return i;
	return -1;
}

unsigned contact_count(void)
{
	book_load();
	return book_count;
}

const struct contact *contact_get(unsigned i)
{
	book_load();
	return i < book_count ? book[i] : NULL;
}

bool contact_add(const char *name, const char *number)
{
	book_load();
	book_append(name, number, false);
	return book_changed();
}

bool contact_update(const struct contact *c, const char *name, const char *number)
{
	if (book_index(c) < 0)
		return false;
	contact_set((struct contact *)c, name, number);
	return book_changed();
}

bool contact_delete(const struct contact *c)
{
	int i = book_index(c);
	if (i < 0)
		return false;
	memmove(&book[i], &book[i + 1], (book_count - i - 1) * sizeof(*book));
	book_count--;
	return book_changed();
}

bool contact_set_favorite(const struct contact *c, bool favorite)
{
	if (book_index(c) < 0)
		return false;
	((struct contact *)c)->favorite = favorite;
	data_version++;
	return book_save();
}

bool contact_exists(const struct contact *c)
{
	return book_index(c) >= 0;
}

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
	book_load();
	for (unsigned i = 0; i < book_count; i++)
		if (number_equal(book[i]->number, number))
			return book[i];
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

/* The contacts changed: who each conversation is with */
static void conversations_relink(void)
{
	for (unsigned i = 0; i < count; i++)
		pool[i].contact = contact_find(pool[i].number);
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
	/* A seed (cap 0) is copied before it grows */
	if (!c->cap || c->count == c->cap) {
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
	lv_obj_t *avatar = ui_avatar_create(parent, size);
	ui_avatar_set(avatar, contact ? contact->color : 0x636366, contact ? contact->initials : LV_SYMBOL_CALL);
	return avatar;
}
