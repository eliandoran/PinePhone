/*
 * Telegram (apps/telegram), the plugin app: telegram.so built for the host (TELEGRAM_APP,
 * the Makefile's), installed in /var/lib/apps/telegram with the tests' libtdjson.so in its
 * lib/ and loaded by phone-ui (external.c) as on the phone. TDLib is fake_tdjson.c: signing
 * in, the bar switching between the chats and the profile, the profile and editing it,
 * notifications of messages while another app is open (TDLib started with phone-ui).
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "test.h"
#include "fake_tdjson.h"
#include "ui.h"
#include "ui_symbols.h"
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
	CHECK(requested("downloadFile", "\"file_id\":100"));
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
	ui_test_td_contact(105, "Ilinca", "", "{\"@type\":\"userStatusLastMonth\",\"by_my_privacy_settings\":false}",
			   NULL);
	ui_test_td_contact(106, "Radu", "", "{\"@type\":\"userStatusEmpty\"}", NULL);
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_shows("last seen 09:30"));
	CHECK(ui_test_shows("last seen within a month"));
	CHECK(ui_test_shows("last seen a long time ago"));
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

/* A contact with a chat already: it opens, none is made */
TEST(contact_with_a_chat)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":101,"
			  "\"type\":{\"@type\":\"chatTypePrivate\",\"user_id\":101},\"title\":\"Mara Ionescu\","
			  "\"photo\":null,\"positions\":[],\"unread_count\":0,\"last_message\":null}}");
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_tap_text("Mara Ionescu"));
	CHECK(ui_test_wait_for("No messages", 1000));
	CHECK_INT(ui_test_td_request_count("createPrivateChat"), 0);
	CHECK(requested("openChat", "\"chat_id\":101"));
}

/* Refused: nothing opens, the contacts stay */
TEST(contact_chat_refused)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	ui_test_td_fail("createPrivateChat", 400, "USER_ID_INVALID");
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK(ui_test_tap_text("Mara Ionescu"));
	ui_test_wait(500);
	CHECK(!ui_test_shows("No messages"));
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

/* Notifications: TDLib's (notification groups) posted to phone-ui's (ui_notify()), while
 * the phone is in use, whatever app is open */

#define TD_DB "/var/lib/telegram/td.binlog"
#define ANA 4242
#define FRIENDS 5151

/* Signed in earlier: TDLib's database is there, so it starts with phone-ui */
static void signed_in_before(void)
{
	account();
	ui_test_write(TD_DB, "");
}

/* A private chat with Ana, a group of friends with Ben in it */
static void chats(void)
{
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":4242,\"title\":\"Ana\","
	                  "\"type\":{\"@type\":\"chatTypePrivate\",\"user_id\":4242},\"positions\":[]}}");
	ui_test_td_update("{\"@type\":\"updateUser\",\"user\":{\"@type\":\"user\",\"id\":303,\"first_name\":\"Ben\","
	                  "\"last_name\":\"\",\"status\":{\"@type\":\"userStatusRecently\"}}}");
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":5151,\"title\":\"Friends\","
	                  "\"type\":{\"@type\":\"chatTypeBasicGroup\",\"basic_group_id\":51},\"positions\":[]}}");
}

/* TDLib's notification group `group` of chat `chat`, `total` messages, this one added:
 * notification `id`, from `sender`; silent: without sound (the sender asked, or muted) */
static void notified(int group, long long chat, long long sender, int id, const char *text, int total, bool silent)
{
	char buf[2048];
	long long now = time(NULL);
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateNotificationGroup\",\"notification_group_id\":%d,"
	         "\"type\":{\"@type\":\"notificationGroupTypeMessages\"},\"chat_id\":%lld,"
	         "\"notification_settings_chat_id\":%lld,\"notification_sound_id\":\"0\",\"total_count\":%d,"
	         "\"added_notifications\":[{\"@type\":\"notification\",\"id\":%d,\"date\":%lld,\"is_silent\":%s,"
	         "\"type\":{\"@type\":\"notificationTypeNewMessage\",\"show_preview\":true,\"message\":"
	         "{\"@type\":\"message\",\"id\":%d,\"chat_id\":%lld,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%lld},\"is_outgoing\":false,\"date\":%lld,\"content\":{\"@type\":\"messageText\","
	         "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}}}],"
	         "\"removed_notification_ids\":[]}",
	         group, chat, chat, total, id, now, silent ? "true" : "false", id * 1048576, chat, sender, now, text);
	ui_test_td_update(buf);
}

