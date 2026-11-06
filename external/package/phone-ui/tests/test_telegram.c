/*
 * Telegram (apps/telegram), the plugin app: telegram.so built for the host (TELEGRAM_APP,
 * the Makefile's), installed in /var/lib/apps/telegram with the tests' libtdjson.so in its
 * lib/ and loaded by phone-ui (external.c) as on the phone. TDLib is fake_tdjson.c: signing
 * in, the bar switching between the chats and the profile, the profile and editing it.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "fake_tdjson.h"
#include "ui.h"
#include "ui_test.h"

#define APP "/var/lib/apps/telegram"
#define PHOTOS "/var/lib/photos"

static bool copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
	char buf[65536];
	size_t n;
	bool ok = in && out;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if (in)
		fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	return ok;
}

/* As `just app telegram` installs it, with the API credentials in place */
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/lib/libtdjson.so", "") || !copy(TELEGRAM_APP "/app.json", APP "/app.json") ||
	    !copy(TELEGRAM_APP "/telegram.so", APP "/telegram.so") ||
	    !copy(TELEGRAM_APP "/lib/libtdjson.so", APP "/lib/libtdjson.so"))
		TEST_FAIL("cannot install %s", TELEGRAM_APP);
	ui_test_write("/var/lib/telegram/api", "12345 0123456789abcdef0123456789abcdef\n");
	/* Loaded first: ASan's dlopen() is the caller of the real one, without telegram.so's
	 * runpath ($ORIGIN/lib), and by its soname telegram.so's dlopen() finds it loaded */
	if (!dlopen(APP "/lib/libtdjson.so", RTLD_NOW | RTLD_GLOBAL))
		TEST_FAIL("%s", dlerror());
}

static void account(void)
{
	ui_test_td_account("Elian", "Doran", "elian", "40712345678");
	ui_test_td_bio("Feature phones are back");
	ui_test_td_birthdate(14, 3, 1990);
}

/* Telegram opened, signed in, on its profile */
static bool open_profile(void)
{
	return ui_test_open("Telegram") && ui_test_wait_for("Chats", 1000) && ui_test_tap_text("Profile") &&
	       ui_test_wait_for("Feature phones are back", 1000);
}

/* The field with that text or placeholder tapped, emptied */
static bool clear_field(const char *text)
{
	if (!ui_test_tap_field(text) || !ui_test_textarea())
		return false;
	lv_textarea_set_text(ui_test_textarea(), "");
	return true;
}

/* The last request of that type contains `part` */
static bool requested(const char *type, const char *part)
{
	const char *r = ui_test_td_requested(type);
	if (!r || !strstr(r, part)) {
		printf("  %s: %s\n", type, r ? r : "not requested");
		return false;
	}
	return true;
}

/* A JPEG in the Gallery, w x h in one color */
static void photo_write(const char *path, int w, int h)
{
	mkdir(PHOTOS, 0755);
	uint8_t *px = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++)
		memcpy(px + i * 4, "\xc0\x80\x40\xff", 4);
	FILE *f = fopen(path, "wb");
	ui_jpeg_write(f, px, w, h, w * 4, 90, false);
	fclose(f);
	free(px);
}

/* Photos drawn on the screen (avatars with one, thumbnails) */
static unsigned photos_shown(void)
{
	ui_test_wait(200);
	return ui_test_find_widgets(&lv_3dtexture_class, NULL, 0);
}

/* Signed out, the bar is not there; it shows once signed in, on the chats */
TEST(bar_once_signed_in)
{
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(!ui_test_shows("Profile"));
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_type("40712345678\n"));
	CHECK(ui_test_wait_for("Enter the code", 1000));
	CHECK(!ui_test_shows("Profile"));
	CHECK(ui_test_tap_field("Code"));
	CHECK(ui_test_type(FAKE_TD_CODE "\n"));
	CHECK(ui_test_wait_for("No chats", 1000));
	CHECK(ui_test_shows("Chats"));
	CHECK(ui_test_shows("Profile"));
	CHECK(!ui_test_shows("Enter the code"));
}

/* The profile: photo or initials, name, phone number, username, bio */
TEST(profile_shown)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_shows("Elian Doran"));
	CHECK(ui_test_shows("+40712345678"));
	CHECK(ui_test_shows("@elian"));
	CHECK(ui_test_shows("Feature phones are back"));
	CHECK(ui_test_shows("ED"));
	CHECK(!ui_test_shows("No chats"));
	CHECK(requested("getUserFullInfo", "\"user_id\":777000111"));
	/* And back to the chats */
	CHECK(ui_test_tap_text("Chats"));
	CHECK(ui_test_shows("No chats"));
	CHECK(!ui_test_shows("Feature phones are back"));
}

