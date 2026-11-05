/* docs/apps/messages.md's screenshots (included by test_docs.c) */

static void messages_contacts(void)
{
	ui_test_write("/var/lib/contacts/contacts.json",
	              "[ { \"name\": \"Maya Okafor\", \"number\": \"+1 415 555 0142\" },\n"
	              "  { \"name\": \"Daniel Reyes\", \"number\": \"+44 7700 900456\" },\n"
	              "  { \"name\": \"Lena Fischer\", \"number\": \"+1 415 555 0187\" } ]\n");
}

/* Four conversations, the most recent first: Maya's with two unread, one from a number not
 * in the contacts, Daniel's with a message not sent */
static void messages_stored(void)
{
	char text[4096];
	snprintf(text, sizeof(text),
	         "[ { \"number\": \"+14155550142\", \"unread\": 2, \"messages\": [\n"
	         "    { \"text\": \"Are we still on for the market tomorrow?\", \"time\": %lld },\n"
	         "    { \"text\": \"Yes! 10 o'clock at the fountain?\", \"time\": %lld, \"out\": true },\n"
	         "    { \"text\": \"Perfect, I'll bring the big bags\", \"time\": %lld },\n"
	         "    { \"text\": \"Running a few minutes late\", \"time\": %lld },\n"
	         "    { \"text\": \"Save me a coffee \xe2\x98\x95\", \"time\": %lld } ] },\n"
	         "  { \"number\": \"+1 202 555 0178\", \"messages\": [\n"
	         "    { \"text\": \"Your parcel arrives today between 12:00 and 14:00.\", \"time\": %lld } ] },\n"
	         "  { \"number\": \"+447700900456\", \"messages\": [\n"
	         "    { \"text\": \"Did you get home all right?\", \"time\": %lld },\n"
	         "    { \"text\": \"Yes, thanks for dinner!\", \"time\": %lld, \"out\": true, "
	         "\"state\": \"failed\", \"error\": \"No service\" } ] },\n"
	         "  { \"number\": \"+1 415 555 0187\", \"messages\": [\n"
	         "    { \"text\": \"Happy birthday Lena!\", \"time\": %lld, \"out\": true },\n"
	         "    { \"text\": \"Thank you, see you on Sunday\", \"time\": %lld } ] } ]\n",
	         (long long)(NOW - 15 * 3600), (long long)(NOW - 15 * 3600 + 240),
	         (long long)(NOW - 14 * 3600), (long long)(NOW - 4 * 60), (long long)(NOW - 3 * 60),
	         (long long)(NOW - 86400 * 2 + 600),
	         (long long)(NOW - 3 * 86400 - 12 * 3600), (long long)(NOW - 3 * 86400 - 12 * 3600 + 300),
	         (long long)(NOW - 12 * 86400), (long long)(NOW - 12 * 86400 + 1800));
	ui_test_write("/var/lib/messages/messages.json", text);
}

/* messages.md: the conversations */
TEST(messages_list)
{
	messages_contacts();
	messages_stored();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_shows("Maya Okafor"));
	CHECK(ui_test_shows("Save me a coffee \xe2\x98\x95"));
	CHECK(ui_test_shows("09:38"));
	CHECK(ui_test_shows("2")); /* unread */
	CHECK(ui_test_shows("+1 202 555 0178")); /* not in the contacts */
	CHECK(ui_test_shows("Thu"));
	CHECK(ui_test_shows("Daniel Reyes"));
	CHECK(ui_test_shows("Lena Fischer"));
	CHECK(ui_test_shows("2 Mar"));
	CHECK(ui_test_shows(LV_SYMBOL_EDIT)); /* a new message */
	shot("messages-list");
}

/* messages.md: a conversation */
TEST(messages_thread)
{
	messages_contacts();
	messages_stored();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Save me a coffee \xe2\x98\x95"));
	CHECK(ui_test_shows("Are we still on for the market tomorrow?"));
	CHECK(ui_test_shows("Yes! 10 o'clock at the fountain?"));
	CHECK(ui_test_shows("Running a few minutes late"));
	CHECK(ui_test_shows("09:38"));
	CHECK(ui_test_shows_part("Yesterday"));
	CHECK(ui_test_find_widget(&lv_textarea_class) != NULL);
	CHECK(!ui_test_textarea()); /* no keyboard until the field is tapped */
	shot("messages-thread");

	/* Read: no unread count in the list any more */
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("2"));
}

/* messages.md: a message not sent, tapped to send it again */
static bool messages_sent(void)
{
	return ui_test_modem_sms_sent(0, NULL) != NULL;
}

static bool messages_none_not_sent(void)
{
	return !ui_test_shows_part("Not sent");
}

TEST(messages_not_sent)
{
	messages_contacts();
	messages_stored();
	CHECK(ui_test_open("Messages"));
	CHECK(ui_test_tap_text("Daniel Reyes"));
	CHECK(ui_test_shows("Yes, thanks for dinner!"));
	CHECK(ui_test_shows("Not sent: No service. Tap to try again"));
	shot("messages-not-sent");

	CHECK(ui_test_tap_text("Yes, thanks for dinner!"));
	ui_test_wait_until(messages_sent, 3000);
	CHECK_STR(ui_test_modem_sms_sent(0, NULL), "Yes, thanks for dinner!");
	ui_test_wait_until(messages_none_not_sent, 3000);
	CHECK(!ui_test_shows_part("Not sent"));
}

/* messages.md: a message received while the list is open */
TEST(messages_received)
{
	messages_contacts();
	messages_stored();
	CHECK(ui_test_open("Messages"));
	ui_test_modem_sms("+447700900456", "Glad to hear it");
	CHECK(ui_test_wait_for("Glad to hear it", 10000));
	CHECK(ui_test_shows("1"));
}