static void ana_writes(int id, const char *text, int total)
{
	notified(1, ANA, ANA, id, text, total, false);
}

/* Read (on another device too): TDLib removes them, the group empty */
static void ana_read(int first, int last)
{
	char ids[256] = "";
	size_t n = 0;
	for (int i = first; i <= last; i++)
		n += snprintf(ids + n, sizeof(ids) - n, "%s%d", i > first ? "," : "", i);
	char buf[768];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateNotificationGroup\",\"notification_group_id\":1,"
	         "\"type\":{\"@type\":\"notificationGroupTypeMessages\"},\"chat_id\":4242,"
	         "\"notification_settings_chat_id\":4242,\"notification_sound_id\":\"0\",\"total_count\":0,"
	         "\"added_notifications\":[],\"removed_notification_ids\":[%s]}", ids);
	ui_test_td_update(buf);
}

/* Started, TDLib signed in and its chats known, in Notes */
static bool in_notes(void)
{
	signed_in_before();
	ui_test_start();
	chats();
	return ui_test_open("Notes") && ui_test_wait_for("No notes", 1000);
}

/* The banner's label with that text: on the top layer, near the top */
static lv_obj_t *banner(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (lv_obj_get_screen(found[i]) == lv_layer_top() && a.y1 >= 0 && a.y2 < 300)
			return found[i];
	}
	return NULL;
}

static const char *awaited;

static bool banner_shown(void)
{
	return banner(awaited) != NULL;
}

static bool tap_banner(const char *text)
{
	lv_obj_t *b = banner(text);
	if (b)
		ui_test_tap(b);
	return b != NULL;
}

static bool banner_comes(const char *text, uint32_t ms)
{
	awaited = text;
	if (!ui_test_wait_until(banner_shown, ms))
		return false;
	ui_test_settle();
	return true;
}

/* The quick settings, opened from the status bar */
static void shade(void)
{
	ui_test_tap_at(360, UI_STATUS_H / 2);
	ui_test_settle();
}

static bool in_status_bar(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 <= UI_STATUS_H && !lv_obj_is_hidden(found[i]))
			return true;
	}
	return false;
}

TEST(started_with_phone_ui_once_signed_in)
{
	signed_in_before();
	ui_test_start();
	ui_test_wait(1500);
	CHECK_INT(ui_test_td_request_count("setTdlibParameters"), 1);
	/* TDLib's notifications asked for */
	CHECK(requested("setOption", "notification_group_count_max"));
	/* Opened, the same client */
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Chats", 1000));
	CHECK_INT(ui_test_td_request_count("setTdlibParameters"), 1);
}

TEST(not_started_before_signing_in)
{
	account();
	ui_test_start();
	ui_test_wait(1500);
	CHECK_INT(ui_test_td_request_count("setTdlibParameters"), 0);
}

/* A message in another app: a banner, then listed in the quick settings, a bell in the
 * status bar */
TEST(message_in_another_app)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	CHECK(banner("Ana") != NULL);
	CHECK(ui_test_shows("No notes")); /* nothing opened */
	ui_test_wait(4500);
	CHECK(banner("Are you coming?") == NULL);
	CHECK(in_status_bar(LV_SYMBOL_BELL));
	shade();
	CHECK(ui_test_shows("Notifications"));
	CHECK(ui_test_shows("Ana"));
	CHECK(ui_test_shows("Are you coming?"));
}

/* Away from Telegram, TDLib is asked every second, not ten times a second */
TEST(polled_slowly_in_the_background)
{
	CHECK(in_notes());
	ui_test_wait(1100); /* a poll just done */
	ana_writes(1, "Are you coming?", 1);
	ui_test_wait(300);
	CHECK(banner("Are you coming?") == NULL);
	CHECK(banner_comes("Are you coming?", 1200));
}

/* In Telegram, quickly */
TEST(polled_quickly_in_front)
{
	signed_in_before();
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Chats", 1000));
	chats();
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 200));
}

