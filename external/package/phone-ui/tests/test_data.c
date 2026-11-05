/* data.c: contacts (saved in CONTACTS, a directory of the test build), conversations
 * (in MESSAGES) */

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "test.h"

#include "data.h"
#include "i18n.h"
#include "json.h"

#define CONTACTS_FILE CONTACTS "/contacts.json"
#define MESSAGES_FILE MESSAGES "/messages.json"

/* Each test starts without contacts and conversations, or with the ones it writes */
TEST_SETUP
{
	mkdir(CONTACTS, 0755);
	unlink(CONTACTS_FILE);
	mkdir(MESSAGES, 0755);
	unlink(MESSAGES_FILE);
}

static void write_file(const char *text)
{
	FILE *f = fopen(CONTACTS_FILE, "w");
	fputs(text, f);
	fclose(f);
}

static struct json *read_file(void)
{
	static char text[65536];
	FILE *f = fopen(CONTACTS_FILE, "r");
	if (!f)
		return NULL;
	text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
	fclose(f);
	return json_parse(text);
}

static const struct contact *named(const char *name)
{
	for (unsigned i = 0; i < contact_count(); i++)
		if (strcmp(contact_get(i)->name, name) == 0)
			return contact_get(i);
	return NULL;
}

TEST(number_equal)
{
	CHECK(number_equal("+1 415-555 0132", "+14155550132"));
	CHECK(number_equal("(0) 7700 900123", "07700900123"));
	CHECK(number_equal("*#06#", "* # 0 6 #"));
	CHECK(number_equal("", ""));
	/* The same number written with its country code or without: the last 9 digits
	 * (7 at least) */
	CHECK(number_equal("+14155550132", "14155550132"));
	CHECK(number_equal("+40 744 555 210", "0744555210"));
	CHECK(number_equal("+40744555210", "0040744555210"));
	CHECK(number_equal("5550132", "+14155550132"));
	CHECK(!number_equal("+40744555210", "+40744555211"));
	CHECK(!number_equal("555013", "+14155550132")); /* 6 digits: the whole number */
	/* Names (alphanumeric senders): their letters, in any case */
	CHECK(number_equal("Vodafone", "VODAFONE"));
	CHECK(!number_equal("Vodafone", "Orange"));
	CHECK(!number_equal("Vodafone", ""));
	CHECK(!number_equal("123", "1234"));
	CHECK(!number_equal("1234", "123"));
	CHECK(!number_equal("", "1"));
}

TEST(sections)
{
	struct contact c = { .name = "alice" };
	CHECK_INT(contact_section(&c), 0);
	c.name = "Zoe";
	CHECK_INT(contact_section(&c), 25);
	c.name = "\xc3\x89lodie"; /* É: not a letter of A-Z */
	CHECK_INT(contact_section(&c), 26);
	c.name = "112";
	CHECK_INT(contact_section(&c), 26);
	c.name = "";
	CHECK_INT(contact_section(&c), 26);
}

TEST(no_file)
{
	CHECK_INT(contact_count(), 0);
	CHECK(!contact_get(0));
	CHECK(!contact_find("+1 415 555 0132"));
}

TEST(load)
{
	write_file("[ { \"name\": \"Mom\", \"number\": \"+40 744 555 210\", \"favorite\": true },\n"
	           "  { \"name\": \"ben\", \"number\": \"+1 415 555 0132\" } ]");
	CHECK_INT(contact_count(), 2);
	CHECK_STR(contact_get(0)->name, "ben");
	CHECK(!contact_get(0)->favorite);
	CHECK_STR(contact_get(1)->name, "Mom");
	CHECK_STR(contact_get(1)->number, "+40 744 555 210");
	CHECK(contact_get(1)->favorite);
}

TEST(load_not_an_array)
{
	write_file("{ \"name\": \"Mom\" }");
	CHECK_INT(contact_count(), 0);
	write_file("garbage");
	CHECK_INT(contact_count(), 0); /* loaded once: still nothing */
}