/* Without a username or bio, they say so */
TEST(profile_without_username_or_bio)
{
	ui_test_td_account("Elian", "", "", "40712345678");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Chats", 1000));
	CHECK(ui_test_tap_text("Profile"));
	CHECK(ui_test_wait_for("Elian", 1000));
	CHECK_INT(ui_test_find_all("None", NULL, 0), 3);
	CHECK(!ui_test_shows_part("@"));
}

/* Each time the app opens, it starts on the chats */
TEST(opens_on_the_chats)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_back());
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("No chats", 1000));
	CHECK(!ui_test_shows("Feature phones are back"));
}

/* Name: first and last, saved with the header's button */
TEST(edit_name)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Name"));
	CHECK(ui_test_shows("First name"));
	CHECK(ui_test_tap_field("Elian"));
	CHECK_STR(ui_test_typed(), "Elian");
	CHECK(clear_field("Elian"));
	CHECK(ui_test_type("Eli"));
	CHECK(clear_field("Doran"));
	CHECK(ui_test_type("D."));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("Eli D.", 1000));
	CHECK(requested("setName", "\"first_name\":\"Eli\""));
	CHECK(requested("setName", "\"last_name\":\"D.\""));
	/* Back on the profile */
	CHECK(!ui_test_shows("First name"));
	CHECK(ui_test_shows("Feature phones are back"));
}

/* Refused: the screen stays, saying why */
TEST(edit_name_refused)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Name"));
	CHECK(clear_field("Elian"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("The first name cannot be empty.", 1000));
	CHECK(ui_test_shows("First name"));
	CHECK_INT(ui_test_td_request_count("setName"), 1);
	/* Left: the name as it was */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Elian Doran"));
}

/* Nothing changed, nothing sent; back leaves without saving */
TEST(edit_unchanged_or_left)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Name"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(!ui_test_shows("First name"));
	CHECK(ui_test_tap_text("Name"));
	CHECK(ui_test_tap_field("Elian"));
	CHECK(ui_test_type("x"));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("First name"));
	CHECK_INT(ui_test_td_request_count("setName"), 0);
	CHECK(ui_test_shows("Elian Doran"));
}

/* A username taken: said so, then another one is saved */
TEST(edit_username)
{
	account();
	ui_test_td_username_taken("elian_d");
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Username"));
	CHECK(ui_test_tap_field("elian"));
	if (!ui_test_textarea())
		return;
	lv_textarea_set_cursor_pos(ui_test_textarea(), LV_TEXTAREA_CURSOR_LAST);
	CHECK(ui_test_type("_d\n"));
	CHECK(ui_test_wait_for("This username is taken.", 1000));
	CHECK(ui_test_type("oran\n"));
	CHECK(ui_test_wait_for("@elian_doran", 1000));
	CHECK(requested("setUsername", "\"username\":\"elian_doran\""));
	CHECK_INT(ui_test_td_request_count("setUsername"), 2);
	CHECK(!ui_test_shows("This username is taken."));
}

/* Removed: none */
TEST(remove_username)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("@elian"));
	CHECK(clear_field("elian"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("None", 1000));
	CHECK(requested("setUsername", "\"username\":\"\""));
	CHECK(!ui_test_shows("@elian"));
}

TEST(edit_bio)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Bio"));
	CHECK(clear_field("Feature phones are back"));
	CHECK(ui_test_type("On a PinePhone \"really\""));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("On a PinePhone \"really\"", 1000));
	CHECK(requested("setBio", "\"bio\":\"On a PinePhone \\\"really\\\"\""));
	CHECK(!ui_test_shows("Feature phones are back"));
}

/* While the request is on its way, the button waits; an error that is not one of ours
 * shows as TDLib says it */
TEST(edit_waits_then_other_error)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Bio"));
	CHECK(clear_field("Feature phones are back"));
	CHECK(ui_test_type("Busy"));
	ui_test_td_fail("setBio", 429, "Too Many Requests: retry after 30");
	ui_test_td_hold(true);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	ui_test_wait(300);
	CHECK(ui_test_shows("Bio"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK)); /* again: not sent twice */
	ui_test_td_hold(false);
	CHECK(ui_test_wait_for("Too many attempts, try again later", 1000));
	CHECK_INT(ui_test_td_request_count("setBio"), 1);
}