/* Several messages: the last one, how many */
TEST(several_messages)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	ana_writes(2, "We are at the bar", 2);
	CHECK(banner_comes("We are at the bar", 2000));
	ui_test_wait(4500);
	shade();
	CHECK(ui_test_shows("We are at the bar"));
	CHECK(!ui_test_shows("Are you coming?"));
	CHECK(ui_test_shows("2"));
}

/* In a group, who wrote it */
TEST(group_message_names_the_sender)
{
	CHECK(in_notes());
	notified(2, FRIENDS, 303, 1, "Pizza tonight?", 1, false);
	CHECK(banner_comes("Ben: Pizza tonight?", 2000));
	CHECK(banner("Friends") != NULL);
}

/* Silent (muted chats are left out by TDLib, these are sent without sound): listed only */
TEST(silent_listed_without_banner)
{
	CHECK(in_notes());
	notified(1, ANA, ANA, 1, "No rush", 1, true);
	ui_test_wait(2000);
	CHECK(banner("No rush") == NULL);
	shade();
	CHECK(ui_test_shows("No rush"));
}

/* Read elsewhere (or in Telegram): gone */
TEST(read_withdraws_it)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	ana_writes(2, "We are at the bar", 2);
	CHECK(banner_comes("We are at the bar", 2000));
	ana_read(1, 2);
	ui_test_wait(6000);
	CHECK(!in_status_bar(LV_SYMBOL_BELL));
	shade();
	CHECK(!ui_test_shows("Notifications"));
	CHECK(!ui_test_shows("We are at the bar"));
}

TEST(on_the_lock_screen)
{
	CHECK(in_notes());
	ui_lock();
	ui_test_settle();
	ana_writes(1, "Are you coming?", 1);
	ui_test_wait(6000); /* the banner gone, the lock screen's card follows each second */
	CHECK(ui_locked());
	CHECK(ui_test_shows("Are you coming?"));
	CHECK(ui_test_shows("Ana"));
}

/* Tapped, the banner opens the chat, which reads it */
TEST(banner_tapped_opens_the_chat)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	CHECK(tap_banner("Are you coming?"));
	CHECK(requested("openChat", "\"chat_id\":4242"));
	CHECK(ui_test_shows("Ana"));
	CHECK(ui_test_textarea() || ui_test_tap_field("Message"));
	/* Back: Telegram's chats, then Notes */
	CHECK(ui_test_back());
	CHECK(ui_test_wait_for("Chats", 1000));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("No notes"));
}

/* From the quick settings, while locked: unlocked, the chat opens */
TEST(opened_from_the_shade)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	ui_test_wait(4500);
	shade();
	CHECK(ui_test_tap_text("Are you coming?"));
	CHECK(requested("openChat", "\"chat_id\":4242"));
	CHECK(ui_test_textarea() || ui_test_tap_field("Message"));
}

/* In that chat, no banner: the message shows there */
TEST(no_banner_in_its_chat)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	CHECK(tap_banner("Are you coming?"));
	ui_test_wait(4500);
	ana_writes(2, "We are at the bar", 2);
	ui_test_wait(1000);
	CHECK(banner("We are at the bar") == NULL);
	/* In another chat, one */
	notified(2, FRIENDS, 303, 3, "Pizza tonight?", 1, false);
	CHECK(banner_comes("Ben: Pizza tonight?", 1000));
}

/* Tapped in another chat: that one opens instead */
TEST(banner_tapped_in_another_chat)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	CHECK(tap_banner("Are you coming?"));
	ui_test_wait(4500);
	notified(2, FRIENDS, 303, 3, "Pizza tonight?", 1, false);
	CHECK(banner_comes("Ben: Pizza tonight?", 1000));
	CHECK(tap_banner("Ben: Pizza tonight?"));
	ui_test_wait(1000);
	CHECK(requested("openChat", "\"chat_id\":5151"));
	CHECK(ui_test_shows("Friends"));
	CHECK(ui_test_textarea() || ui_test_tap_field("Message"));
}

