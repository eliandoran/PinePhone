/* Messages (messages.c, the conversations of data.c) through the UI (ui_test.h): kept
 * in /var/lib/messages, SMS sent and received through the modem (modem_at.c on
 * fake_modem.c) */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#include "modem/modem.h"
#include "ui_symbols.h"

#define CONTACTS "/var/lib/contacts/contacts.json"
#define MESSAGES "/var/lib/messages/messages.json"
#define MOM "+40744555210"

TEST_SETUP
{
	ui_test_reset();
}

/* A file's content (static), "" if it cannot be read */
static const char *file(const char *path)
{
	static char text[65536];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	if (f)
		fclose(f);
	text[n] = '\0';
	return text;
}

/* "HH:MM" of a time (UTC in the sandbox, as on the phone) */
static const char *hhmm(time_t t)
{
	static char text[8];
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(text, sizeof(text), "%H:%M", &tm);
	return text;
}

/* Two conversations: Ben's the most recent, earlier today */
static time_t stored(void)
{
	time_t now = time(NULL), t = now - now % 86400 + 9 * 3600 + 41 * 60; /* 09:41 today */
	if (t > now)
		t = now;
	char text[1024];
	snprintf(text, sizeof(text),
	         "[ { \"number\": \"+1 415 555 0132\", \"messages\": [\n"
	         "    { \"text\": \"Is the phone thing booting?\", \"time\": %lld },\n"
	         "    { \"text\": \"It boots\", \"time\": %lld, \"out\": true },\n"
	         "    { \"text\": \"Running 10 min late, grab us a table\", \"time\": %lld } ] },\n"
	         "  { \"number\": \"" MOM "\", \"messages\": [\n"
	         "    { \"text\": \"Patience is the hard part\", \"time\": %lld, \"out\": true } ] } ]\n",
	         (long long)t - 3 * 86400, (long long)t - 3 * 86400 + 60, (long long)t, (long long)t - 10 * 86400);
	ui_test_write(MESSAGES, text);
	return t;
}

static bool none_sending(void)
{
	return !ui_test_shows_part("Sending") && !ui_test_shows_part("Not sent");
}

/* Waits for the thread to show no message being sent, or not sent */
static bool sent_shown(void)
{
	return ui_test_wait_until(none_sending, 3000);
}

static bool none_stored(void)
{
	return !ui_test_modem_sms_stored();
}

/* Waits for the modem to store nothing (deleted, one at a time) */
static unsigned stored_after_deletes(void)
{
	ui_test_wait_until(none_stored, 3000);
	return ui_test_modem_sms_stored();
}

/* The modem was given a message to send */
static bool sms_sent(void)
{
	return ui_test_modem_sms_sent(0, NULL) != NULL;
}

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

static bool no_sim(void)
{
	return modem_state() == MODEM_NO_SIM;
}

/* The modem with a ready SIM, registered */
static void ready(void)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(registered, 10000));
}

TEST(empty)
{
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("No messages"));
}

TEST(conversations)
{
	time_t t = stored();
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("+1 415 555 0132"));
	CHECK(ui_test_shows("Running 10 min late, grab us a table"));
	CHECK(ui_test_shows(hhmm(t)));
	CHECK(ui_test_shows(MOM));
	CHECK(ui_test_shows("Patience is the hard part"));
	CHECK(!ui_test_shows("No messages"));
	/* The last message read as body text, its time smaller */
	CHECK(ui_test_font("Running 10 min late, grab us a table") == UI_FONT_BODY);
	CHECK(ui_test_font(hhmm(t)) == UI_FONT_CAPTION);
}

/* Names from the contacts, the number written either way */
TEST(names_from_the_contacts)
{
	stored();
	ui_test_write(CONTACTS, "[ { \"name\": \"Ben\", \"number\": \"+14155550132\" },"
	                        "  { \"name\": \"Mom\", \"number\": \"0744 555 210\" } ]");
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("Ben"));
	CHECK(ui_test_shows("Mom"));
	CHECK(!ui_test_shows("+1 415 555 0132"));
}

