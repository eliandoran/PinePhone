/* data.c: contacts (saved in CONTACTS, a directory of the test build), conversations */

#include <sys/stat.h>

#include "test.h"

#include "data.h"
#include "json.h"

#define CONTACTS_FILE CONTACTS "/contacts.json"

/* Each test starts without contacts, or with the ones it writes */
TEST_SETUP
{
	mkdir(CONTACTS, 0755);
	unlink(CONTACTS_FILE);
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
	CHECK(!number_equal("+14155550132", "14155550132"));
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

TEST(conversations_seeded)
{
	CHECK(conversation_count() > 0);
	struct conversation *c = conversation_find("+14155550132", false);
	CHECK(c && c->count > 0);
	CHECK(!c->contact);
	CHECK_STR(conversation_title(c), "+1 415 555 0132");
	CHECK(!conversation_find("+1 999", false));
}

TEST(conversation_send)
{
	struct conversation *c = conversation_find("+234 803 555 0147", false);
	unsigned count = c->count;
	CHECK(conversation_get(0) != c);

	const struct message *m = conversation_send(c, "On my way");
	CHECK_STR(m->text, "On my way");
	CHECK(m->outgoing && m->unsent);
	CHECK_INT(strlen(m->time), 5); /* "HH:MM" */
	CHECK_INT(c->count, count + 1);
	CHECK(conversation_get(0) == c);

	/* Past the seed's room: the earlier messages are kept */
	for (int i = 0; i < 40; i++)
		conversation_send(c, "again");
	CHECK_INT(c->count, count + 41);
	CHECK_STR(c->messages[count].text, "On my way");
	CHECK(conversation_get(0) == c);
}

TEST(conversation_new)
{
	unsigned count = conversation_count();
	struct conversation *c = conversation_find("555 1234", true);
	CHECK(c && c->count == 0);
	CHECK_INT(conversation_count(), count + 1);
	CHECK(conversation_get(count) == c); /* last until something is sent */
	CHECK(conversation_find("5551234", false) == c);
}

TEST(conversation_linked_to_contacts)
{
	struct conversation *c = conversation_find("+1 415 555 0132", false);
	contact_add("Ben", "+14155550132");
	CHECK(c->contact == named("Ben"));
	CHECK_STR(conversation_title(c), "Ben");
	contact_delete(named("Ben"));
	CHECK(!c->contact);
}