/* Its icon in the menu counts them */
TEST(badge_on_its_icon)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	ana_writes(2, "We are at the bar", 2);
	CHECK(banner_comes("We are at the bar", 2000));
	ui_home();
	ui_open_menu();
	ui_test_settle();
	ui_test_wait(1100);
	lv_obj_t *label = ui_test_find("Telegram"), *badge = NULL;
	CHECK(label != NULL);
	if (!label)
		return;
	lv_area_t l;
	lv_obj_get_coords(label, &l);
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all("2", found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y2 < l.y1 && a.y2 > l.y1 - 160 && a.x1 > l.x1)
			badge = found[i];
	}
	CHECK(badge != NULL);
}

/* Clear all in the quick settings: Telegram's go too */
TEST(cleared_from_the_shade)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	ui_test_wait(4500);
	shade();
	CHECK(ui_test_tap_text("Clear all"));
	CHECK(!ui_test_shows("Are you coming?"));
	ui_test_wait(1100);
	CHECK(!in_status_bar(LV_SYMBOL_BELL));
}

/* Replies: a message swiped left is replied to, the bar above the field says to what;
 * replies show the message they answer at the top of their bubble */

#define MARA 101

/* Mara's message `id` in her chat, answering `reply_to` (a messageReplyTo's JSON, NULL none) */
static void mara_wrote(long long id, const char *text, const char *reply_to)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%d,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%d},\"is_outgoing\":false,\"date\":%lld,\"reply_to\":%s,\"content\":"
	         "{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}",
	         id, MARA, MARA, (long long)time(NULL) - 60, reply_to ? reply_to : "null", text);
	ui_test_td_message(buf);
}

/* The account's message, sent earlier */
static void i_wrote(long long id, const char *text)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%d,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%d},\"is_outgoing\":true,\"date\":%lld,\"content\":{\"@type\":\"messageText\","
	         "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}",
	         id, MARA, FAKE_TD_ME, (long long)time(NULL) - 120, text);
	ui_test_td_message(buf);
}

/* Her chat open from the contacts, `shown` in it */
static bool open_mara(const char *shown)
{
	account();
	ui_test_td_contact(MARA, "Mara", "Ionescu", NULL, NULL);
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":101,"
			  "\"type\":{\"@type\":\"chatTypePrivate\",\"user_id\":101},\"title\":\"Mara Ionescu\","
			  "\"photo\":null,\"positions\":[],\"unread_count\":0,\"last_message\":null}}");
	return open_contacts() && ui_test_wait_for("Mara Ionescu", 1000) && ui_test_tap_text("Mara Ionescu") &&
	       ui_test_wait_for(shown, 1000);
}

/* The bubble of a message's text, swiped left by `dx` */
static bool swipe_left(const char *text, int32_t dx)
{
	lv_obj_t *label = ui_test_find(text);
	if (!label)
		return false;
	lv_area_t a;
	lv_obj_get_coords(lv_obj_get_parent(label), &a);
	int32_t x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
	ui_test_swipe(x, y, x - dx, y, 300);
	return true;
}

/* The bubble of the label with that text holds one with `other` too (its quote) */
static bool same_bubble(const char *text, const char *other)
{
	lv_obj_t *bubble = ui_test_find(text) ? lv_obj_get_parent(ui_test_find(text)) : NULL;
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(other, found, 8);
	for (unsigned i = 0; bubble && i < n && i < 8; i++)
		for (lv_obj_t *p = lv_obj_get_parent(found[i]); p; p = lv_obj_get_parent(p))
			if (p == bubble)
				return true;
	printf("  \"%s\" not in the bubble of \"%s\"\n", other, text);
	return false;
}

TEST(swipe_replies)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	CHECK(!ui_test_shows("Mara Ionescu ")); /* nothing replied to yet */
	CHECK(swipe_left("Lunch tomorrow?", 200));
	/* The bar above the field: her name and the message; typing at once */
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 2);
	CHECK(ui_test_textarea() != NULL);
	CHECK(ui_test_type("Yes, at noon\n"));
	ui_test_wait(500);
	CHECK(requested("sendMessage", "\"reply_to\":{\"@type\":\"inputMessageReplyToMessage\",\"message_id\":1048576}"));
	/* The bar gone; the reply quotes her message */
	CHECK(ui_test_wait_for("Yes, at noon", 1000));
	CHECK(same_bubble("Yes, at noon", "Lunch tomorrow?"));
	CHECK(same_bubble("Yes, at noon", "Mara Ionescu"));
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 2);
	/* The next message answers nothing */
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("See you\n"));
	ui_test_wait(500);
	CHECK(requested("sendMessage", "See you"));
	CHECK(!strstr(ui_test_td_requested("sendMessage"), "reply_to"));
}

