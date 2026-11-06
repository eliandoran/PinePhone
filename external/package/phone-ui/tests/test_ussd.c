/* USSD (ussd.c, modem_at.c): codes dialed in Phone, the network's replies and menus,
 * through the UI (ui_test.h) and the fake modem */

#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#include "modem.h"
#include "ui_symbols.h"

#define CALLS "/var/lib/phone/calls.jsonl"

static bool registered(void)
{
	return modem_network() == MODEM_NET_HOME;
}

/* The modem with a ready SIM, registered */
static void ready(void)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_READY);
	CHECK(ui_test_wait_until(registered, 10000));
}

/* Phone's keypad: the code typed, then Call */
static bool cancelled(void)
{
	return ui_test_modem_ussd_cancels() > 0;
}

static bool dial(const char *keys)
{
	if (!ui_test_open("Phone"))
		return false;
	for (; *keys; keys++) {
		char key[2] = { *keys, '\0' };
		if (!ui_test_tap_text(key))
			return false;
	}
	return ui_test_tap_text(UI_SYMBOL_PHONE);
}

/* The reply field, shown while the network wants an answer */
static bool reply_field_shown(void)
{
	lv_obj_t *ta = NULL;
	lv_obj_t *found[8];
	unsigned n = ui_test_find_widgets(&lv_textarea_class, found, 8);
	for (unsigned i = 0; i < n; i++)
		if (lv_obj_get_screen(found[i]) == lv_screen_active())
			ta = found[i];
	return ta && !lv_obj_is_hidden(ta) && !lv_obj_is_hidden(lv_obj_get_parent(ta));
}

static bool no_reply_field(void)
{
	return !reply_field_shown();
}

TEST(code_answered)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5.00 EUR");
	CHECK(dial("*100#"));
	CHECK(ui_test_shows("*100#"));
	CHECK(ui_test_shows("Sending..."));
	CHECK(!ui_test_shows("Calling..."));
	CHECK(ui_test_wait_for("Credit: 5.00 EUR", 5000));
	CHECK(!ui_test_shows("Sending..."));
	CHECK(no_reply_field()); /* the session ended */
	CHECK_STR(ui_test_modem_ussd_sent(0), "*100#");
	/* Nothing else sent while the reply was awaited: a late error would be taken as
	 * another command's */
	CHECK_INT(ui_test_modem_ussd_overlaps(), 0);
	/* Not a call: not in the call log */
	CHECK(access(CALLS, F_OK) != 0);
}

TEST(menu_answered)
{
	ready();
	ui_test_modem_ussd("*111#", UI_TEST_USSD_MORE, "Id client: 16930678\n1 Contul meu\n2 Oferte\n3 Extraoptiuni");
	ui_test_modem_ussd("1", UI_TEST_USSD_MORE, "1 Oferta Actuala\n2 Upgrade");
	CHECK(dial("*111#"));
	CHECK(ui_test_wait_for("Id client: 16930678\n1 Contul meu\n2 Oferte\n3 Extraoptiuni", 5000));
	CHECK(reply_field_shown());

	CHECK(ui_test_tap_field("Reply"));
	CHECK(ui_test_type("1"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("1 Oferta Actuala\n2 Upgrade", 5000));
	CHECK_STR(ui_test_modem_ussd_sent(1), "1");
	CHECK(ui_test_shows("Id client: 16930678\n1 Contul meu\n2 Oferte\n3 Extraoptiuni")); /* still above */
	CHECK_INT(ui_test_modem_ussd_overlaps(), 0);
}

TEST(back_cancels_an_open_session)
{
	ready();
	ui_test_modem_ussd("*111#", UI_TEST_USSD_MORE, "1 Contul meu");
	CHECK(dial("*111#"));
	CHECK(ui_test_wait_for("1 Contul meu", 5000));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Keypad"));
	ui_test_wait_until(cancelled, 5000);
	CHECK_INT(ui_test_modem_ussd_cancels(), 1);
}

TEST(back_after_the_end_cancels_nothing)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5.00 EUR");
	CHECK(dial("*100#"));
	CHECK(ui_test_wait_for("Credit: 5.00 EUR", 5000));
	CHECK(ui_test_back());
	ui_test_wait(1000);
	CHECK_INT(ui_test_modem_ussd_cancels(), 0);
}

TEST(back_while_sending_cancels)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5.00 EUR");
	CHECK(dial("*100#"));
	CHECK(ui_test_back());
	ui_test_wait_until(cancelled, 5000);
	CHECK_INT(ui_test_modem_ussd_cancels(), 1);
	/* The reply of the cancelled session, had it come, opens nothing */
	ui_test_wait(2000);
	CHECK(!ui_test_shows("Credit: 5.00 EUR"));
}

TEST(refused_code)
{
	ready();
	CHECK(dial("*123#")); /* nothing scripted: +CME ERROR after the OK */
	CHECK(ui_test_wait_for("Refused by the network", 5000));
	CHECK(no_reply_field());
	CHECK_INT(ui_test_modem_ussd_overlaps(), 0);
	/* The modem goes on as before: the signal still asked for, nothing else refused */
	CHECK(registered());
}