TEST(sorted)
{
	contact_add("bob", "1");
	contact_add("Alice", "2");
	contact_add("112 Emergency", "112");
	contact_add("Charlie", "3");
	contact_add("alice", "4");
	CHECK_INT(contact_count(), 5);
	/* Letters first, case-insensitive (upper case first on a tie), then the rest */
	CHECK_STR(contact_get(0)->name, "Alice");
	CHECK_STR(contact_get(1)->name, "alice");
	CHECK_STR(contact_get(2)->name, "bob");
	CHECK_STR(contact_get(3)->name, "Charlie");
	CHECK_STR(contact_get(4)->name, "112 Emergency");
}

TEST(trimmed_and_initials)
{
	contact_add("  ada king lovelace \n", " +44 7700 900123 ");
	const struct contact *c = contact_get(0);
	CHECK_STR(c->name, "ada king lovelace");
	CHECK_STR(c->number, "+44 7700 900123");
	CHECK_STR(c->initials, "AL"); /* first and last word */

	contact_add("\xc3\xa9lodie", "1"); /* élodie: É */
	CHECK_STR(named("\xc3\xa9lodie")->initials, "\xc3\x89");
	contact_add("Grace", "2");
	CHECK_STR(named("Grace")->initials, "G");
	contact_add("", "3");
	CHECK_STR(named("")->initials, "");
}

TEST(avatar_color_by_name)
{
	contact_add("Ben", "1");
	contact_add("Ben", "2");
	CHECK_INT(contact_get(0)->color, contact_get(1)->color);
}

TEST(saved)
{
	contact_add("Ben", "+1 415 555 0132");
	contact_add("Say \"hi\"", "2");
	contact_set_favorite(named("Ben"), true);

	struct json *root = read_file();
	CHECK(root && root->kind == JSON_ARRAY);
	struct json *a = root ? root->child : NULL, *b = a ? a->next : NULL;
	CHECK_STR(json_str(json_get(a, "name"), NULL), "Ben");
	CHECK_STR(json_str(json_get(a, "number"), NULL), "+1 415 555 0132");
	CHECK(json_bool(json_get(a, "favorite")));
	CHECK_STR(json_str(json_get(b, "name"), NULL), "Say \"hi\"");
	CHECK(!json_get(b, "favorite"));
	CHECK(b && !b->next);
	json_free(root);
}

TEST(update_and_delete)
{
	contact_add("Ben", "1");
	contact_add("Zed", "2");
	const struct contact *ben = named("Ben");

	unsigned version = data_version;
	CHECK(contact_update(ben, "Xavier", "11"));
	CHECK(data_version != version);
	/* The same contact, renamed and sorted again */
	CHECK(contact_get(0) == ben);
	CHECK_STR(ben->name, "Xavier");
	CHECK_STR(ben->number, "11");

	CHECK(contact_delete(ben));
	CHECK(!contact_exists(ben));
	CHECK_INT(contact_count(), 1);
	CHECK_STR(ben->name, "Xavier"); /* still valid for screens showing it */
	CHECK(!contact_update(ben, "Ben", "1"));
	CHECK(!contact_delete(ben));

	struct json *root = read_file();
	CHECK_STR(json_str(json_get(root->child, "name"), NULL), "Zed");
	CHECK(!root->child->next);
	json_free(root);
}

TEST(find_by_number)
{
	contact_add("Ben", "+1 415 555 0132");
	CHECK(contact_find("+14155550132") == named("Ben"));
	CHECK(contact_find("+1 (415) 555-0132") == named("Ben"));
	CHECK(!contact_find("+1 415 555 0133"));
	CHECK(!contact_find(""));
	CHECK(!contact_find(" - "));
}

TEST(save_fails)
{
	rmdir(CONTACTS);
	/* A file where the directory should be: cannot save, kept in RAM */
	FILE *f = fopen(CONTACTS, "w");
	fclose(f);
	CHECK(!contact_add("Ben", "1"));
	CHECK_INT(contact_count(), 1);
	unlink(CONTACTS);
}

