/*
 * Contacts in RAM and in /var/lib/contacts, conversations in /var/lib/messages, see
 * data.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "data.h"
#include "i18n.h"
#include "json.h"
#include "modem/modem.h"
#include "ui_symbols.h"

#ifndef CONTACTS /* the tests' is elsewhere */
#define CONTACTS "/var/lib/contacts"
#endif
#define CONTACTS_FILE CONTACTS "/contacts.json"
#ifndef MESSAGES
#define MESSAGES "/var/lib/messages"
#endif
#define MESSAGES_FILE MESSAGES "/messages.json"
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

/* The digits of a number (and '*', '#'), the last ones if it has too many */
static size_t digits_of(const char *s, char *out, size_t size)
{
	size_t n = 0;
	for (; *s; s++) {
		if (!((*s >= '0' && *s <= '9') || *s == '*' || *s == '#'))
			continue;
		if (n + 1 == size)
			memmove(out, out + 1, --n);
		out[n++] = *s;
	}
	out[n] = '\0';
	return n;
}

bool number_equal(const char *a, const char *b)
{
	char da[32], db[32];
	size_t na = digits_of(a, da, sizeof(da)), nb = digits_of(b, db, sizeof(db));
	if (!na || !nb) {
		if (na || nb)
			return false;
		/* Names: their letters and digits, in any case */
		for (;;) {
			while (*a && !isalnum((unsigned char)*a))
				a++;
			while (*b && !isalnum((unsigned char)*b))
				b++;
			if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
				return false;
			if (!*a)
				return true;
			a++;
			b++;
		}
	}
	if (strcmp(da, db) == 0)
		return true;
	size_t n = na < nb ? na : nb;
	if (n < 7)
		return false;
	n = n > 9 ? 9 : n;
	return strcmp(da + na - n, db + nb - n) == 0;
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

/* Conversations, most recent first, oldest message first in each, in MESSAGES_FILE:
 *
 *	[ { "number": "+40744555210", "unread": 1, "messages": [
 *		{ "text": "Lunch?", "time": 1762335000 },
 *		{ "text": "Yes", "time": 1762335060, "out": true },
 *		{ "text": "Hello", "time": 1762335100, "out": true, "state": "failed", "error": "No SIM card" } ] } ]
 *
 * "state" for an outgoing one not sent: "sending" (given to the modem; failed when
 * read back, phone-ui ended before it was sent) or "failed" */

static struct conversation **order;
static unsigned count, order_size;
static bool conversations_loaded;
unsigned data_version;

static struct conversation *conversation_new(const char *number)
{
	if (count == order_size) {
		order_size = order_size ? order_size * 2 : 32;
		order = realloc(order, order_size * sizeof(*order));
	}
	struct conversation *c = calloc(1, sizeof(*c));
	c->contact = contact_find(number);
	c->number = c->contact ? c->contact->number : ui_intern(number);
	order[count++] = c;
	return c;
}

static struct message *message_add(struct conversation *c)
{
	if (c->count == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->messages = realloc(c->messages, c->cap * sizeof(*c->messages));
	}
	struct message *m = &c->messages[c->count++];
	memset(m, 0, sizeof(*m));
	return m;
}

static void conversations_load(void)
{
	if (conversations_loaded)
		return;
	conversations_loaded = true;

	FILE *f = fopen(MESSAGES_FILE, "r");
	if (!f)
		return;
	struct stat st;
	char *text = NULL;
	if (fstat(fileno(f), &st) == 0) {
		text = malloc(st.st_size + 1);
		text[fread(text, 1, st.st_size, f)] = '\0';
	}
	fclose(f);
	struct json *root = text ? json_parse(text) : NULL;
	free(text);
	if (!root || root->kind != JSON_ARRAY) {
		printf("phone-ui: messages: " MESSAGES_FILE " is not a JSON array, ignored\n");
		json_free(root);
		return;
	}

	for (struct json *v = root->child; v; v = v->next) {
		const char *number = json_str(json_get(v, "number"), "");
		struct json *list = json_get(v, "messages");
		if (!*number || !list || !list->child)
			continue;
		/* The same number twice (written otherwise): the first conversation */
		struct conversation *c = conversation_new(number);
		c->unread = (unsigned)json_int(json_get(v, "unread"), 0);
		for (struct json *j = list->child; j; j = j->next) {
			const char *t = json_str(json_get(j, "text"), NULL);
			if (!t)
				continue;
			struct message *m = message_add(c);
			m->text = strdup(t);
			m->time = (time_t)json_int(json_get(j, "time"), 0);
			m->outgoing = json_bool(json_get(j, "out"));
			const char *state = json_str(json_get(j, "state"), "");
			m->state = !m->outgoing ? MESSAGE_RECEIVED : *state ? MESSAGE_FAILED : MESSAGE_SENT;
			const char *error = json_str(json_get(j, "error"), NULL);
			m->error = error && m->state == MESSAGE_FAILED ? ui_intern(error) : NULL;
		}
		if (!c->count) {
			free(c);
			count--;
		}
	}
	json_free(root);
}

/* Through a temporary file, synced, as the contacts */
static bool conversations_save(void)
{
	mkdir(MESSAGES, 0755);
	FILE *f = fopen(MESSAGES_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: messages");
		return false;
	}
	fputs("[", f);
	bool first = true;
	for (unsigned i = 0; i < count; i++) {
		struct conversation *c = order[i];
		if (!c->count)
			continue;
		fputs(first ? "\n\t{ " : ",\n\t{ ", f);
		first = false;
		json_put(f, "number", c->number);
		if (c->unread)
			fprintf(f, ", \"unread\": %u", c->unread);
		fputs(", \"messages\": [", f);
		for (unsigned k = 0; k < c->count; k++) {
			const struct message *m = &c->messages[k];
			fputs(k ? ",\n\t\t{ " : "\n\t\t{ ", f);
			json_put(f, "text", m->text);
			fprintf(f, ", \"time\": %lld", (long long)m->time);
			if (m->outgoing)
				fputs(", \"out\": true", f);
			if (m->state == MESSAGE_SENDING || m->state == MESSAGE_FAILED)
				fputs(m->state == MESSAGE_SENDING ? ", \"state\": \"sending\"" : ", \"state\": \"failed\"", f);
			if (m->state == MESSAGE_FAILED && m->error) {
				fputs(", ", f);
				json_put(f, "error", m->error);
			}
			fputs(" }", f);
		}
		fputs(" ] }", f);
	}
	fputs("\n]\n", f);
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(MESSAGES_FILE ".new", MESSAGES_FILE) != 0) {
		perror("phone-ui: messages");
		unlink(MESSAGES_FILE ".new");
		return false;
	}
	return true;
}

