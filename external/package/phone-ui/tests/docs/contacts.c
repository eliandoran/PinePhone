/* docs/apps/contacts.md's screenshots (included by test_docs.c) */

static void contacts_seed(void)
{
	ui_test_write("/var/lib/contacts/contacts.json",
	              "[ { \"name\": \"Maya Okafor\", \"number\": \"+1 415 555 0142\", \"favorite\": true },\n"
	              "  { \"name\": \"Daniel Reyes\", \"number\": \"+44 7700 900456\", \"favorite\": true },\n"
	              "  { \"name\": \"Amara Nwosu\", \"number\": \"+1 415 555 0123\" },\n"
	              "  { \"name\": \"Lena Fischer\", \"number\": \"+1 415 555 0187\" },\n"
	              "  { \"name\": \"Grace Liu\", \"number\": \"+1 415 555 0110\" },\n"
	              "  { \"name\": \"Marco Bellini\", \"number\": \"+44 7700 900321\" },\n"
	              "  { \"name\": \"Priya Shah\", \"number\": \"+1 415 555 0155\" },\n"
	              "  { \"name\": \"Tom Becker\", \"number\": \"+1 415 555 0168\" },\n"
	              "  { \"name\": \"24h Locksmith\", \"number\": \"+1 415 555 0100\" } ]\n");
}

/* contacts.md: the list, favorites first, then by letter */
TEST(contacts_list)
{
	contacts_seed();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_shows(UI_SYMBOL_STAR "  Favorites"));
	CHECK(ui_test_shows("A"));
	CHECK(ui_test_shows("Amara Nwosu"));
	CHECK(ui_test_shows("M"));
	CHECK(ui_test_shows("Marco Bellini"));
	CHECK(ui_test_shows("#"));
	CHECK(ui_test_shows("24h Locksmith"));
	CHECK(ui_test_shows("9 contacts"));
	CHECK(ui_test_shows(LV_SYMBOL_PLUS));

	/* A favorite is listed twice: in Favorites and under its letter */
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("Maya Okafor", found, 4), 2);

	/* "#" after Z */
	lv_obj_t *tom = ui_test_find("Tom Becker"), *locksmith = ui_test_find("24h Locksmith");
	CHECK(tom && locksmith);
	if (tom && locksmith) {
		lv_area_t a, b;
		lv_obj_get_coords(tom, &a);
		lv_obj_get_coords(locksmith, &b);
		CHECK(a.y1 < b.y1);
	}
	shot("contacts-list");
}

/* contacts.md: a contact, with Call, Message and Favorite */
TEST(contacts_detail)
{
	contacts_seed();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Priya Shah"));
	CHECK(ui_test_shows("PS")); /* initials */
	CHECK(ui_test_shows("Call"));
	CHECK(ui_test_shows("Message"));
	CHECK(ui_test_shows("Favorite"));
	CHECK(ui_test_shows("mobile"));
	CHECK(ui_test_shows("+1 415 555 0155"));
	CHECK(ui_test_shows(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows(LV_SYMBOL_TRASH));
	shot("contacts-detail");

	/* The star: in Favorites */
	CHECK(ui_test_tap_text(UI_SYMBOL_STAR_REGULAR));
	CHECK(ui_test_shows(UI_SYMBOL_STAR));
	CHECK(ui_test_back());
	lv_obj_t *found[4];
	CHECK_INT(ui_test_find_all("Priya Shah", found, 4), 2);

	/* The number: in the dialer, not called */
	CHECK(ui_test_tap_text("Priya Shah"));
	CHECK(ui_test_tap_text("+1 415 555 0155"));
	CHECK(ui_test_shows("+14155550155"));
	CHECK(!ui_test_shows("Calling..."));
}

/* contacts.md: a contact shared as a QR code */
TEST(contacts_share)
{
	contacts_seed();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Priya Shah"));
	CHECK(ui_test_tap_text(UI_SYMBOL_QRCODE));
	ui_test_settle();
	CHECK(ui_test_shows("+1 415 555 0155"));
	shot("contacts-share");
}

/* contacts.md: adding someone */
TEST(contacts_new)
{
	contacts_seed();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("New contact"));
	CHECK(ui_test_shows("Name"));
	CHECK(ui_test_shows("Mobile"));
	CHECK(ui_test_textarea() != NULL); /* the keyboard at once */
	CHECK(ui_test_type("Sofia Marin\n")); /* Enter: on to the number */
	CHECK(ui_test_type("+1 415 555 0163"));
	CHECK_STR(ui_test_typed(), "+1 415 555 0163");
	shot("contacts-new");

	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_shows("Sofia Marin"));
	CHECK(ui_test_shows("10 contacts"));
}

/* contacts.md: deleting someone, after a confirmation */
TEST(contacts_delete)
{
	contacts_seed();
	CHECK(ui_test_open("Contacts"));
	CHECK(ui_test_tap_text("Tom Becker"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete contact"));
	CHECK(!ui_test_shows("Tom Becker"));
	CHECK(ui_test_shows("8 contacts"));
}
