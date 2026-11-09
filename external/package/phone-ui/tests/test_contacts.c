/* Contacts (contacts.c, the address book of data.c), through the UI (ui_test.h) */

#include "test.h"
#include "i18n.h"
#include "ui_test.h"

#include "json.h"
#include "ui_symbols.h"

#define CONTACTS "/var/lib/contacts/contacts.json"
#define STAR_ON UI_SYMBOL_STAR
#define STAR_OFF UI_SYMBOL_STAR_REGULAR

/* Started by the first ui_test_open(): the call log written before, as phone-ui reads it
 * at startup (the status bar's missed calls) */
TEST_SETUP
{
	ui_test_reset();
}

static void ada(void)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Ada Lovelace\", \"number\": \"+44 7700 900123\" } ]");
}

/* The address book as saved, to be freed */
static struct json *saved(void)
{
	static char text[8192];
	FILE *f = fopen(CONTACTS, "r");
	if (!f)
		return NULL;
	text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
	fclose(f);
	return json_parse(text);
}

static int32_t y_of(const char *text)
{
	lv_obj_t *obj = ui_test_find(text);
	lv_area_t a = { 0 };
	if (obj)
		lv_obj_get_coords(obj, &a);
	return obj ? a.y1 : -1;
}

/* A section heading: of the labels with its letter (avatars show initials), the
 * least nested */
static int32_t y_of_section(const char *letter)
{
	lv_obj_t *found[16], *best = NULL;
	unsigned n = ui_test_find_all(letter, found, 16);
	int best_depth = 0;
	for (unsigned i = 0; i < n && i < 16; i++) {
		int depth = 0;
		for (lv_obj_t *o = found[i]; o; o = lv_obj_get_parent(o))
			depth++;
		if (!best || depth < best_depth) {
			best = found[i];
			best_depth = depth;
		}
	}
	lv_area_t a = { 0 };
	if (best)
		lv_obj_get_coords(best, &a);
	return best ? a.y1 : -1;
}

static void cursor_to_end(void)
{
	lv_obj_t *ta = ui_test_textarea();
	CHECK(ta);
	if (ta)
		lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
}

TEST(empty)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_shows("No contacts"));
}

TEST(add)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("New contact"));
	/* Starts on the name; Enter goes to the number */
	CHECK(ui_test_type("Ada Lovelace\n"));
	CHECK(ui_test_textarea() && lv_textarea_get_accepted_chars(ui_test_textarea()));
	CHECK(ui_test_type("+44 7700 900123"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));

	CHECK(ui_test_shows("Contacts"));
	CHECK(ui_test_shows("A"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_shows("1 contact"));
	struct json *root = saved();
	CHECK(root && root->child && !root->child->next);
	if (root && root->child) {
		CHECK_STR(json_str(json_get(root->child, "name"), NULL), "Ada Lovelace");
		CHECK_STR(json_str(json_get(root->child, "number"), NULL), "+44 7700 900123");
	}
	json_free(root);
}

/* Added from the round button at the bottom right, as Messages' new message */
/* The keyboard hidden with its key: what was typed stays, saved with the check mark */
TEST(keyboard_hidden_while_adding)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Ada Lovelace\n+44 7700 900123"));
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	CHECK(!ui_test_textarea());
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("Ada Lovelace"));
}

TEST(add_button)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_action_button(LV_SYMBOL_PLUS));
}

/* Scrolled to the end, the count under the list is not under the button */
TEST(add_button_clear_of_the_end)
{
	char text[4096];
	size_t n = snprintf(text, sizeof(text), "[");
	for (int i = 0; i < 20; i++)
		n += snprintf(text + n, sizeof(text) - n, "%s { \"name\": \"%c Person\", \"number\": \"+1 415 555 01%02d\" }",
		              i ? "," : "", 'A' + i, i);
	snprintf(text + n, sizeof(text) - n, " ]");
	ui_test_write(CONTACTS, text);
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_clear_of_action_button("20 contacts", LV_SYMBOL_PLUS));
}

TEST(saved_with_enter_on_the_number)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Ben\n555 0100\n"));
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("1 contact"));
}