/* Something changed in c: it becomes the most recent, saved */
static bool conversation_changed(struct conversation *c)
{
	unsigned i = 0;
	while (order[i] != c)
		i++;
	memmove(&order[1], &order[0], sizeof(order[0]) * i);
	order[0] = c;
	data_version++;
	return conversations_save();
}

/* The contacts changed: who each conversation is with */
static void conversations_relink(void)
{
	for (unsigned i = 0; i < count; i++)
		order[i]->contact = contact_find(order[i]->number);
}

unsigned conversation_count(void)
{
	conversations_load();
	return count;
}

struct conversation *conversation_get(unsigned i)
{
	conversations_load();
	return i < count ? order[i] : NULL;
}

struct conversation *conversation_find(const char *number, bool create)
{
	conversations_load();
	for (unsigned i = 0; i < count; i++)
		if (number_equal(order[i]->number, number))
			return order[i];
	if (!create)
		return NULL;

	/* New ones go last until something is sent */
	struct conversation *c = conversation_new(number);
	data_version++;
	return c;
}

/* To the modem, or failed with why it cannot send */
static void message_give(struct conversation *c, struct message *m)
{
	m->error = modem_sms_unavailable();
	m->id = m->error ? 0 : modem_sms_send(c->number, m->text);
	m->state = m->id ? MESSAGE_SENDING : MESSAGE_FAILED;
}