TEST(not_supported)
{
	ready();
	ui_test_modem_ussd("*999#", UI_TEST_USSD_UNSUPPORTED, NULL);
	CHECK(dial("*999#"));
	CHECK(ui_test_wait_for("Not supported by the network", 5000));
}

TEST(ended_by_the_network)
{
	ready();
	ui_test_modem_ussd("*111#", UI_TEST_USSD_MORE, "1 Contul meu");
	ui_test_modem_ussd("1", UI_TEST_USSD_ENDED, NULL);
	CHECK(dial("*111#"));
	CHECK(ui_test_wait_for("1 Contul meu", 5000));
	CHECK(ui_test_tap_field("Reply"));
	CHECK(ui_test_type("1\n")); /* Enter sends too */
	CHECK(ui_test_wait_for("Ended by the network", 5000));
	CHECK(no_reply_field());
}

TEST(no_answer)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_SILENT, NULL);
	CHECK(dial("*100#"));
	ui_test_wait(20000);
	CHECK(ui_test_shows("Sending..."));
	CHECK(ui_test_wait_for("No answer from the network", 15000));
	/* The modem is used again meanwhile: still registered, its signal asked */
	CHECK(registered());
	CHECK_INT(ui_test_modem_ussd_cancels(), 1); /* the session given up */
}

TEST(network_timed_out)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_TIMEOUT, NULL);
	CHECK(dial("*100#"));
	CHECK(ui_test_wait_for("No answer from the network", 5000));
}

static bool no_sim(void)
{
	return modem_state() == MODEM_NO_SIM;
}

static bool imei_known(void)
{
	return modem_imei()[0];
}

TEST(without_sim)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_until(no_sim, 5000));
	CHECK(dial("*100#"));
	CHECK(ui_test_shows("No SIM card"));
	ui_test_wait(2000);
	CHECK(!ui_test_modem_ussd_sent(0));
}

TEST(without_modem)
{
	ui_test_start();
	CHECK(dial("*100#"));
	CHECK(ui_test_shows("No modem"));
}

TEST(modem_gone)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_SILENT, NULL);
	CHECK(dial("*100#"));
	ui_test_wait(500);
	ui_test_modem_unplug();
	CHECK(ui_test_wait_for("Not sent", 3000));
}

TEST(other_characters)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5,00 lei \xc8\x99i 100 \xe2\x82\xac");
	CHECK(dial("*100#"));
	CHECK(ui_test_wait_for("Credit: 5,00 lei \xc8\x99i 100 \xe2\x82\xac", 5000));
}

TEST(character_set_back_after)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5.00 EUR");
	CHECK(dial("*100#"));
	CHECK(ui_test_wait_for("Credit: 5.00 EUR", 5000));
	ui_test_wait(1000);
	CHECK(!ui_test_modem_ucs2());
	/* The operator's name asked again with the signal: plain text, not hexadecimal */
	ui_test_wait(11000);
	CHECK_STR(modem_operator(), "Fake Mobile");
}

TEST(network_sends_unasked)
{
	ready();
	ui_test_modem_ussd("1", UI_TEST_USSD_DONE, "Thank you");
	ui_test_modem_ussd_push(UI_TEST_USSD_MORE, "Top-up of 10 EUR done\n1 OK\n2 Details");
	CHECK(ui_test_wait_for("Top-up of 10 EUR done\n1 OK\n2 Details", 3000)); /* lines joined */
	CHECK(ui_test_shows("USSD"));
	CHECK(reply_field_shown());
	CHECK(ui_test_tap_field("Reply"));
	CHECK(ui_test_type("1"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("Thank you", 5000));
}

TEST(network_notice_while_open)
{
	ready();
	ui_test_modem_ussd("*100#", UI_TEST_USSD_DONE, "Credit: 5.00 EUR");
	CHECK(dial("*100#"));
	CHECK(ui_test_wait_for("Credit: 5.00 EUR", 5000));
	ui_test_modem_ussd_push(UI_TEST_USSD_DONE, "Bonus added");
	CHECK(ui_test_wait_for("Bonus added", 3000));
	CHECK(ui_test_shows("Credit: 5.00 EUR")); /* in the same screen */
}

TEST(imei_without_the_network)
{
	ui_test_start();
	ui_test_modem(UI_TEST_SIM_ABSENT);
	CHECK(ui_test_wait_until(imei_known, 5000));
	CHECK(dial("*#06#"));
	CHECK(ui_test_shows("IMEI\n" UI_TEST_IMEI));
	CHECK(no_reply_field());
	ui_test_wait(2000);
	CHECK(!ui_test_modem_ussd_sent(0));
}

TEST(imei_without_modem)
{
	ui_test_start();
	CHECK(dial("*#06#"));
	CHECK(ui_test_shows("No modem"));
}

TEST(a_number_is_still_a_call)
{
	ready();
	CHECK(dial("*31#0721"));
	CHECK(ui_test_shows("Calling..."));
}

/* In Romanian */
TEST(romanian_without_modem)
{
	ui_test_reset();
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	ui_test_start();
	CHECK(ui_test_open("Telefon"));
	for (const char *k = "*100#"; *k; k++) {
		char key[2] = { *k, '\0' };
		CHECK(ui_test_tap_text(key));
	}
	CHECK(ui_test_tap_text(UI_SYMBOL_PHONE));
	CHECK(ui_test_shows("Fără modem"));
}