/* Conversations: in MESSAGES (a directory of the test build), sent through the modem's
 * functions, faked here */

static const char *unavailable; /* modem_sms_unavailable()'s answer */
static unsigned sends;          /* modem_sms_send() calls, its ids */
static char sent_to[64], sent_text[256];

const char *modem_sms_unavailable(void)
{
	return unavailable;
}

unsigned modem_sms_send(const char *number, const char *text)
{
	snprintf(sent_to, sizeof(sent_to), "%s", number);
	snprintf(sent_text, sizeof(sent_text), "%s", text);
	return ++sends;
}

static void write_messages(const char *text)
{
	mkdir(MESSAGES, 0755);
	FILE *f = fopen(MESSAGES_FILE, "w");
	fputs(text, f);
	fclose(f);
}

static struct json *read_messages(void)
{
	static char text[65536];
	FILE *f = fopen(MESSAGES_FILE, "r");
	if (!f)
		return NULL;
	text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
	fclose(f);
	return json_parse(text);
}

TEST(no_conversations)
{
	CHECK_INT(conversation_count(), 0);
	CHECK(!conversation_find("+14155550132", false));
}

TEST(conversations_loaded)
{
	write_messages("[ { \"number\": \"+1 415 555 0132\", \"unread\": 2, \"messages\": [\n"
	               "    { \"text\": \"Hi\", \"time\": 1000 },\n"
	               "    { \"text\": \"Yo\", \"time\": 2000, \"out\": true },\n"
	               "    { \"text\": \"Lost\", \"time\": 3000, \"out\": true, \"state\": \"failed\", \"error\": \"No SIM card\" },\n"
	               "    { \"text\": \"Cut\", \"time\": 4000, \"out\": true, \"state\": \"sending\" } ] },\n"
	               "  { \"number\": \"7726\", \"messages\": [ { \"text\": \"Parcel\", \"time\": 500 } ] },\n"
	               "  { \"number\": \"\", \"messages\": [ { \"text\": \"No one\", \"time\": 1 } ] },\n"
	               "  { \"number\": \"555\", \"messages\": [ ] } ]");
	CHECK_INT(conversation_count(), 2); /* without a number or messages: left out */
	struct conversation *c = conversation_get(0);
	CHECK_STR(c->number, "+1 415 555 0132");
	CHECK_STR(conversation_title(c), "+1 415 555 0132");
	CHECK_INT(c->unread, 2);
	CHECK_INT(c->count, 4);
	CHECK_STR(c->messages[0].text, "Hi");
	CHECK_INT(c->messages[0].time, 1000);
	CHECK(!c->messages[0].outgoing);
	CHECK(c->messages[0].state == MESSAGE_RECEIVED);
	CHECK(c->messages[1].outgoing && c->messages[1].state == MESSAGE_SENT);
	CHECK(c->messages[2].state == MESSAGE_FAILED);
	CHECK_STR(c->messages[2].error, "No SIM card");
	CHECK(c->messages[3].state == MESSAGE_FAILED); /* was being sent when phone-ui ended */
	CHECK_STR(conversation_get(1)->number, "7726");
}

TEST(conversations_not_an_array)
{
	write_messages("{ \"number\": \"1\" }");
	CHECK_INT(conversation_count(), 0);
}

