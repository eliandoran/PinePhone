/* Messages (messages.c, the conversations of data.c), through the UI (ui_test.h) */

#include "test.h"
#include "ui_test.h"

#define CONTACTS "/var/lib/contacts/contacts.json"

TEST_SETUP
{
	ui_test_start();
}

/* The conversations are made up (data.c) until there is a modem */
TEST(conversations)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("Messages"));
	CHECK(ui_test_shows("+1 415 555 0132"));
	CHECK(ui_test_shows("Running 10 min late, grab us a table"));
	CHECK(ui_test_shows("09:41"));
	CHECK(ui_test_shows("7726"));
}

TEST(names_from_the_contacts)
{
	ui_test_write(CONTACTS, "[ { \"name\": \"Ben\", \"number\": \"+14155550132\" } ]");
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("Ben"));
	CHECK(!ui_test_shows("+1 415 555 0132"));
}

TEST(thread)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_shows("+1 415 555 0132")); /* the title */
	CHECK(ui_test_shows("Nice! Bring it on Friday, I want to see it"));
	CHECK(ui_test_shows("Running 10 min late, grab us a table"));
	CHECK(!ui_test_textarea()); /* no keyboard until the field is tapped */
}

TEST(send)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_shows("+39 347 555 0198"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("See you at 8"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_shows("See you at 8"));
	CHECK(ui_test_shows_part("Not sent: no SIM card"));
	CHECK(!ui_test_textarea()); /* the keyboard hidden */
	CHECK(ui_test_tap_field("Message"));
	CHECK_STR(ui_test_typed(), ""); /* the field cleared */

	/* Back: the conversation is now the most recent, its last message the preview */
	CHECK(ui_test_back());
	lv_obj_t *sent = ui_test_find("See you at 8"), *ben = ui_test_find("+1 415 555 0132");
	CHECK(sent && ben);
	if (sent && ben) {
		lv_area_t a, b;
		lv_obj_get_coords(sent, &a);
		lv_obj_get_coords(ben, &b);
		CHECK(a.y1 < b.y1);
	}
}

TEST(send_with_enter)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Ok\n"));
	CHECK(ui_test_shows("Ok"));
	CHECK(ui_test_shows_part("Not sent"));
}

TEST(blank_not_sent)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("   "));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(!ui_test_shows_part("Not sent"));
}

TEST(trimmed)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("  On my way  "));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_shows("On my way"));
}

TEST(call_from_a_thread)
{
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_text(LV_SYMBOL_CALL));
	CHECK(ui_test_shows("Calling..."));
	CHECK(ui_test_shows("+1 415 555 0132"));
}