TEST(needs_a_name_and_a_number)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("New contact"));

	/* A name, no number: the number field gets the keyboard */
	CHECK(ui_test_type("Ada"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_textarea() && strcmp(lv_textarea_get_placeholder_text(ui_test_textarea()),
	                                   "+44 7700 900123") == 0);
	/* Without a digit it is not a number */
	CHECK(ui_test_type("+ ()"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_type("1"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("Ada"));
	CHECK(!ui_test_shows("New contact"));
}

TEST(blank_name_refused)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("   \n123"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("New contact"));
	CHECK(!saved());
}

TEST(only_number_characters)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Ada\n"));
	/* The dial pad has no letters; text put in otherwise loses them */
	lv_textarea_add_text(ui_test_textarea(), "07a7b0c0");
	CHECK_STR(ui_test_typed(), "07700");
}

TEST(cancelled_by_back)
{
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Ada\n123"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("No contacts"));
	CHECK(!saved());
}

TEST(sections_and_favorites)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Zoe\", \"number\": \"1\" },\n"
	                        "  { \"name\": \"ada\", \"number\": \"2\" },\n"
	                        "  { \"name\": \"Ben\", \"number\": \"3\", \"favorite\": true },\n"
	                        "  { \"name\": \"112 Emergency\", \"number\": \"112\" } ]");
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_shows("4 contacts"));
	int32_t fav = y_of(STAR_ON "  Favorites"), a = y_of_section("A"), b = y_of_section("B"),
	        z = y_of_section("Z"), other = y_of_section("#");
	CHECK(fav >= 0 && a > fav && b > a && z > b && other > z);
	CHECK(!ui_test_shows("C"));
	/* A favorite is listed in both */
	lv_obj_t *bens[4];
	CHECK_INT(ui_test_find_all("Ben", bens, 4), 2);
	CHECK(y_of("Ben") > fav && y_of("Ben") < a);
	CHECK(y_of("ada") > a && y_of("ada") < b);
	CHECK(y_of("112 Emergency") > other);
}

TEST(detail)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_shows("AL")); /* the avatar */
	CHECK(ui_test_shows("mobile"));
	CHECK(ui_test_shows("+44 7700 900123"));
	CHECK(ui_test_shows("Call"));
	CHECK(ui_test_shows("Message"));
	CHECK(ui_test_shows(STAR_OFF));
	CHECK(ui_test_font("Call") == UI_FONT_BODY); /* the buttons' captions */
}

TEST(favorite)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(STAR_OFF));
	CHECK(ui_test_shows(STAR_ON));
	struct json *root = saved();
	CHECK(root && json_bool(json_get(root->child, "favorite")));
	json_free(root);

	CHECK(ui_test_back());
	CHECK(ui_test_shows(STAR_ON "  Favorites"));

	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(STAR_ON));
	CHECK(ui_test_shows(STAR_OFF));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows(STAR_ON "  Favorites"));
}

TEST(edit)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows("Edit contact"));
	CHECK(!ui_test_textarea()); /* reads first */
	CHECK(ui_test_tap_field("Ada Lovelace"));
	cursor_to_end();
	CHECK(ui_test_type(" King"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));

	/* Back on the detail, changed */
	CHECK(ui_test_shows("Ada Lovelace King"));
	CHECK(ui_test_shows("AK"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Ada Lovelace King"));
	CHECK(ui_test_shows("1 contact"));
	struct json *root = saved();
	CHECK(root && root->child);
	if (root && root->child)
		CHECK_STR(json_str(json_get(root->child, "name"), NULL), "Ada Lovelace King");
	json_free(root);
}

TEST(delete)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete contact"));
	CHECK(ui_test_shows("No contacts"));
	struct json *root = saved();
	CHECK(root && root->kind == JSON_ARRAY && !root->child);
	json_free(root);
}

TEST(call)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(UI_SYMBOL_PHONE));
	CHECK(ui_test_shows("No modem")); /* none in this test */
	CHECK(ui_test_shows("Ada Lovelace"));
}

TEST(message)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(LV_SYMBOL_ENVELOPE));
	CHECK(ui_test_shows("Ada Lovelace")); /* the thread's title */
	CHECK(ui_test_shows("No messages yet"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(!ui_test_shows("No messages yet"));

	/* In Messages from now on, by name */
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_shows("Hello"));
}

