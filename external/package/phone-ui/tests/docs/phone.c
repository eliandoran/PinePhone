/* docs/apps/phone.md's screenshots (included by test_docs.c) */

/* A few people in the contacts */
static void phone_contacts(void)
{
	ui_test_write("/var/lib/contacts/contacts.json",
	              "[ { \"name\": \"Maya Okafor\", \"number\": \"+1 415 555 0142\", \"favorite\": true },\n"
	              "  { \"name\": \"Daniel Reyes\", \"number\": \"+44 7700 900456\" },\n"
	              "  { \"name\": \"Lena Fischer\", \"number\": \"+1 415 555 0187\" } ]\n");
}

/* A line of the call log, `age` seconds before NOW */
static void phone_call_line(FILE *f, const char *number, const char *kind, long age, unsigned duration)
{
	fprintf(f, "{ \"number\": \"%s\", \"time\": %lld, \"kind\": \"%s\", \"duration\": %u }\n", number,
	        (long long)(NOW - age), kind, duration);
}

/* The last few days' calls, oldest first */
static void phone_calls(void)
{
	ui_test_write("/var/lib/phone/calls.jsonl", "");
	FILE *f = fopen("/var/lib/phone/calls.jsonl", "w");
	CHECK(f != NULL);
	if (!f)
		return;
	phone_call_line(f, "+44 7700 900456", "incoming", 3 * 86400 + 5 * 3600, 312); /* Wednesday */
	phone_call_line(f, "+1 202 555 0178", "outgoing", 86400 + 2 * 3600, 45);      /* yesterday */
	phone_call_line(f, "+14155550142", "missed", 89 * 60, 0);                     /* 08:12 */
	phone_call_line(f, "+1 415 555 0142", "missed", 81 * 60, 0);                  /* 08:20 */
	phone_call_line(f, "+1 415 555 0199", "incoming", 36 * 60, 95);               /* 09:05 */
	phone_call_line(f, "+1 415 555 0187", "outgoing", 11 * 60, 0);                /* 09:30 */
	fclose(f);
}

/* phone.md: Recents, the calls of the last days */
TEST(phone_recents)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_shows("Recents"));
	CHECK(ui_test_shows("Keypad"));
	CHECK(ui_test_shows("Lena Fischer"));
	CHECK(ui_test_shows("Outgoing, not connected"));
	CHECK(ui_test_shows("+1 415 555 0199"));
	CHECK(ui_test_shows("Incoming, 1:35"));
	CHECK(ui_test_shows("Maya Okafor (2)")); /* two missed calls in a row: one row */
	CHECK(ui_test_shows("Missed"));
	CHECK(ui_test_shows("Yesterday") || ui_test_shows("Fri"));
	CHECK(ui_test_shows("Daniel Reyes"));
	CHECK(ui_test_shows("Incoming, 5:12"));
	CHECK(ui_test_shows(LV_SYMBOL_TRASH)); /* clearing them */
	shot("phone-recents");
}

/* phone.md: the keypad, with the name of the contact whose number is typed */
TEST(phone_keypad)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("Keypad"));
	CHECK(ui_test_long_press_text("0")); /* + */
	for (const char *k = "14155550142"; *k; k++) {
		char key[2] = { *k, '\0' };
		CHECK(ui_test_tap_text(key));
	}
	CHECK(ui_test_shows("+14155550142"));
	CHECK(ui_test_shows("Maya Okafor"));
	CHECK(ui_test_shows(LV_SYMBOL_BACKSPACE));
	shot("phone-keypad");
}

/* phone.md: the call screen, called back from Recents and answered */
TEST(phone_call)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("Daniel Reyes"));
	CHECK(ui_test_shows("Calling..."));
	CHECK(ui_test_wait_for("Ringing...", 3000));
	CHECK(ui_test_wait_for("0:05", 10000)); /* answered after 2 s */
	CHECK(ui_test_shows("Daniel Reyes"));
	CHECK(ui_test_shows("+44 7700 900456"));
	CHECK(ui_test_shows("Mute"));
	CHECK(ui_test_shows("Keypad"));
	CHECK(ui_test_shows("Speaker"));
	shot("phone-call");

	CHECK(ui_test_tap_text(UI_SYMBOL_PHONE_135)); /* hang up */
	CHECK(ui_test_wait_for("Call ended", 3000));
	CHECK(ui_test_wait_for("Recents", 5000));
	CHECK(ui_test_shows("Outgoing, 0:0") || ui_test_shows_part("Outgoing, 0:"));
}

/* phone.md: the keypad during a call, a menu's choice pressed */
TEST(phone_call_keypad)
{
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("Daniel Reyes"));
	CHECK(ui_test_wait_for("0:03", 10000));
	lv_obj_t *label = ui_test_find("Keypad");
	CHECK(label);
	if (!label)
		return;
	ui_test_tap(lv_obj_get_child(lv_obj_get_parent(label), 0)); /* its round button */
	CHECK(ui_test_tap_text("1"));
	CHECK(ui_test_tap_text("2"));
	CHECK(ui_test_shows("12"));
	CHECK(ui_test_shows("Hide"));
	shot("phone-call-keypad");
}

/* phone.md: a call coming in */
TEST(phone_incoming)
{
	ui_test_modem_incoming("+14155550142");
	CHECK(ui_test_wait_for("Incoming call", 3000));
	CHECK(ui_test_shows("Maya Okafor"));
	CHECK(ui_test_shows("Decline"));
	CHECK(ui_test_shows("Answer"));
	shot("phone-incoming");
}

/* phone.md: a USSD code, its menu answered once */
TEST(phone_ussd)
{
	ui_test_modem_ussd("*123#", UI_TEST_USSD_MORE, "Fake Mobile\n1 Balance\n2 Data bundles\n3 Top-up");
	ui_test_modem_ussd("1", UI_TEST_USSD_MORE, "Credit: 12.40 EUR\nValid until 30.04.2026\n0 Back");
	CHECK(ui_test_open("Phone"));
	CHECK(ui_test_tap_text("Keypad"));
	for (const char *k = "*123#"; *k; k++) {
		char key[2] = { *k, '\0' };
		CHECK(ui_test_tap_text(key));
	}
	CHECK(ui_test_tap_text(UI_SYMBOL_PHONE));
	CHECK(ui_test_wait_for("Fake Mobile\n1 Balance\n2 Data bundles\n3 Top-up", 5000));
	CHECK(ui_test_tap_field("Reply"));
	CHECK(ui_test_type("1"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("Credit: 12.40 EUR\nValid until 30.04.2026\n0 Back", 5000));
	shot("phone-ussd");
}