TEST(conversation_send)
{
	write_messages("[ { \"number\": \"+1 415 555 0132\", \"messages\": [ { \"text\": \"Hi\", \"time\": 1000 } ] },\n"
	               "  { \"number\": \"+234 803 555 0147\", \"messages\": [ { \"text\": \"Yo\", \"time\": 900 } ] } ]");
	struct conversation *c = conversation_find("+2348035550147", false);
	CHECK(c && conversation_get(1) == c);

	time_t before = time(NULL);
	const struct message *m = conversation_send(c, "On my way");
	CHECK_STR(m->text, "On my way");
	CHECK(m->outgoing && m->state == MESSAGE_SENDING);
	CHECK(m->time >= before);
	CHECK_STR(sent_to, "+234 803 555 0147");
	CHECK_STR(sent_text, "On my way");
	CHECK_INT(c->count, 2);
	CHECK(conversation_get(0) == c);

	/* Saved, "sending" */
	struct json *root = read_messages();
	CHECK(root && root->child);
	if (root && root->child) {
		CHECK_STR(json_str(json_get(root->child, "number"), NULL), "+234 803 555 0147");
		struct json *last = json_get(root->child, "messages")->child->next;
		CHECK_STR(json_str(json_get(last, "text"), NULL), "On my way");
		CHECK_STR(json_str(json_get(last, "state"), NULL), "sending");
	}
	json_free(root);

	/* The modem's answer, by its id */
	conversation_sent(sends, true);
	CHECK(c->messages[1].state == MESSAGE_SENT);
	root = read_messages();
	CHECK(root && !json_get(json_get(root->child, "messages")->child->next, "state"));
	json_free(root);

	/* Growing: the earlier messages are kept */
	for (int i = 0; i < 40; i++)
		conversation_send(c, "again");
	CHECK_INT(c->count, 42);
	CHECK_STR(c->messages[1].text, "On my way");
}

TEST(conversation_send_fails)
{
	struct conversation *c = conversation_find("555 1234", true);
	const struct message *m = conversation_send(c, "Hello");
	conversation_sent(sends, false);
	CHECK(m->state == MESSAGE_FAILED);
	CHECK(!m->error); /* refused: no reason known */

	/* Again: sent once more, in its place */
	CHECK(conversation_retry(c, 0));
	CHECK(c->messages[0].state == MESSAGE_SENDING);
	CHECK_INT(sends, 2);
	CHECK_INT(c->count, 1);
	CHECK(!conversation_retry(c, 0)); /* not failed */
	CHECK(!conversation_retry(c, 1));

	/* Without a modem, a SIM: not sent, why */
	unavailable = "No SIM card";
	m = conversation_send(c, "Hello?");
	CHECK(m->state == MESSAGE_FAILED);
	CHECK_STR(m->error, "No SIM card");
	CHECK_INT(sends, 2);
	struct json *root = read_messages();
	struct json *last = root ? json_get(root->child, "messages")->child->next : NULL;
	CHECK_STR(json_str(json_get(last, "state"), NULL), "failed");
	CHECK_STR(json_str(json_get(last, "error"), NULL), "No SIM card");
	json_free(root);
}

TEST(conversation_new)
{
	write_messages("[ { \"number\": \"+1 415 555 0132\", \"messages\": [ { \"text\": \"Hi\", \"time\": 1000 } ] } ]");
	struct conversation *c = conversation_find("555 1234", true);
	CHECK(c && c->count == 0);
	CHECK_INT(conversation_count(), 2);
	CHECK(conversation_get(1) == c); /* last until something is sent */
	CHECK(conversation_find("5551234", false) == c);
	/* Not saved while empty */
	struct json *root = read_messages();
	CHECK(root && root->child && !root->child->next);
	json_free(root);
}