/* A new message from the round button at the bottom right, in reach of the thumb */
TEST(new_message_button)
{
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_action_button(LV_SYMBOL_EDIT));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows("Contacts"));
}

/* Scrolled to the end, the last conversation is not under the button */
TEST(new_message_button_clear_of_the_last)
{
	char text[8192];
	size_t n = snprintf(text, sizeof(text), "[");
	time_t now = time(NULL);
	for (int i = 0; i < 16; i++)
		n += snprintf(text + n, sizeof(text) - n,
		              "%s { \"number\": \"+1 415 555 01%02d\", \"messages\": ["
		              " { \"text\": \"Message %d\", \"time\": %lld } ] }",
		              i ? "," : "", i, i, (long long)now - 3600 * (i + 1));
	snprintf(text + n, sizeof(text) - n, " ]\n");
	ui_test_write(MESSAGES, text);
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_clear_of_action_button("Message 15", LV_SYMBOL_EDIT));
}

TEST(thread)
{
	time_t t = stored();
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_shows("+1 415 555 0132")); /* the title */
	CHECK(ui_test_shows("Is the phone thing booting?"));
	CHECK(ui_test_shows("It boots"));
	CHECK(ui_test_shows("Running 10 min late, grab us a table"));
	CHECK(!ui_test_textarea()); /* no keyboard until the field is tapped */
	CHECK(ui_test_font(hhmm(t)) == UI_FONT_CAPTION); /* the messages' time */
}

TEST(send)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("See you at 8"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_shows("See you at 8"));
	CHECK(!ui_test_textarea()); /* the keyboard hidden */

	/* Sent through the modem: its time instead of "Sending..." */
	const char *to = NULL;
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, &to), "See you at 8");
	CHECK_STR(to, MOM);
	CHECK(sent_shown());
	CHECK(ui_test_shows(hhmm(time(NULL))) || ui_test_shows(hhmm(time(NULL) - 60)));
	CHECK(strstr(file(MESSAGES), "See you at 8"));

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

/* The keyboard goes with its key, a tap on a message or on the list around them;
 * what was typed stays */
TEST(keyboard_hidden_in_a_thread)
{
	stored();
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("On my"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(!ui_test_textarea());

	CHECK(ui_test_tap_field("On my"));
	CHECK(ui_test_textarea());
	CHECK(ui_test_tap_text("It boots"));
	CHECK(!ui_test_textarea());

	CHECK(ui_test_tap_field("On my"));
	lv_obj_t *message = ui_test_find("Is the phone thing booting?");
	CHECK(message != NULL);
	if (message) {
		/* Right of the message, on the list itself */
		lv_area_t a;
		lv_obj_get_coords(message, &a);
		ui_test_tap_at(700, (a.y1 + a.y2) / 2);
	}
	CHECK(!ui_test_textarea());
	CHECK(ui_test_tap_field("On my"));
	CHECK_STR(ui_test_typed(), "On my");
}

TEST(send_with_enter)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Ok\n"));
	CHECK(ui_test_shows("Ok"));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), "Ok");
}

/* Long ones go in parts, others in UCS-2: the modem gets the text as typed */
TEST(send_long)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	char text[201];
	for (int i = 0; i < 200; i++)
		text[i] = "abcdefghij"[i % 10];
	text[200] = '\0';
	CHECK(ui_test_type(text));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), text);
}

/* The field grows with what is typed, wrapped, up to a few lines; then it scrolls.
 * Sent, it is one line again. */