/* A short swipe, or one up the list, replies to nothing */
TEST(short_swipe_does_not_reply)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	CHECK(swipe_left("Lunch tomorrow?", 60));
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 1);
	CHECK(ui_test_textarea() == NULL);
}

TEST(reply_canceled)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	CHECK(swipe_left("Lunch tomorrow?", 200));
	CHECK(ui_test_tap_text(LV_SYMBOL_CLOSE));
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 1);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	ui_test_wait(500);
	CHECK(requested("sendMessage", "Hello"));
	CHECK(!strstr(ui_test_td_requested("sendMessage"), "reply_to"));
}

/* Her reply to the account's message, both loaded: "You" and its text */
TEST(reply_to_a_message_shown)
{
	i_wrote(1048576, "Are you free on Sunday?");
	mara_wrote(2097152, "I am, why?",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":101,\"message_id\":1048576}");
	CHECK(open_mara("I am, why?"));
	CHECK(same_bubble("I am, why?", "Are you free on Sunday?"));
	CHECK(same_bubble("I am, why?", "You"));
	CHECK_INT(ui_test_td_request_count("getRepliedMessage"), 0);
}

/* The message answered is not loaded: asked for */
TEST(reply_to_an_older_message)
{
	i_wrote(1048576, "Are you free on Sunday?");
	for (int i = 0; i < 60; i++) {
		char text[32];
		snprintf(text, sizeof(text), "Filler %d", i);
		i_wrote(2097152 + i * 1048576LL, text);
	}
	mara_wrote(100 * 1048576LL, "Yes!",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":101,\"message_id\":1048576}");
	CHECK(open_mara("Yes!"));
	CHECK(requested("getRepliedMessage", "\"message_id\":104857600"));
	CHECK(ui_test_wait_for("Are you free on Sunday?", 1000));
	CHECK(same_bubble("Yes!", "Are you free on Sunday?"));
	CHECK(same_bubble("Yes!", "You"));
}

/* Deleted since: said so */
TEST(reply_to_a_deleted_message)
{
	mara_wrote(2097152, "As I said",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":101,\"message_id\":1048576}");
	CHECK(open_mara("As I said"));
	CHECK(ui_test_wait_for("Deleted message", 1000));
	CHECK(same_bubble("As I said", "Deleted message"));
}

/* A part of it quoted: that part, not the whole message */
TEST(reply_with_a_quote)
{
	i_wrote(1048576, "Bring bread and cheese and wine");
	mara_wrote(2097152, "Which cheese?",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":101,\"message_id\":1048576,\"quote\":"
	           "{\"@type\":\"textQuote\",\"text\":{\"@type\":\"formattedText\",\"text\":\"cheese\","
	           "\"entities\":[]},\"position\":15,\"is_manual\":true}}");
	CHECK(open_mara("Which cheese?"));
	CHECK(same_bubble("Which cheese?", "cheese"));
}

/* Deleted accounts: TDLib gives them no name (userTypeDeleted), and their private chat an
 * empty title; named as other Telegram apps do */

#define GHOST 6060

static void deleted_account(void)
{
	ui_test_td_update("{\"@type\":\"updateUser\",\"user\":{\"@type\":\"user\",\"id\":6060,\"first_name\":\"\","
	                  "\"last_name\":\"\",\"status\":{\"@type\":\"userStatusEmpty\"},"
	                  "\"type\":{\"@type\":\"userTypeDeleted\"}}}");
}

TEST(deleted_account_chat)
{
	account();
	deleted_account();
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":6060,\"title\":\"\","
	                  "\"type\":{\"@type\":\"chatTypePrivate\",\"user_id\":6060},\"photo\":null,\"unread_count\":0,"
	                  "\"last_message\":null,\"positions\":[{\"@type\":\"chatPosition\",\"list\":"
	                  "{\"@type\":\"chatListMain\"},\"order\":\"100\",\"is_pinned\":false}]}}");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Deleted Account", 1000));
	CHECK(ui_test_shows(UI_SYMBOL_GHOST)); /* its avatar, not an initial */
	CHECK(!ui_test_shows("D"));
	CHECK(ui_test_tap_text("Deleted Account"));
	CHECK(ui_test_wait_for("No messages", 1000));
	CHECK(ui_test_shows("Deleted Account")); /* the chat's title */
}