TEST(number_to_the_dialer)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text("+44 7700 900123"));
	CHECK(ui_test_shows("Keypad"));
	CHECK(ui_test_shows("+447700900123")); /* only what is dialed */
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(!ui_test_shows("Calling..."));
}

TEST(new_from_recents)
{
	ui_test_write("/var/lib/phone/calls.jsonl", "{ \"number\": \"+44 7700 900123\", \"time\": 1, "
	                                            "\"kind\": \"missed\", \"duration\": 0 }\n");
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("+44 7700 900123"));
	lv_obj_t *row = ui_test_find("+44 7700 900123");
	while (row && !lv_obj_is_clickable(row))
		row = lv_obj_get_parent(row);
	CHECK(row);
	if (!row)
		return;
	ui_test_tap(lv_obj_get_child(row, -1)); /* (i) */
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_type("Ada"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));

	/* Back on the recents, the call is now Ada's */
	CHECK(ui_test_shows("Ada"));
	CHECK(!ui_test_shows("+44 7700 900123"));
	/* And (i) shows her now */
	row = ui_test_find("Ada");
	while (row && !lv_obj_is_clickable(row))
		row = lv_obj_get_parent(row);
	if (row)
		ui_test_tap(lv_obj_get_child(row, -1));
	CHECK(ui_test_shows("Message"));
	CHECK(ui_test_shows("+44 7700 900123"));
}

/* Another number: the calls and messages of the old one are no longer hers */
TEST(renumbered)
{
	ada();
	ui_test_write("/var/lib/phone/calls.jsonl", "{ \"number\": \"+447700900123\", \"time\": 1, "
	                                            "\"kind\": \"incoming\", \"duration\": 5 }\n");
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_tap_text("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_tap_field("+44 7700 900123"));
	cursor_to_end();
	CHECK(ui_test_type("\b\b\b456"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("+44 7700 900456"));
	CHECK(ui_test_back());

	CHECK(ui_test_tap_text("Recents"));
	CHECK(ui_test_shows("+447700900123"));
	CHECK(!ui_test_shows("Ada Lovelace"));
}

TEST(in_phone)
{
	ada();
	ui_test_write("/var/lib/phone/calls.jsonl", "");
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("Contacts"));
	CHECK(ui_test_shows("Ada Lovelace"));
	/* Added from there: the list follows */
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Ben\n123"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("2 contacts"));
	CHECK(ui_test_tap_text("Ben"));
	CHECK(ui_test_shows("Favorite"));
}

/* In Romanian, switched to meanwhile: the count's plural, Ada's actions */
TEST(romanian)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Ada Lovelace\", \"number\": \"+44 7700 900123\" },\n"
	                        "  { \"name\": \"Ben\", \"number\": \"3\" } ]");
	ui_test_start();
	ui_language_set("ro", NULL);
	ui_test_settle();
	CHECK(ui_test_open("Contacte"));
	CHECK(ui_test_shows("2 contacte"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_shows("Apel"));
	CHECK(ui_test_shows("Mesaj"));
	CHECK(ui_test_shows("Favorit"));
}

TEST(romanian_one)
{
	ada();
	ui_test_start();
	ui_language_set("ro", NULL);
	ui_test_settle();
	CHECK(ui_test_open("Contacte"));
	CHECK(ui_test_shows("1 contact"));
}

/* Shared as a QR code: a vCard other phones' cameras offer to save */
TEST(share)
{
	ada();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Ada Lovelace"));
	CHECK(ui_test_tap_text(UI_SYMBOL_QRCODE));
	CHECK(ui_test_shows_qr("BEGIN:VCARD\r\nVERSION:3.0\r\nN:Lovelace;Ada;;;\r\nFN:Ada Lovelace\r\n"
	                       "TEL;TYPE=CELL:+44 7700 900123\r\nEND:VCARD\r\n"));
	CHECK(ui_test_shows("Ada Lovelace"));
	CHECK(ui_test_shows("+44 7700 900123"));
	ui_test_screenshot("build/contact_share.png");
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Message")); /* the contact again */
}