TEST(field_wraps)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	lv_obj_t *ta = ui_test_textarea();
	CHECK(ta);
	if (!ta)
		return;
	int32_t one = lv_obj_get_height(ta);
	int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(ta, LV_PART_MAIN));

	CHECK(ui_test_type("Running late, the bus broke down near the bridge, "));
	CHECK(ui_test_type("I will be there in twenty minutes or so"));
	int32_t two = lv_obj_get_height(ta);
	CHECK(two > one + line / 2);
	/* Above the keyboard, the send button at its last line */
	lv_area_t field, send;
	lv_obj_get_coords(ta, &field);
	lv_obj_get_coords(ui_test_find(LV_SYMBOL_UP), &send);
	CHECK(send.y2 <= field.y2 && send.y2 >= field.y2 - line);
	lv_area_t kb;
	lv_obj_get_coords(lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(ta)), -1), &kb);
	CHECK(field.y2 <= kb.y1);

	for (int i = 0; i < 6; i++)
		CHECK(ui_test_type(" and then some more words to fill a line"));
	int32_t most = lv_obj_get_height(ta);
	CHECK(most > two);
	CHECK(most <= one + 4 * line);
	CHECK(ui_test_type(" still more"));
	CHECK_INT(lv_obj_get_height(ta), most);

	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_INT(lv_obj_get_height(ta), one);
}

TEST(send_without_sim)
{
	stored();
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	ui_test_wait_until(no_sim, 5000);
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_shows("Hello"));
	CHECK(ui_test_shows_part("Not sent: No SIM card"));
	CHECK(strstr(file(MESSAGES), "\"failed\""));
}

TEST(send_without_modem)
{
	stored();
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_shows_part("Not sent: No modem"));
}

/* Refused by the network: not sent, until tapped to try again */
TEST(send_refused_then_again)
{
	stored();
	ready();
	ui_test_modem_sms_fails(1);
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_wait_for("Not sent. Tap to try again", 3000));
	CHECK(!ui_test_modem_sms_sent(0, NULL));

	CHECK(ui_test_tap_text("Hello"));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), "Hello");
	CHECK(sent_shown());
}

/* phone-ui ended while one was being sent: not sent, said so */
TEST(sending_when_ended)
{
	ui_test_write(MESSAGES, "[ { \"number\": \"" MOM "\", \"messages\": [\n"
	                        "    { \"text\": \"Hello\", \"time\": 1762300000, \"out\": true, \"state\": \"sending\" } ] } ]");
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Hello"));
	CHECK(ui_test_shows_part("Not sent"));
}

TEST(blank_not_sent)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("   "));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	ui_test_wait(1000);
	CHECK(!ui_test_modem_sms_sent(0, NULL));
	CHECK(!ui_test_shows_part("Sending"));
}

TEST(trimmed)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("  On my way  "));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_shows("On my way"));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), "On my way");
}

/* A message received shown, in the list too: the banner shows it first, the list the next
 * time it looks (each 250 ms) */
static bool listed(const char *text)
{
	if (!ui_test_wait_for(text, 3000))
		return false;
	ui_test_wait(300);
	return true;
}

/* Received with the list open: on top, unread until its thread is opened; kept, and
 * deleted from the modem */
TEST(receive)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms(MOM, "Are you coming for lunch?");
	CHECK(listed("Are you coming for lunch?"));
	CHECK(ui_test_shows("1")); /* unread */
	CHECK(strstr(file(MESSAGES), "Are you coming for lunch?"));
	CHECK_INT(stored_after_deletes(), 0);

	lv_obj_t *mom = ui_test_find(MOM), *ben = ui_test_find("+1 415 555 0132");
	CHECK(mom && ben);
	if (mom && ben) {
		lv_area_t a, b;
		lv_obj_get_coords(mom, &a);
		lv_obj_get_coords(ben, &b);
		CHECK(a.y1 < b.y1);
	}

	CHECK(ui_test_tap_text("Are you coming for lunch?"));
	CHECK(ui_test_shows("Patience is the hard part"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("1"));
}

TEST(receive_in_the_thread)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	ui_test_modem_sms(MOM, "I'm making sarmale");
	CHECK(ui_test_wait_for("I'm making sarmale", 3000));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("1")); /* read there */
}

/* From someone new, in UCS-2 */
TEST(receive_new)
{
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("No messages"));
	ui_test_modem_sms("+33612345678", "Ça va? \xf0\x9f\x98\x80");
	CHECK(listed("Ça va? \xf0\x9f\x98\x80"));
	CHECK(ui_test_shows("+33612345678"));
	CHECK(!ui_test_shows("No messages"));
}