/* The birthday: day, month, and the year if known */
TEST(birthday_shown)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_shows("14 March 1990"));
	CHECK(ui_test_shows("Birthday"));
}

TEST(birthday_without_year)
{
	account();
	ui_test_td_birthdate(29, 2, 0);
	CHECK(open_profile());
	CHECK(ui_test_shows("29 February"));
}

/* Day, month and year in fields of numbers; the year left empty: unknown */
TEST(edit_birthday)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("14 March 1990"));
	CHECK(ui_test_shows("Day"));
	CHECK(ui_test_shows("Month"));
	CHECK(ui_test_shows("Year"));
	CHECK(clear_field("14"));
	CHECK(ui_test_type("2\n")); /* Enter: the next field */
	CHECK_STR(ui_test_typed(), "3");
	CHECK(clear_field("1990"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("2 March", 1000));
	CHECK(requested("setBirthdate", "\"birthdate\":{\"@type\":\"birthdate\",\"day\":2,\"month\":3,\"year\":0}"));
	CHECK(!ui_test_shows("Day"));
}

/* A day the month does not have: said, nothing sent */
TEST(edit_birthday_invalid)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Birthday"));
	CHECK(clear_field("14"));
	CHECK(ui_test_type("31"));
	CHECK(clear_field("3"));
	CHECK(ui_test_type("4"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("This is not a date.", 1000));
	CHECK_INT(ui_test_td_request_count("setBirthdate"), 0);
	/* Not a number (the dial pad has signs too) */
	CHECK(clear_field("31"));
	CHECK(ui_test_type("#"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK_INT(ui_test_td_request_count("setBirthdate"), 0);
	CHECK(ui_test_shows("Day"));
}

/* All fields emptied: no birthday */
TEST(remove_birthday)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Birthday"));
	CHECK(clear_field("14"));
	CHECK(clear_field("3"));
	CHECK(clear_field("1990"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("None", 1000));
	CHECK(requested("setBirthdate", "\"birthdate\":null"));
	CHECK(!ui_test_shows("14 March 1990"));
}

/* The photo: initials until it is downloaded, then the photo */
TEST(profile_photo_shown)
{
	account();
	photo_write("/var/lib/telegram/me.jpg", 160, 160);
	ui_test_td_photo("/var/lib/telegram/me.jpg");
	CHECK(open_profile());
	CHECK_INT(photos_shown(), 1);
}

/* Tapping the avatar picks a photo from the Gallery, set as the profile's */
TEST(profile_photo_changed)
{
	account();
	photo_write(PHOTOS "/a.jpg", 320, 240);
	CHECK(open_profile());
	CHECK_INT(photos_shown(), 0);
	CHECK(ui_test_tap_text("ED"));
	CHECK(ui_test_wait_for("Profile photo", 1000));
	lv_obj_t *thumb;
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, &thumb, 1), 1);
	ui_test_tap(thumb);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	ui_test_wait(500);
	CHECK(requested("setProfilePhoto", "\"@type\":\"inputChatPhotoStatic\""));
	CHECK(requested("setProfilePhoto", "\"path\":\"" PHOTOS "/a.jpg\""));
	CHECK(ui_test_shows("Feature phones are back"));
	CHECK_INT(photos_shown(), 1);
}

/* A screenshot (PPM) is sent as a JPEG */
TEST(profile_photo_from_a_screenshot)
{
	account();
	mkdir(PHOTOS, 0755);
	FILE *f = fopen(PHOTOS "/shot.ppm", "w");
	fprintf(f, "P6\n64 64\n255\n");
	for (int i = 0; i < 64 * 64; i++)
		fwrite("\x40\x80\xc0", 1, 3, f);
	fclose(f);
	CHECK(open_profile());
	CHECK(ui_test_tap_text("ED"));
	CHECK(ui_test_wait_for("Profile photo", 1000));
	lv_obj_t *thumb;
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, &thumb, 1), 1);
	ui_test_tap(thumb);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	ui_test_wait(500);
	const char *r = ui_test_td_requested("setProfilePhoto");
	CHECK(r && strstr(r, ".jpg\""));
	CHECK(r && !strstr(r, "shot.ppm"));
	CHECK_INT(photos_shown(), 1);
}

/* Contacts */