const struct message *conversation_send(struct conversation *c, const char *text)
{
	struct message *m = message_add(c);
	m->text = strdup(text);
	m->time = time(NULL);
	m->outgoing = true;
	message_give(c, m);
	conversation_changed(c);
	return m;
}

bool conversation_retry(struct conversation *c, unsigned i)
{
	if (i >= c->count || c->messages[i].state != MESSAGE_FAILED)
		return false;
	message_give(c, &c->messages[i]);
	data_version++;
	conversations_save();
	return true;
}

void conversation_read(struct conversation *c)
{
	if (!c->unread)
		return;
	c->unread = 0;
	data_version++;
	conversations_save();
}

bool conversation_received(const char *sender, time_t time, const char *text)
{
	struct conversation *c = conversation_find(sender, false);
	for (unsigned i = 0; c && i < c->count; i++) {
		const struct message *m = &c->messages[i];
		if (!m->outgoing && m->time == time && strcmp(m->text, text) == 0)
			return true; /* listed again: the modem did not delete it */
	}
	if (!c)
		c = conversation_new(sender);
	struct message *m = message_add(c);
	m->text = strdup(text);
	m->time = time;
	m->state = MESSAGE_RECEIVED;
	c->unread++;
	return conversation_changed(c);
}

void conversation_sent(unsigned id, bool ok)
{
	for (unsigned i = 0; i < count; i++) {
		struct conversation *c = order[i];
		for (unsigned k = 0; k < c->count; k++) {
			struct message *m = &c->messages[k];
			if (m->state != MESSAGE_SENDING || m->id != id)
				continue;
			m->state = ok ? MESSAGE_SENT : MESSAGE_FAILED;
			m->id = 0;
			data_version++;
			conversations_save();
			return;
		}
	}
}

const char *conversation_title(const struct conversation *c)
{
	return c->contact ? c->contact->name : c->number;
}

const char *message_time_text(time_t t, bool hour)
{
	time_t now = time(NULL);
	struct tm tn, tt;
	localtime_r(&now, &tn);
	localtime_r(&t, &tt);
	tn.tm_hour = tn.tm_min = tn.tm_sec = 0;
	time_t today = mktime(&tn); /* its midnight */

	/* The names of i18n.c: the phone has no locales for strftime()'s */
	char hm[8], buf[64];
	strftime(hm, sizeof(hm), "%H:%M", &tt);
	if (t >= today) /* or later: a clock behind the network's */
		return ui_intern(hm);
	if (t >= today - 86400 && !hour)
		return _("Yesterday");
	if (t >= today - 86400)
		snprintf(buf, sizeof(buf), _("Yesterday %s"), hm);
	else if (t >= today - 6 * 86400)
		snprintf(buf, sizeof(buf), "%s %s", ui_day_name(tt.tm_wday, true), hour ? hm : "");
	else if (tt.tm_year == tn.tm_year)
		snprintf(buf, sizeof(buf), "%d %s %s", tt.tm_mday, ui_month_name(tt.tm_mon, true), hour ? hm : "");
	else
		snprintf(buf, sizeof(buf), "%d %s %d", tt.tm_mday, ui_month_name(tt.tm_mon, true),
		         tt.tm_year + 1900);
	size_t n = strlen(buf);
	if (n && buf[n - 1] == ' ') /* without the hour */
		buf[n - 1] = '\0';
	return ui_intern(buf);
}

lv_obj_t *avatar_create(lv_obj_t *parent, const struct contact *contact, int32_t size)
{
	lv_obj_t *avatar = ui_avatar_create(parent, size);
	ui_avatar_set(avatar, contact ? contact->color : 0x636366, contact ? contact->initials : UI_SYMBOL_PHONE);
	return avatar;
}