/* Stored on the modem before phone-ui started (or while it was not running) */
TEST(receive_stored_before)
{
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_modem_sms(MOM, "Call your grandmother");
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_wait_for("Call your grandmother", 3000));
	CHECK_INT(stored_after_deletes(), 0);
}

/* A long one in parts, in any order: shown whole once all are there, the parts kept on
 * the modem until then */
TEST(receive_parts)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms_part(MOM, "sarmale, ", 42, 3, 2);
	ui_test_modem_sms_part(MOM, "and cake.", 42, 3, 3);
	ui_test_wait(1500);
	CHECK(ui_test_shows("No messages"));
	CHECK_INT(ui_test_modem_sms_stored(), 2);
	ui_test_modem_sms_part(MOM, "I made ", 42, 3, 1);
	CHECK(ui_test_wait_for("I made sarmale, and cake.", 3000));
	CHECK_INT(stored_after_deletes(), 0);
}

/* From a name (alphanumeric sender) */
TEST(receive_from_a_name)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms("Vodafone", "Your bill is ready");
	CHECK(ui_test_wait_for("Your bill is ready", 3000));
	CHECK(ui_test_shows("Vodafone"));
	ui_test_modem_sms("Orange", "Welcome");
	CHECK(ui_test_wait_for("Welcome", 3000));
	CHECK(ui_test_shows("Orange")); /* another conversation */
	CHECK(ui_test_wait_for("Your bill is ready", 1000)); /* the list: the banner shows the last one */
}

TEST(call_from_a_thread)
{
	stored();
	ui_test_start();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Running 10 min late, grab us a table"));
	CHECK(ui_test_tap_text(UI_SYMBOL_PHONE));
	CHECK(ui_test_shows("No modem")); /* none in this test */
	CHECK(ui_test_shows("+1 415 555 0132"));
}

/* SMS not ready yet as the SIM is (busy): set up a little later, what it stores listed */
TEST(receive_once_sms_is_ready)
{
	ui_test_modem(UI_TEST_SIM_READY);
	ui_test_modem_sms_busy(1);
	ui_test_modem_sms(MOM, "Call your grandmother");
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("No messages"));
	CHECK(ui_test_wait_for("Call your grandmother", 8000));
}

/* The modem gone while one is being sent: not sent */
TEST(modem_gone_while_sending)
{
	stored();
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	ui_test_modem_silent(true);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_shows("Sending..."));
	ui_test_modem_unplug();
	CHECK(ui_test_wait_for("Not sent. Tap to try again", 3000));
}

/* Several from the same sender listed together: each kept */
TEST(receive_several)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms(MOM, "Are you coming for lunch?");
	ui_test_modem_sms(MOM, "Bring the phone");
	CHECK(listed("Bring the phone"));
	CHECK(ui_test_shows("2")); /* unread */
	CHECK_INT(stored_after_deletes(), 0);
	CHECK(ui_test_tap_text("Bring the phone"));
	CHECK(ui_test_shows("Are you coming for lunch?"));
}

/* One arriving while the store is listed for another (+CMTI before the list's OK): listed
 * again after, neither lost */
TEST(receive_while_listing)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms_while_listing(MOM, "Bring the phone");
	ui_test_modem_sms(MOM, "Are you coming for lunch?");
	CHECK(listed("Bring the phone"));
	CHECK_INT(stored_after_deletes(), 0);
	CHECK(ui_test_tap_text("Bring the phone"));
	CHECK(ui_test_shows("Are you coming for lunch?"));
}

/* Data, not text (a WAP push): deleted from the modem, nothing shown; texts still come */
TEST(receive_data_not_shown)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms_data(MOM);
	ui_test_modem_sms(MOM, "Are you coming for lunch?");
	CHECK(listed("Are you coming for lunch?"));
	CHECK_INT(stored_after_deletes(), 0);
	CHECK(!ui_test_shows("2")); /* one unread */
	CHECK(ui_test_tap_text("Are you coming for lunch?"));
	CHECK(!ui_test_shows("x"));
}