/* In a group, as the sender of a message */
TEST(deleted_account_in_a_group)
{
	account();
	deleted_account();
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":5151,\"title\":\"Friends\","
	                  "\"type\":{\"@type\":\"chatTypeBasicGroup\",\"basic_group_id\":51},\"photo\":null,"
	                  "\"unread_count\":0,\"last_message\":null,\"positions\":[{\"@type\":\"chatPosition\","
	                  "\"list\":{\"@type\":\"chatListMain\"},\"order\":\"100\",\"is_pinned\":false}]}}");
	ui_test_td_message("{\"@type\":\"message\",\"id\":1048576,\"chat_id\":5151,\"sender_id\":"
	                   "{\"@type\":\"messageSenderUser\",\"user_id\":6060},\"is_outgoing\":false,\"date\":1793966400,"
	                   "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\","
	                   "\"text\":\"Hello from the past\",\"entities\":[]}}}");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Friends", 1000));
	CHECK(ui_test_tap_text("Friends"));
	CHECK(ui_test_wait_for("Hello from the past", 1000));
	CHECK(ui_test_shows("Deleted Account"));
}

/* Link previews and Instant View: a message with a link shows its page's site, title
 * and description under the text; a page Telegram has an instant view of has an
 * Instant View button, which opens the article in the app (getWebPageInstantView) */

#define COFFEE "https://perfectdailygrind.com/2021/03/exploring-the-science-behind-adding-salt-to-coffee/"

/* Mara's link to the coffee article: with an instant view of version `iv` (0: none) */
static void mara_links(long long id, int iv)
{
	char buf[2048];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%d,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%d},\"is_outgoing\":false,\"date\":%lld,\"content\":{\"@type\":\"messageText\","
	         "\"text\":{\"@type\":\"formattedText\",\"text\":\"Look: " COFFEE "\",\"entities\":[]},"
	         "\"link_preview\":{\"@type\":\"linkPreview\",\"url\":\"" COFFEE "\",\"display_url\":"
	         "\"perfectdailygrind.com/2021/03/exploring-the-science-behind-adding-salt-to-coffee\","
	         "\"site_name\":\"Perfect Daily Grind\",\"title\":\"Exploring the science behind adding salt to coffee\","
	         "\"description\":{\"@type\":\"formattedText\",\"text\":\"Why a pinch of salt makes coffee taste less bitter\","
	         "\"entities\":[]},\"author\":\"\",\"type\":{\"@type\":\"linkPreviewTypeArticle\",\"photo\":null},"
	         "\"instant_view_version\":%d}}}",
	         id, MARA, MARA, (long long)time(NULL) - 60, iv);
	ui_test_td_message(buf);
}

/* Its author and date, on a line */
#define BYLINE "Janice Ng \xc2\xb7 15 March 2021"

#define PLAIN(t) "{\"@type\":\"richTextPlain\",\"text\":\"" t "\"}"