#define NOW 1793966400 /* Friday 6 November 2026, 12:00 */
#define ONLINE "{\"@type\":\"userStatusOnline\",\"expires\":1793967000}"
#define OFFLINE_0930 "{\"@type\":\"userStatusOffline\",\"was_online\":1793957400}"
#define LAST_WEEK "{\"@type\":\"userStatusLastWeek\",\"by_my_privacy_settings\":false}"

static bool open_contacts(void)
{
	return ui_test_open("Telegram") && ui_test_wait_for("Chats", 1000) && ui_test_tap_text("Contacts");
}

static int32_t y_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	if (!label)
		return -1;
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	return a.y1;
}

/* By name, each with when it was last seen */
TEST(contacts_listed)
{
	ui_test_wall_clock(NOW);
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", OFFLINE_0930, NULL);
	ui_test_td_contact(102, "andrei", "", ONLINE, NULL);
	ui_test_td_contact(103, "Zoe", "Lambert", NULL, NULL);
	ui_test_td_contact(104, "Bogdan", "Pop", LAST_WEEK, NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_shows("last seen 09:30"));
	CHECK(ui_test_shows("andrei"));
	CHECK(ui_test_shows("online"));
	CHECK(ui_test_shows("Zoe Lambert"));
	CHECK(ui_test_shows("last seen recently"));
	CHECK(ui_test_shows("Bogdan Pop"));
	CHECK(ui_test_shows("last seen within a week"));
	CHECK(y_of("andrei") < y_of("Bogdan Pop"));
	CHECK(y_of("Bogdan Pop") < y_of("Mara Ionescu"));
	CHECK(y_of("Mara Ionescu") < y_of("Zoe Lambert"));
	CHECK(!ui_test_shows("No chats"));
	CHECK(!ui_test_shows("Feature phones are back"));
	CHECK_INT(ui_test_td_request_count("getContacts"), 1);
}

TEST(no_contacts)
{
	account();
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("No contacts", 1000));
}

TEST(contacts_loading)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Chats", 1000));
	ui_test_td_hold(true);
	CHECK(ui_test_tap_text("Contacts"));
	CHECK(ui_test_shows("Loading contacts..."));
	ui_test_td_hold(false);
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(!ui_test_shows("Loading contacts..."));
}

/* A status changing while shown */
TEST(contact_status_changes)
{
	ui_test_wall_clock(NOW);
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", OFFLINE_0930, NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("last seen 09:30", 1000));
	ui_test_td_status(101, ONLINE);
	CHECK(ui_test_wait_for("online", 1000));
	CHECK(!ui_test_shows("last seen 09:30"));
}

/* Their photos, downloaded for the list */
TEST(contact_photo)
{
	account();
	photo_write("/var/lib/telegram/mara.jpg", 160, 160);
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, "/var/lib/telegram/mara.jpg");
	ui_test_td_contact(102, "Zoe", "Lambert", NULL, NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	ui_test_wait(500);
	CHECK_INT(photos_shown(), 1);
	CHECK(ui_test_td_requested("downloadFile"));
}

/* Tapping one opens the chat with them, made if there was none */
TEST(contact_opens_chat)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_tap_text("Mara Ionescu"));
	CHECK(ui_test_wait_for("No messages", 1000));
	CHECK(requested("createPrivateChat", "\"user_id\":101"));
	CHECK(ui_test_shows("Mara Ionescu"));
	CHECK(ui_test_textarea() || ui_test_tap_field("Message"));
	/* Back: still on the contacts */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("last seen recently"));
}

/* Logging out on with contacts: none left, loaded again for the next account */
TEST(contacts_forgotten_on_log_out)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_tap_text("Profile"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_type("40712345678\n"));
	CHECK(ui_test_wait_for("Enter the code", 1000));
	CHECK(ui_test_tap_field("Code"));
	CHECK(ui_test_type(FAKE_TD_CODE "\n"));
	CHECK(ui_test_wait_for("Contacts", 1000));
	CHECK(ui_test_tap_text("Contacts"));
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK_INT(ui_test_td_request_count("getContacts"), 2);
}

/* Logging out asks first; then the login form, without the bar */
TEST(log_out)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_shows("Cancel"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows("Cancel"));
	CHECK_INT(ui_test_td_request_count("logOut"), 0);
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK_INT(ui_test_td_request_count("logOut"), 1);
	CHECK(!ui_test_shows("Profile"));
	CHECK(!ui_test_shows("Elian Doran"));
}