/* A reply to a name ("Vodafone" sent it): no number to send to, not sent */
TEST(reply_to_a_name_not_sent)
{
	ready();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms("Vodafone", "Your bill is ready");
	CHECK(ui_test_wait_for("Your bill is ready", 3000));
	ui_test_wait(6000); /* the banner gone */
	CHECK(ui_test_tap_text("Your bill is ready"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Thanks\n"));
	CHECK(ui_test_wait_for("Thanks", 1000));
	CHECK(ui_test_shows_part("Not sent"));
	ui_test_wait(1000);
	CHECK(!ui_test_modem_sms_sent(0, NULL));
}

/* The board's modem.conf (ui_test_reset()'s) with a line more, before ui_test_start() */
static void conf_add(const char *line)
{
	FILE *f = fopen("/tmp/modem.conf", "a");
	fprintf(f, "%s\n", line);
	fclose(f);
}

/* Waits for the modem's SMS domain to be `domain` after `restarts` restarts */
static bool sms_domain_becomes(unsigned domain, unsigned restarts, uint32_t ms)
{
	unsigned now_restarts;
	for (uint32_t t = 0; t <= ms; t += 100) {
		if (ui_test_modem_sms_domain_now(&now_restarts) == domain && now_restarts == restarts)
			return true;
		ui_test_wait(100);
	}
	fprintf(stderr, "  SMS domain %u after %u restarts, expected %u after %u\n",
	        ui_test_modem_sms_domain_now(&now_restarts), now_restarts, domain, restarts);
	return false;
}

/* A network that refuses SMS over IMS (Digi's) with the modem preferring it: on LTE,
 * not sent. Without sms_domain= in modem.conf the modem is left as it is */
TEST(sms_over_ims_refused)
{
	stored();
	ui_test_modem_sms_domain(1, true, false);
	ready();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_wait_for("Not sent. Tap to try again", 3000));
	CHECK(sms_domain_becomes(1, 0, 0));
}

/* sms_domain=cs: the modem set to send them circuit switched (SGs on LTE), restarted
 * for it to take, once; then they go */
TEST(sms_domain_set)
{
	stored();
	conf_add("sms_domain=cs");
	ui_test_modem_sms_domain(1, true, false);
	ready();
	CHECK(sms_domain_becomes(0, 1, 5000));
	ui_test_wait_until(registered, 10000); /* registered again after the restart */
	CHECK_INT(ui_test_modem_restart_commands(), 0); /* nothing sent while it restarted */
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	ui_test_wait_until(sms_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), "Hello");
	ui_test_wait(3000);
	CHECK(sms_domain_becomes(0, 1, 0)); /* not restarted again */
}

/* Already so: neither written nor restarted */
TEST(sms_domain_already_set)
{
	conf_add("sms_domain=cs");
	ui_test_modem_sms_domain(0, true, false);
	ready();
	ui_test_wait(3000);
	CHECK(sms_domain_becomes(0, 0, 0));
}

/* sms_domain=ims sets it back */
TEST(sms_domain_ims)
{
	conf_add("sms_domain=ims");
	ui_test_modem_sms_domain(0, false, false);
	ready();
	CHECK(sms_domain_becomes(1, 1, 5000));
}

/* A write that does not take: one restart, not one after another */
TEST(sms_domain_not_taking)
{
	conf_add("sms_domain=cs");
	ui_test_modem_sms_domain(1, true, true);
	ready();
	CHECK(sms_domain_becomes(1, 1, 5000));
	ui_test_wait(8000);
	CHECK(sms_domain_becomes(1, 1, 0));
	CHECK(modem_network() == MODEM_NET_HOME); /* working all the same */
}

/* In Romanian: why it was not sent, said in the language (kept in English) */
TEST(romanian_send_without_modem)
{
	stored();
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	ui_test_start();
	CHECK(ui_test_open("Mesaje"));
	CHECK(ui_test_tap_text("Patience is the hard part"));
	CHECK(ui_test_tap_field("Mesaj"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_shows_part("Netrimis: Fără modem"));
	CHECK(strstr(file(MESSAGES), "\"No modem\""));
}