/* The article as Telegram's instant view has it, a photo (file 700) after its first part */
static const char coffee_view[] =
	"{\"@type\":\"webPageInstantView\",\"version\":2,\"view_count\":0,\"is_rtl\":false,\"is_full\":true,"
	"\"blocks\":["
	"{\"@type\":\"pageBlockTitle\",\"title\":" PLAIN("Exploring the science behind adding salt to coffee") "},"
	"{\"@type\":\"pageBlockAuthorDate\",\"author\":" PLAIN("Janice Ng") ",\"publish_date\":1615766400},"
	"{\"@type\":\"pageBlockParagraph\",\"text\":{\"@type\":\"richTexts\",\"texts\":["
		PLAIN("Salt has long been added to coffee ") ","
		"{\"@type\":\"richTextBold\",\"text\":" PLAIN("in some cultures") "},"
		PLAIN(", and ") ","
		"{\"@type\":\"richTextUrl\",\"text\":" PLAIN("research") ",\"url\":\"https://example.com/study\","
		"\"is_cached\":false},"
		PLAIN(" explains why.") "]}},"
	"{\"@type\":\"pageBlockPhoto\",\"photo\":{\"@type\":\"photo\",\"has_stickers\":false,\"sizes\":["
		"{\"@type\":\"photoSize\",\"type\":\"m\",\"width\":320,\"height\":240,\"photo\":{\"@type\":\"file\",\"id\":701,"
		"\"local\":{\"@type\":\"localFile\",\"path\":\"\",\"is_downloading_completed\":false}}},"
		"{\"@type\":\"photoSize\",\"type\":\"x\",\"width\":800,\"height\":600,\"photo\":{\"@type\":\"file\",\"id\":700,"
		"\"local\":{\"@type\":\"localFile\",\"path\":\"\",\"is_downloading_completed\":false}}}]},"
		"\"caption\":{\"@type\":\"pageBlockCaption\",\"text\":" PLAIN("A cup of coffee") ",\"credit\":" PLAIN("Photo: PDG") "},"
		"\"url\":\"\",\"has_spoiler\":false},"
	"{\"@type\":\"pageBlockHeader\",\"header\":" PLAIN("How salt works") "},"
	"{\"@type\":\"pageBlockList\",\"items\":["
		"{\"@type\":\"pageBlockListItem\",\"label\":\"\\u2022\",\"blocks\":[{\"@type\":\"pageBlockParagraph\",\"text\":"
		PLAIN("Sodium blocks bitterness") "}]},"
		"{\"@type\":\"pageBlockListItem\",\"label\":\"\\u2022\",\"blocks\":[{\"@type\":\"pageBlockParagraph\",\"text\":"
		PLAIN("A pinch is enough") "}]}]},"
	"{\"@type\":\"pageBlockBlockQuote\",\"blocks\":[{\"@type\":\"pageBlockParagraph\",\"text\":"
		PLAIN("Taste is chemistry.") "}],\"credit\":" PLAIN("A barista") "},"
	"{\"@type\":\"pageBlockDivider\"},"
	"{\"@type\":\"pageBlockVideo\",\"video\":null,\"caption\":{\"@type\":\"pageBlockCaption\",\"text\":"
		PLAIN("Brewing it") ",\"credit\":" PLAIN("") "},\"need_autoplay\":false,\"is_looped\":false,\"has_spoiler\":false},"
	"{\"@type\":\"pageBlockUnsupported\"},"
	"{\"@type\":\"pageBlockFooter\",\"footer\":" PLAIN("Perfect Daily Grind") "}"
	"]}";

TEST(link_preview_shown)
{
	mara_links(1048576, 0);
	CHECK(open_mara("Exploring the science behind adding salt to coffee"));
	CHECK(ui_test_shows("Perfect Daily Grind"));
	CHECK(ui_test_shows("Why a pinch of salt makes coffee taste less bitter"));
	CHECK(same_bubble("Look: " COFFEE, "Perfect Daily Grind"));
	/* No instant view of it: no button */
	CHECK(!ui_test_shows("Instant View"));
}

TEST(instant_view_opens)
{
	mara_links(1048576, 2);
	ui_test_td_instant_view(COFFEE, coffee_view);
	CHECK(open_mara("Exploring the science behind adding salt to coffee"));
	CHECK(ui_test_tap_text("Instant View"));
	CHECK(requested("getWebPageInstantView", "\"url\":\"" COFFEE "\""));
	CHECK(ui_test_wait_for(BYLINE, 1000));
	/* The site in the header, the blocks in order */
	CHECK(ui_test_shows("Perfect Daily Grind"));
	CHECK(ui_test_shows("Exploring the science behind adding salt to coffee"));
	CHECK(ui_test_shows("Salt has long been added to coffee in some cultures, and research explains why."));
	CHECK(ui_test_shows("How salt works"));
	CHECK(ui_test_shows("Sodium blocks bitterness"));
	CHECK(ui_test_shows("A pinch is enough"));
	CHECK(ui_test_shows("Taste is chemistry."));
	CHECK(ui_test_shows("A barista"));
	CHECK(ui_test_shows("A cup of coffee"));
	CHECK(ui_test_shows_part("Photo: PDG"));
	/* What it cannot show yet, said so: a video by its caption */
	CHECK(ui_test_shows("Brewing it"));
	CHECK(y_of("Exploring the science behind adding salt to coffee") < y_of(BYLINE));
	CHECK(y_of("How salt works") < y_of("Sodium blocks bitterness"));
	CHECK(y_of("Sodium blocks bitterness") < y_of("Taste is chemistry."));
	/* Back: the chat */
	CHECK(ui_test_back());
	CHECK(ui_test_wait_for("Instant View", 1000));
}