TEST(conversation_received)
{
	write_messages("[ { \"number\": \"+1 415 555 0132\", \"messages\": [ { \"text\": \"Hi\", \"time\": 1000 } ] },\n"
	               "  { \"number\": \"0744 555 210\", \"messages\": [ { \"text\": \"Yo\", \"time\": 900 } ] } ]");
	CHECK(conversation_received("+40744555210", 5000, "Lunch?"));
	struct conversation *c = conversation_get(0);
	CHECK_STR(c->number, "0744 555 210"); /* the same number, written otherwise */
	CHECK_INT(c->count, 2);
	CHECK_STR(c->messages[1].text, "Lunch?");
	CHECK_INT(c->messages[1].time, 5000);
	CHECK(!c->messages[1].outgoing && c->messages[1].state == MESSAGE_RECEIVED);
	CHECK_INT(c->unread, 1);

	/* The same again (the modem listed it twice): kept once */
	CHECK(conversation_received("+40744555210", 5000, "Lunch?"));
	CHECK_INT(c->count, 2);
	CHECK_INT(c->unread, 1);

	conversation_read(c);
	CHECK_INT(c->unread, 0);
	struct json *root = read_messages();
	CHECK(root && !json_get(root->child, "unread"));
	json_free(root);

	/* From a name: its own conversation, by name */
	CHECK(conversation_received("Vodafone", 6000, "Bill"));
	CHECK(conversation_received("Orange", 7000, "Hi"));
	CHECK_INT(conversation_count(), 4);
	CHECK_STR(conversation_title(conversation_get(1)), "Vodafone");
	CHECK(conversation_find("VODAFONE", false) == conversation_get(1));
}

TEST(conversation_received_not_saved)
{
	rmdir(MESSAGES);
	FILE *f = fopen(MESSAGES, "w"); /* a file where the directory should be */
	fclose(f);
	CHECK(!conversation_received("+40744555210", 5000, "Lunch?"));
	unlink(MESSAGES);
}

TEST(conversation_linked_to_contacts)
{
	struct conversation *c = conversation_find("+1 415 555 0132", true);
	contact_add("Ben", "+14155550132");
	CHECK(c->contact == named("Ben"));
	CHECK_STR(conversation_title(c), "Ben");
	contact_delete(named("Ben"));
	CHECK(!c->contact);
}

/* "09:41" today, "Yesterday", the day of the week, the date; with the hour in threads */
TEST(message_time_text)
{
	time_t now = time(NULL);
	time_t today = now - now % 86400 + 9 * 3600 + 41 * 60;
	CHECK_STR(message_time_text(today, false), "09:41");
	CHECK_STR(message_time_text(today, true), "09:41");
	CHECK_STR(message_time_text(today - 86400, false), "Yesterday");
	CHECK_STR(message_time_text(today - 86400, true), "Yesterday 09:41");

	char want[32];
	time_t t = today - 3 * 86400;
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(want, sizeof(want), "%a", &tm);
	CHECK_STR(message_time_text(t, false), want);
	strftime(want, sizeof(want), "%a 09:41", &tm);
	CHECK_STR(message_time_text(t, true), want);

	/* 20 days ago: this year, or (in January) the last */
	struct tm year;
	gmtime_r(&now, &year);
	t = today - 20 * 86400;
	gmtime_r(&t, &tm);
	bool same = tm.tm_year == year.tm_year;
	strftime(want, sizeof(want), same ? "%-d %b" : "%-d %b %Y", &tm);
	CHECK_STR(message_time_text(t, false), want);
	strftime(want, sizeof(want), same ? "%-d %b 09:41" : "%-d %b %Y", &tm);
	CHECK_STR(message_time_text(t, true), want);

	t = today - 400 * 86400;
	gmtime_r(&t, &tm);
	strftime(want, sizeof(want), "%-d %b %Y", &tm);
	CHECK_STR(message_time_text(t, false), want);
	CHECK_STR(message_time_text(t, true), want);
}

/* In Romanian: the day names of i18n.c, not strftime()'s */
TEST(message_time_text_romanian)
{
	CHECK(ui_language_use("ro"));
	time_t now = time(NULL);
	time_t today = now - now % 86400 + 9 * 3600 + 41 * 60;
	CHECK_STR(message_time_text(today - 86400, false), "Ieri");
	CHECK_STR(message_time_text(today - 86400, true), "Ieri 09:41");
	static const char *const days[] = { "dum.", "lun.", "mar.", "mie.", "joi", "vin.", "sâm." };
	char want[32];
	time_t t = today - 3 * 86400;
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(want, sizeof(want), "%s 09:41", days[tm.tm_wday]);
	CHECK_STR(message_time_text(t, true), want);
}