/* Its photo: the smallest size as wide as the screen, downloaded, then drawn */
TEST(instant_view_photo)
{
	photo_write("/var/lib/telegram/coffee.jpg", 800, 600);
	ui_test_td_file(700, "/var/lib/telegram/coffee.jpg");
	mara_links(1048576, 2);
	ui_test_td_instant_view(COFFEE, coffee_view);
	CHECK(open_mara("Exploring the science behind adding salt to coffee"));
	unsigned before = photos_shown();
	CHECK(ui_test_tap_text("Instant View"));
	CHECK(ui_test_wait_for(BYLINE, 1000));
	CHECK(requested("downloadFile", "\"file_id\":700"));
	CHECK_INT(photos_shown(), before + 1);
}

/* Loading, then what it could not do */
TEST(instant_view_loading_and_failing)
{
	mara_links(1048576, 2);
	CHECK(open_mara("Exploring the science behind adding salt to coffee"));
	ui_test_td_hold(true);
	CHECK(ui_test_tap_text("Instant View"));
	CHECK(ui_test_shows("Loading..."));
	ui_test_td_hold(false);
	CHECK(ui_test_wait_for("Cannot load this page", 1000));
}

/* Sending without network: TDLib keeps the message pending and sends it once the network
 * is back; meanwhile the chat says so. Refused by the server: "Not sent". */

TEST(sent_without_network)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	ui_test_td_offline(true);
	CHECK(ui_test_wait_for("Waiting for network...", 1000));
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Yes\n"));
	CHECK(ui_test_wait_for("Sending...", 1000));
	ui_test_wait(2000);
	CHECK(ui_test_shows("Sending..."));
	CHECK(!ui_test_shows_part("Not sent"));
	/* Back: sent, the notice gone */
	ui_test_td_offline(false);
	ui_test_wait(500);
	CHECK(!ui_test_shows("Sending..."));
	CHECK(!ui_test_shows("Waiting for network..."));
	CHECK(ui_test_shows("Yes"));
}

/* On the chat list too, without any message pending */
TEST(network_notice_in_the_chat_list)
{
	account();
	ui_test_td_offline(true);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Waiting for network...", 1000));
	ui_test_td_offline(false);
	/* Polled every second at first: the screen was still sliding in */
	ui_test_wait(1500);
	CHECK(!ui_test_shows("Waiting for network..."));
}

TEST(send_refused)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	ui_test_td_send_refused(403, "USER_IS_BLOCKED", false);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Yes\n"));
	ui_test_wait(500);
	CHECK(ui_test_shows_part("Not sent"));
	CHECK(!ui_test_shows("Sending..."));
	CHECK(ui_test_shows("Yes"));
	/* Why, in words; it cannot be sent again, only deleted */
	CHECK(ui_test_shows("This person blocked you."));
	CHECK(!ui_test_shows("Retry"));
	CHECK(ui_test_tap_text("Delete"));
	CHECK(requested("deleteMessages", "\"chat_id\":101"));
	ui_test_wait(500);
	CHECK(!ui_test_shows("Yes"));
	CHECK(!ui_test_shows_part("Not sent"));
	CHECK(!ui_test_shows("Delete"));
}

/* Refused for now (too many messages): sent again, then sent */
TEST(send_refused_retried)
{
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	ui_test_td_send_refused(429, "Too Many Requests: retry after 3", true);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Yes\n"));
	ui_test_wait(500);
	CHECK(ui_test_shows("Too Many Requests: retry after 3"));
	CHECK(ui_test_tap_text("Retry"));
	CHECK(requested("resendMessages", "\"chat_id\":101"));
	ui_test_wait(500);
	CHECK(ui_test_shows("Yes"));
	CHECK(!ui_test_shows_part("Not sent"));
	CHECK(!ui_test_shows("Retry"));
}
