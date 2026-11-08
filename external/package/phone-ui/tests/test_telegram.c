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
#include <unistd.h>

#include <webp/encode.h>

#include "test.h"
#include "fake_tdjson.h"
#include "i18n.h"
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

/* Signing in: what each step asks, what went wrong */

/* The phone number typed, on the code's step */
static bool phone_typed(void)
{
	return ui_test_open("Telegram") && ui_test_wait_for("Your phone number", 1000) && ui_test_tap_field("+") &&
	       ui_test_type("40712345678\n") && ui_test_wait_for("Enter the code", 1000);
}

/* Without TDLib in the app's lib/ (its runpath only: dlopen() finds no other) */
TEST(no_tdlib)
{
	void *lib = dlopen(APP "/lib/libtdjson.so", RTLD_NOW | RTLD_NOLOAD);
	CHECK(lib != NULL);
	if (lib) {
		dlclose(lib);
		dlclose(lib);
	}
	unlink(APP "/lib/libtdjson.so");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Telegram is not installed", 1000));
	CHECK(ui_test_shows("This firmware has no TDLib (libtdjson.so)."));
	CHECK(!ui_test_shows("Next"));
}

/* Without the API credentials: how to get them; tried again once they are there */
TEST(no_api_credentials)
{
	unlink("/var/lib/telegram/api");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("No API credentials", 1000));
	CHECK(ui_test_shows_part("my.telegram.org"));
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_shows("No API credentials"));
	ui_test_write("/var/lib/telegram/api", "12345 0123456789abcdef0123456789abcdef\n");
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
}

/* The code's step as TDLib describes it: sent `type` (JSON) to the number */
static void code_sent(const char *type)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":"
	         "\"authorizationStateWaitCode\",\"code_info\":{\"@type\":\"authenticationCodeInfo\","
	         "\"phone_number\":\"+40712345678\",\"type\":%s,\"timeout\":60}}}", type);
	ui_test_td_update(buf);
}

/* Where the code went, as TDLib says */
TEST(code_sent_by)
{
	CHECK(phone_typed());
	CHECK(ui_test_shows("Sent to 40712345678 by SMS."));
	code_sent("{\"@type\":\"authenticationCodeTypeTelegramMessage\",\"length\":5}");
	CHECK(ui_test_wait_for("Sent to +40712345678 in Telegram on your other devices.", 1000));
	code_sent("{\"@type\":\"authenticationCodeTypeCall\",\"length\":5}");
	CHECK(ui_test_wait_for("Sent to +40712345678 by a phone call.", 1000));
	code_sent("{\"@type\":\"authenticationCodeTypeFlashCall\",\"pattern\":\"*\"}");
	CHECK(ui_test_wait_for("Sent to +40712345678.", 1000));
}

/* A wrong code: said so, the next one signs in */
TEST(wrong_code)
{
	CHECK(phone_typed());
	CHECK(ui_test_tap_field("Code"));
	CHECK(ui_test_type("11111\n"));
	CHECK(ui_test_wait_for("Wrong code", 1000));
	CHECK(ui_test_shows("Enter the code"));
	CHECK(clear_field("11111"));
	CHECK(ui_test_type(FAKE_TD_CODE "\n"));
	CHECK(ui_test_wait_for("No chats", 1000));
	CHECK(!ui_test_shows("Wrong code"));
}

/* TDLib's errors in words, the others as it says them */
TEST(phone_number_refused)
{
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(ui_test_tap_field("+"));
	ui_test_td_fail("setAuthenticationPhoneNumber", 400, "PHONE_NUMBER_INVALID");
	CHECK(ui_test_type("407\n"));
	CHECK(ui_test_wait_for("Invalid phone number", 1000));
	/* The keyboard hid as it was sent */
	ui_test_td_fail("setAuthenticationPhoneNumber", 429, "Too Many Requests: retry after 60");
	CHECK(ui_test_tap_text("Next"));
	CHECK(ui_test_wait_for("Too many attempts, try again later", 1000));
	ui_test_td_fail("setAuthenticationPhoneNumber", 400, "PHONE_NUMBER_BANNED");
	CHECK(ui_test_tap_text("Next"));
	CHECK(ui_test_wait_for("PHONE_NUMBER_BANNED", 1000));
	CHECK(ui_test_shows("Your phone number"));
}

/* Two-step verification: the password, hidden as typed, with its hint */
TEST(two_step_password)
{
	ui_test_td_password("hunter22", "the dog");
	CHECK(phone_typed());
	CHECK(ui_test_tap_field("Code"));
	CHECK(ui_test_type(FAKE_TD_CODE "\n"));
	CHECK(ui_test_wait_for("Two-step verification", 1000));
	CHECK(ui_test_shows("Your password. Hint: the dog"));
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_textarea() && lv_textarea_get_password_mode(ui_test_textarea()));
	CHECK(ui_test_type("nope\n"));
	CHECK(ui_test_wait_for("Wrong password", 1000));
	CHECK(clear_field("Password"));
	CHECK(ui_test_type("hunter22\n"));
	CHECK(ui_test_wait_for("No chats", 1000));
	CHECK(requested("checkAuthenticationPassword", "\"password\":\"hunter22\""));
}

/* Next, while TDLib has not answered: waits, sent once */
TEST(next_waits_for_the_answer)
{
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_type("40712345678"));
	ui_test_td_hold(true);
	CHECK(ui_test_tap_text("Next"));
	CHECK(ui_test_shows("Wait..."));
	CHECK(ui_test_tap_text("Wait..."));
	ui_test_td_hold(false);
	CHECK(ui_test_wait_for("Enter the code", 1000));
	CHECK_INT(ui_test_td_request_count("setAuthenticationPhoneNumber"), 1);
}

/* The keyboard's hide key hides it; the field shows it again */
TEST(login_keyboard_hidden)
{
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_textarea() != NULL);
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(ui_test_textarea() == NULL);
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_textarea() != NULL);
}

/* A step this app does not do (an email address to set up): said so, and logging out */
TEST(unsupported_step)
{
	CHECK(phone_typed());
	ui_test_td_update("{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":"
	                  "\"authorizationStateWaitEmailAddress\",\"allow_apple_id\":false,\"allow_google_id\":false}}");
	CHECK(ui_test_wait_for("Not supported yet", 1000));
	CHECK(ui_test_shows_part("authorizationStateWaitEmailAddress"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK_INT(ui_test_td_request_count("logOut"), 1);
}

/* Something TDLib sends that is not JSON is skipped, what follows still counts */
TEST(unreadable_update_skipped)
{
	account();
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("No chats", 1000));
	ui_test_td_update("{\"@type\":\"updateNewChat\",");
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":4242,\"title\":\"Ana\","
	                  "\"type\":{\"@type\":\"chatTypePrivate\",\"user_id\":4242},\"positions\":"
	                  "[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},\"order\":\"100\","
	                  "\"is_pinned\":false}]}}");
	CHECK(ui_test_wait_for("Ana", 1000));
}

/* The chat lists: order, last message and its time, unread count, pinned chats, avatars,
 * the archive; following TDLib's updates */

/* A text message's JSON, from `sender` (the account: outgoing), `date` */
static void text_message(char *buf, size_t size, long long chat, long long id, long long sender, long long date,
                         const char *text)
{
	snprintf(buf, size,
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%lld,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%lld},\"is_outgoing\":%s,\"date\":%lld,\"content\":{\"@type\":\"messageText\","
	         "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}",
	         id, chat, sender, sender == FAKE_TD_ME ? "true" : "false", date, text);
}

/* A chat (updateNewChat) of that type (JSON) at `order` in `list`, its last message (JSON,
 * "null" none), unread count and photo (a chatPhotoInfo's JSON, "null" none) */
static void chat_in(const char *list, long long id, const char *title, const char *type, int order, bool pinned,
                    const char *last, int unread, const char *photo)
{
	char buf[4096];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":%lld,\"title\":\"%s\","
	         "\"type\":%s,\"photo\":%s,\"unread_count\":%d,\"last_message\":%s,\"positions\":"
	         "[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"%s\"},\"order\":\"%d\",\"is_pinned\":%s}]}}",
	         id, title, type, photo, unread, last, list, order, pinned ? "true" : "false");
	ui_test_td_update(buf);
}

#define PRIVATE(id) "{\"@type\":\"chatTypePrivate\",\"user_id\":" #id "}"
#define GROUP "{\"@type\":\"chatTypeBasicGroup\",\"basic_group_id\":51}"
#define CHANNEL "{\"@type\":\"chatTypeSupergroup\",\"supergroup_id\":71,\"is_channel\":true}"

static void main_chat(long long id, const char *title, int order)
{
	chat_in("chatListMain", id, title, PRIVATE(1), order, false, "null", 0, "null");
}

static bool above(const char *a, const char *b)
{
	int32_t ya = y_of(a), yb = y_of(b);
	if (ya < 0 || yb < 0 || ya >= yb) {
		printf("  \"%s\" (%d) not above \"%s\" (%d)\n", a, (int)ya, b, (int)yb);
		return false;
	}
	return true;
}

TEST(chat_list)
{
	ui_test_wall_clock(NOW);
	account();
	char last[1024];
	text_message(last, sizeof(last), 4242, 1048576, 4242, NOW - 600, "See you");
	chat_in("chatListMain", 4242, "andrei", PRIVATE(4242), 300, false, last, 2, "null");
	text_message(last, sizeof(last), 5151, 1048576, FAKE_TD_ME, NOW - 2 * 86400, "Chapter 3");
	chat_in("chatListMain", 5151, "Book club", GROUP, 200, true, last, 0, "null");
	chat_in("chatListMain", 6262, "Zed", PRIVATE(6262), 100, false, "null", 0, "null");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("andrei", 1000));
	CHECK(ui_test_shows("A")); /* its initial, upper case */
	CHECK(ui_test_shows("See you"));
	CHECK(ui_test_shows("11:50"));
	CHECK(ui_test_shows("2"));
	CHECK(ui_test_shows("You: Chapter 3"));
	CHECK(ui_test_shows("Wed"));
	CHECK(above("andrei", "Book club"));
	CHECK(above("Book club", "Zed"));
	CHECK_INT(photos_shown(), 1); /* Book club's pin */

	/* Renamed, read, moved; a new last message takes it to the top */
	ui_test_td_update("{\"@type\":\"updateChatTitle\",\"chat_id\":4242,\"title\":\"Andrei Pop\"}");
	ui_test_td_update("{\"@type\":\"updateChatReadInbox\",\"chat_id\":4242,\"last_read_inbox_message_id\":1048576,"
	                  "\"unread_count\":0}");
	ui_test_td_update("{\"@type\":\"updateChatPosition\",\"chat_id\":6262,\"position\":{\"@type\":\"chatPosition\","
	                  "\"list\":{\"@type\":\"chatListMain\"},\"order\":\"400\",\"is_pinned\":false}}");
	text_message(last, sizeof(last), 5151, 2097152, 303, NOW - 60, "Next one?");
	char buf[2048];
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateChatLastMessage\",\"chat_id\":5151,\"last_message\":%s,"
	         "\"positions\":[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},\"order\":\"500\","
	         "\"is_pinned\":true}]}", last);
	ui_test_td_update(buf);
	ui_test_td_update("{\"@type\":\"updateChatDraftMessage\",\"chat_id\":4242,\"draft_message\":null,"
	                  "\"positions\":[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},"
	                  "\"order\":\"50\",\"is_pinned\":false}]}");
	/* Of a chat not known, or not about the list: nothing changes */
	ui_test_td_update("{\"@type\":\"updateChatTitle\",\"chat_id\":999,\"title\":\"Nobody\"}");
	ui_test_td_update("{\"@type\":\"updateChatAccentColors\",\"chat_id\":4242,\"accent_color_id\":3}");
	CHECK(ui_test_wait_for("Andrei Pop", 1000));
	CHECK(ui_test_shows("Next one?"));
	CHECK(!ui_test_shows("2"));
	CHECK(!ui_test_shows("Nobody"));
	CHECK(above("Book club", "Zed"));
	CHECK(above("Zed", "Andrei Pop"));
}

/* The chat with oneself: Saved messages, with its own avatar */
TEST(saved_messages)
{
	account();
	chat_in("chatListMain", FAKE_TD_ME, "Elian Doran", PRIVATE(777000111), 100, false, "null", 0, "null");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Saved messages", 1000));
	CHECK(!ui_test_shows("Elian Doran"));
	CHECK_INT(photos_shown(), 1);
	CHECK(ui_test_tap_text("Saved messages"));
	CHECK(ui_test_wait_for("No messages", 1000));
	CHECK(ui_test_shows("Saved messages"));
}

/* A chat's small photo, `id`, downloaded at `path` ("" not yet) */
static void chat_photo(char *buf, size_t size, int id, const char *path)
{
	snprintf(buf, size, "{\"@type\":\"chatPhotoInfo\",\"small\":{\"@type\":\"file\",\"id\":%d,\"local\":"
	         "{\"@type\":\"localFile\",\"path\":\"%s\",\"is_downloading_completed\":%s}},\"big\":{\"@type\":\"file\","
	         "\"id\":%d,\"local\":{\"@type\":\"localFile\",\"path\":\"\",\"is_downloading_completed\":false}},"
	         "\"has_animation\":false,\"is_personal\":false}", id, path, path[0] ? "true" : "false", id + 1);
}

/* Avatars: downloaded unless they are, a photo that cannot be read leaves the initial */
TEST(chat_avatars)
{
	account();
	photo_write("/var/lib/telegram/ana.jpg", 160, 160);
	photo_write("/var/lib/telegram/ben.jpg", 160, 160);
	ui_test_write("/var/lib/telegram/cat.jpg", "not a JPEG");
	ui_test_td_file(900, "/var/lib/telegram/ana.jpg");
	char photo[1024];
	chat_photo(photo, sizeof(photo), 900, "");
	chat_in("chatListMain", 4242, "Ana", PRIVATE(4242), 300, false, "null", 0, photo);
	chat_photo(photo, sizeof(photo), 910, "/var/lib/telegram/ben.jpg");
	chat_in("chatListMain", 4343, "Ben", PRIVATE(4343), 200, false, "null", 0, photo);
	chat_photo(photo, sizeof(photo), 920, "/var/lib/telegram/cat.jpg");
	chat_in("chatListMain", 4444, "Cat", PRIVATE(4444), 100, false, "null", 0, photo);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Cat", 1000));
	ui_test_wait(500);
	CHECK_INT(photos_shown(), 2);
	CHECK(requested("downloadFile", "\"file_id\":900"));
	CHECK_INT(ui_test_td_request_count("downloadFile"), 1);
	CHECK(!ui_test_shows("A"));
	CHECK(ui_test_shows("C"));
	/* Removed: the initial again */
	ui_test_td_update("{\"@type\":\"updateChatPhoto\",\"chat_id\":4242,\"photo\":null}");
	CHECK(ui_test_wait_for("A", 1000));
	CHECK_INT(photos_shown(), 1);
}

/* Archived chats: a row above the others names them, opens them as a list of their own */
TEST(archived_chats)
{
	account();
	main_chat(4242, "Ana", 100);
	chat_in("chatListArchive", 8001, "Old group", GROUP, 300, false, "null", 3, "null");
	chat_in("chatListArchive", 8002, "Spam bot", PRIVATE(8002), 200, false, "null", 0, "null");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Archived chats", 1000));
	CHECK(ui_test_shows("Old group, Spam bot"));
	CHECK(ui_test_shows("1")); /* unread chats */
	CHECK(above("Archived chats", "Ana"));
	CHECK(!ui_test_shows("Old group"));
	CHECK(ui_test_tap_text("Archived chats"));
	CHECK(ui_test_wait_for("Old group", 1000));
	CHECK(ui_test_shows("3"));
	CHECK(above("Old group", "Spam bot"));
	CHECK(!ui_test_shows("Ana"));
	CHECK(ui_test_tap_text("Spam bot"));
	CHECK(ui_test_wait_for("No messages", 1000));
	CHECK(ui_test_back());
	CHECK(ui_test_wait_for("Old group", 1000));
	CHECK(ui_test_back());
	CHECK(ui_test_wait_for("Ana", 1000));
}

/* Their names cut to fit, never in the middle of a character */
TEST(archived_chats_names_cut)
{
	account();
	/* 20 two-byte letters and one: the cut falls in a letter */
	char title[64] = "", all[512] = "";
	for (int i = 0; i < 20; i++)
		strcat(title, "\xc3\xa9");
	strcat(title, "a");
	for (int i = 0; i < 7; i++) {
		chat_in("chatListArchive", 8001 + i, title, GROUP, 300 - i, false, "null", 0, "null");
		snprintf(all + strlen(all), sizeof(all) - strlen(all), "%s%s", i ? ", " : "", title);
	}
	all[253] = '\0'; /* up to the last whole letter */
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Archived chats", 1000));
	CHECK(ui_test_shows(all));
}

/* A page of chats that could not be loaded: not "No chats", asked again at the end of the list */
TEST(chat_list_page_failed)
{
	account();
	ui_test_td_fail("loadChats", 500, "Internal Server Error"); /* the archive's */
	ui_test_td_fail("loadChats", 500, "Internal Server Error");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Loading chats...", 1000));
	ui_test_wait(500);
	CHECK(ui_test_shows("Loading chats..."));
	CHECK_INT(ui_test_td_request_count("loadChats"), 2);
	for (int i = 0; i < 15; i++) {
		char title[32];
		snprintf(title, sizeof(title), "Chat %d", i);
		main_chat(9000 + i, title, 100 - i);
	}
	CHECK(ui_test_wait_for("Chat 0", 1000));
	ui_test_swipe(360, 1200, 360, 300, 300);
	CHECK_INT(ui_test_td_request_count("loadChats"), 3);
}

/* Logging out: the chats go with the account */
TEST(chats_forgotten_on_log_out)
{
	account();
	photo_write("/var/lib/telegram/ana.jpg", 160, 160);
	char photo[1024];
	chat_photo(photo, sizeof(photo), 910, "/var/lib/telegram/ana.jpg");
	chat_in("chatListMain", 4242, "Ana", PRIVATE(4242), 300, false, "null", 0, photo);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Ana", 1000));
	CHECK(ui_test_tap_text("Profile"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	CHECK(ui_test_tap_field("+"));
	CHECK(ui_test_type("40712345678\n"));
	CHECK(ui_test_wait_for("Enter the code", 1000));
	CHECK(ui_test_tap_field("Code"));
	CHECK(ui_test_type(FAKE_TD_CODE "\n"));
	CHECK(ui_test_wait_for("No chats", 1000));
	CHECK(!ui_test_shows("Ana"));
}

/* A channel posting in a group: named by its title */
TEST(group_message_from_a_channel)
{
	account();
	chat_in("chatListMain", 5151, "Friends", GROUP, 200, false, "null", 0, "null");
	chat_in("chatListMain", 7070, "News", CHANNEL, 100, false, "null", 0, "null");
	ui_test_td_message("{\"@type\":\"message\",\"id\":1048576,\"chat_id\":5151,\"sender_id\":"
	                   "{\"@type\":\"messageSenderChat\",\"chat_id\":7070},\"is_outgoing\":false,\"date\":1793966400,"
	                   "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\","
	                   "\"text\":\"Breaking\",\"entities\":[]}}}");
	ui_test_td_message("{\"@type\":\"message\",\"id\":1048576,\"chat_id\":7070,\"sender_id\":"
	                   "{\"@type\":\"messageSenderChat\",\"chat_id\":7070},\"is_outgoing\":false,\"date\":1793966400,"
	                   "\"content\":{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\","
	                   "\"text\":\"On the channel\",\"entities\":[]}}}");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_tap_text("Friends"));
	CHECK(ui_test_wait_for("Breaking", 1000));
	CHECK_INT(ui_test_find_all("News", NULL, 0), 1);
	/* In the channel itself, no names */
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("News"));
	CHECK(ui_test_wait_for("On the channel", 1000));
	CHECK_INT(ui_test_find_all("News", NULL, 0), 1); /* the title */
}

/* An open chat: photos, stickers, other kinds of messages, messages edited, deleted,
 * coming late; older ones loaded scrolling up; photos sent from the Gallery */

/* Mara's message `id` with this content (JSON), `ago` seconds ago */
static void mara_sent(long long id, long long ago, const char *content)
{
	char buf[4096];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%d,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%d},\"is_outgoing\":false,\"date\":%lld,\"content\":%s}",
	         id, MARA, MARA, (long long)time(NULL) - ago, content);
	ui_test_td_message(buf);
}

/* A JPEG of w x h in one color (0xRRGGBB) */
static void jpeg_write(const char *path, int w, int h, uint32_t rgb)
{
	uint8_t *px = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) {
		px[i * 4] = rgb & 0xff;
		px[i * 4 + 1] = rgb >> 8 & 0xff;
		px[i * 4 + 2] = rgb >> 16 & 0xff;
		px[i * 4 + 3] = 0xff;
	}
	FILE *f = fopen(path, "wb");
	ui_jpeg_write(f, px, w, h, w * 4, 90, false);
	fclose(f);
	free(px);
}

/* A file not downloaded yet, as photos and stickers carry it */
#define REMOTE(id) "{\"@type\":\"file\",\"id\":" #id ",\"local\":{\"@type\":\"localFile\",\"path\":\"\"," \
	"\"is_downloading_completed\":false}}"

/* A photo of three sizes (files 801 to 803), with a caption */
#define PHOTO(caption) "{\"@type\":\"messagePhoto\",\"photo\":{\"@type\":\"photo\",\"has_stickers\":false,\"sizes\":[" \
	"{\"@type\":\"photoSize\",\"type\":\"m\",\"width\":320,\"height\":240,\"photo\":" REMOTE(801) "}," \
	"{\"@type\":\"photoSize\",\"type\":\"x\",\"width\":800,\"height\":600,\"photo\":" REMOTE(802) "}," \
	"{\"@type\":\"photoSize\",\"type\":\"y\",\"width\":1280,\"height\":960,\"photo\":" REMOTE(803) "}]}," \
	"\"caption\":{\"@type\":\"formattedText\",\"text\":\"" caption "\",\"entities\":[]}}"

/* How many labels show `text` under the status bar */
static unsigned below_status_bar(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8), below = 0;
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		below += a.y1 > UI_STATUS_H;
	}
	return below;
}

static bool color_near(lv_color_t c, uint32_t rgb)
{
	int r = rgb >> 16, g = rgb >> 8 & 0xff, b = rgb & 0xff;
	if (abs(c.red - r) <= 16 && abs(c.green - g) <= 16 && abs(c.blue - b) <= 16)
		return true;
	printf("  color %02x%02x%02x, not about %06x\n", c.red, c.green, c.blue, (unsigned)rgb);
	return false;
}

/* The size as wide as the bubble at least is shown; tapped, the largest one in the viewer */
TEST(photo_shown_and_opened)
{
	mkdir("/var/lib/telegram", 0755);
	jpeg_write("/var/lib/telegram/x.jpg", 800, 600, 0xc04040);
	jpeg_write("/var/lib/telegram/y.jpg", 1280, 960, 0x4040c0);
	ui_test_td_file(802, "/var/lib/telegram/x.jpg");
	ui_test_td_file(803, "/var/lib/telegram/y.jpg");
	mara_sent(1048576, 60, PHOTO("Our view"));
	CHECK(open_mara("Our view"));
	CHECK(requested("downloadFile", "\"file_id\":802"));
	CHECK_INT(photos_shown(), 1);
	CHECK(!ui_test_shows("Photo"));
	lv_obj_t *photo = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(photo != NULL);
	if (!photo)
		return;
	CHECK(ui_test_shows_color(photo, 0xc04040));
	ui_test_tap(photo);
	CHECK(ui_test_wait_for("Photo", 1000));
	CHECK(requested("downloadFile", "\"file_id\":803"));
	/* The largest once downloaded */
	ui_test_wait(1000);
	CHECK(color_near(ui_test_pixel(360, 800), 0x4040c0));
}

/* A photo that cannot be read: a warning in its place */
TEST(photo_not_readable)
{
	ui_test_write("/var/lib/telegram/x.jpg", "not a JPEG");
	ui_test_td_file(802, "/var/lib/telegram/x.jpg");
	mara_sent(1048576, 60, PHOTO("Our view"));
	CHECK(open_mara("Our view"));
	ui_test_wait(500);
	CHECK_INT(photos_shown(), 0);
	CHECK_INT(below_status_bar(LV_SYMBOL_WARNING), 1); /* the status bar has one too */
	CHECK_INT(ui_test_find_all(LV_SYMBOL_IMAGE, NULL, 0), 1); /* the Gallery's button only */
}

/* The same photo again (forwarded): not downloaded twice */
TEST(same_photo_twice)
{
	mkdir("/var/lib/telegram", 0755);
	jpeg_write("/var/lib/telegram/x.jpg", 800, 600, 0xc04040);
	ui_test_td_file(802, "/var/lib/telegram/x.jpg");
	mara_sent(1048576, 60, PHOTO("Our view"));
	CHECK(open_mara("Our view"));
	CHECK_INT(photos_shown(), 1);
	char buf[4096];
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateNewMessage\",\"message\":{\"@type\":\"message\",\"id\":2097152,"
	         "\"chat_id\":101,\"sender_id\":{\"@type\":\"messageSenderUser\",\"user_id\":101},\"is_outgoing\":false,"
	         "\"date\":%lld,\"content\":" PHOTO("Again") "}}", (long long)time(NULL));
	ui_test_td_update(buf);
	CHECK(ui_test_wait_for("Again", 1000));
	CHECK_INT(photos_shown(), 2);
	CHECK_INT(ui_test_td_request_count("downloadFile"), 1);
}

/* Without any size: what it is */
TEST(photo_without_sizes)
{
	mara_sent(1048576, 60, "{\"@type\":\"messagePhoto\",\"photo\":{\"@type\":\"photo\",\"sizes\":[]},"
	                       "\"caption\":{\"@type\":\"formattedText\",\"text\":\"\",\"entities\":[]}}");
	CHECK(open_mara("Photo"));
	CHECK_INT(ui_test_td_request_count("downloadFile"), 0);
}

/* A photo replying: the message it answers above it */
TEST(photo_reply)
{
	i_wrote(1048576, "Show me the view");
	char buf[4096];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"message\",\"id\":2097152,\"chat_id\":101,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":101},\"is_outgoing\":false,\"date\":%lld,\"reply_to\":{\"@type\":\"messageReplyToMessage\","
	         "\"chat_id\":101,\"message_id\":1048576},\"content\":" PHOTO("") "}", (long long)time(NULL) - 60);
	ui_test_td_message(buf);
	CHECK(open_mara("Show me the view"));
	CHECK_INT(ui_test_find_all("Show me the view", NULL, 0), 2);
	CHECK(ui_test_shows("You"));
}

/* A WEBP of w x h in one color (0xAARRGGBB), as stickers are */
static void webp_write(const char *path, int w, int h, uint32_t argb)
{
	uint32_t *px = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++)
		px[i] = argb; /* BGRA in memory */
	uint8_t *out = NULL;
	size_t size = WebPEncodeLosslessBGRA((const uint8_t *)px, w, h, w * 4, &out);
	FILE *f = fopen(path, "wb");
	fwrite(out, 1, size, f);
	fclose(f);
	WebPFree(out);
	free(px);
}

/* A WEBP cut short: its header whole, its picture not */
static void webp_cut_write(const char *path)
{
	enum { W = 256, H = 256 };
	uint8_t *px = malloc(W * H * 4);
	for (int i = 0; i < W * H * 4; i++)
		px[i] = (uint8_t)(i * 7919 >> 3); /* noise: a long picture */
	uint8_t *out = NULL;
	size_t size = WebPEncodeBGRA(px, W, H, W * 4, 90, &out);
	FILE *f = fopen(path, "wb");
	fwrite(out, 1, size / 2, f);
	fclose(f);
	WebPFree(out);
	free(px);
}

/* A sticker (JSON): `format` its file's, a thumbnail (JSON, "null" none) */
static void sticker(char *buf, size_t size, int file, const char *format, const char *thumbnail, const char *emoji)
{
	snprintf(buf, size,
	         "{\"@type\":\"messageSticker\",\"sticker\":{\"@type\":\"sticker\",\"id\":\"1\",\"set_id\":\"2\","
	         "\"width\":512,\"height\":512,\"emoji\":\"%s\",\"format\":{\"@type\":\"%s\"},"
	         "\"full_type\":{\"@type\":\"stickerFullTypeRegular\"},\"thumbnail\":%s,\"sticker\":"
	         "{\"@type\":\"file\",\"id\":%d,\"local\":{\"@type\":\"localFile\",\"path\":\"\","
	         "\"is_downloading_completed\":false}}},\"is_premium\":false}", emoji, format, thumbnail, file);
}

#define GRIN "\xf0\x9f\x98\x80"

/* Static stickers drawn without a bubble; animated ones by their thumbnail (a WEBP or a JPEG) */
TEST(stickers_shown)
{
	mkdir("/var/lib/telegram", 0755);
	webp_write("/var/lib/telegram/s.webp", 512, 512, 0xff20a020);
	webp_write("/var/lib/telegram/t.webp", 128, 128, 0xff20a020);
	jpeg_write("/var/lib/telegram/t.jpg", 128, 128, 0x20a020);
	ui_test_td_file(830, "/var/lib/telegram/s.webp");
	ui_test_td_file(841, "/var/lib/telegram/t.webp");
	ui_test_td_file(851, "/var/lib/telegram/t.jpg");
	char buf[2048];
	sticker(buf, sizeof(buf), 830, "stickerFormatWebp", "null", GRIN);
	mara_sent(1048576, 60, buf);
	sticker(buf, sizeof(buf), 840, "stickerFormatTgs", "{\"@type\":\"thumbnail\",\"format\":{\"@type\":"
	        "\"thumbnailFormatWebp\"},\"width\":128,\"height\":128,\"file\":" REMOTE(841) "}", GRIN);
	mara_sent(2097152, 60, buf);
	sticker(buf, sizeof(buf), 850, "stickerFormatWebm", "{\"@type\":\"thumbnail\",\"format\":{\"@type\":"
	        "\"thumbnailFormatJpeg\"},\"width\":128,\"height\":128,\"file\":" REMOTE(851) "}", GRIN);
	mara_sent(3145728, 60, buf);
	i_wrote(4194304, "Nice");
	CHECK(open_mara("Nice"));
	ui_test_wait(1000);
	lv_obj_t *found[3];
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, found, 3), 3);
	/* As large as the sticker, the thumbnail too */
	for (int i = 0; i < 3 && found[i]; i++)
		CHECK_INT(lv_obj_get_width(found[i]), 256);
	CHECK(!ui_test_shows("Sticker"));
	CHECK(ui_test_shows_color(found[0], 0x20a020));
}

/* Without an image (an animated one with no thumbnail): its emoji, large; one that cannot
 * be read keeps its emoji */
TEST(stickers_as_emoji)
{
	mkdir("/var/lib/telegram", 0755);
	ui_test_write("/var/lib/telegram/empty.webp", "");
	webp_cut_write("/var/lib/telegram/bad.webp");
	ui_test_td_file(830, "/var/lib/telegram/empty.webp");
	ui_test_td_file(831, "/var/lib/telegram/bad.webp");
	char buf[2048];
	sticker(buf, sizeof(buf), 820, "stickerFormatTgs", "null", GRIN);
	mara_sent(1048576, 60, buf);
	sticker(buf, sizeof(buf), 821, "stickerFormatTgs", "null", "");
	mara_sent(2097152, 60, buf);
	sticker(buf, sizeof(buf), 830, "stickerFormatWebp", "null", "\xe2\x9d\xa4");
	mara_sent(3145728, 60, buf);
	sticker(buf, sizeof(buf), 831, "stickerFormatWebp", "null", "\xf0\x9f\x91\x8d");
	mara_sent(4194304, 60, buf);
	i_wrote(5242880, "Nice");
	CHECK(open_mara("Nice"));
	ui_test_wait(1000);
	CHECK_INT(photos_shown(), 0);
	char *grin = ui_emoji_compose(GRIN), *heart = ui_emoji_compose("\xe2\x9d\xa4");
	char *thumb = ui_emoji_compose("\xf0\x9f\x91\x8d");
	CHECK(ui_test_font(grin) == UI_FONT_DISPLAY);
	CHECK(ui_test_shows("Sticker"));
	CHECK(ui_test_shows(heart));
	CHECK(ui_test_shows(thumb));
	free(grin);
	free(heart);
	free(thumb);
}

/* Messages of one emoji (animated, dice): large */
TEST(big_emoji_and_dice)
{
	mara_sent(1048576, 60, "{\"@type\":\"messageAnimatedEmoji\",\"animated_emoji\":{\"@type\":\"animatedEmoji\","
	                       "\"sticker\":null,\"sticker_width\":512,\"sticker_height\":512,\"fitzpatrick_type\":0},"
	                       "\"emoji\":\"\xf0\x9f\x91\x8d\"}");
	mara_sent(2097152, 60, "{\"@type\":\"messageDice\",\"initial_state\":null,\"final_state\":null,"
	                       "\"emoji\":\"\xf0\x9f\x8e\xb2\",\"value\":4,\"success_animation_frame_number\":0}");
	i_wrote(3145728, "Nice");
	CHECK(open_mara("Nice"));
	char *thumb = ui_emoji_compose("\xf0\x9f\x91\x8d"), *die = ui_emoji_compose("\xf0\x9f\x8e\xb2");
	CHECK(ui_test_font(thumb) == UI_FONT_DISPLAY);
	CHECK(ui_test_font(die) == UI_FONT_DISPLAY);
	CHECK(ui_test_font("Nice") != UI_FONT_DISPLAY);
	free(thumb);
	free(die);
}

/* What it cannot show: its kind, with the caption; what it does not know, said so */
TEST(other_kinds_of_messages)
{
	mara_sent(1048576, 60, "{\"@type\":\"messageVideo\",\"video\":null,\"caption\":{\"@type\":\"formattedText\","
	                       "\"text\":\"At the beach\",\"entities\":[]}}");
	mara_sent(2097152, 60, "{\"@type\":\"messageDocument\",\"document\":null,\"caption\":{\"@type\":"
	                       "\"formattedText\",\"text\":\"\",\"entities\":[]}}");
	mara_sent(3145728, 60, "{\"@type\":\"messageChatAddMembers\",\"member_user_ids\":[303]}");
	CHECK(open_mara("Video\nAt the beach"));
	CHECK(ui_test_shows("File"));
	CHECK(ui_test_shows("Unsupported message"));
}

/* A line for each day: today, yesterday, this year's, an earlier year's */
/* An earlier day as the chat names it: with its year if not this one's */
static void day_text(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	if (tm.tm_year == today.tm_year)
		snprintf(buf, size, "%s %d %s", ui_day_name(tm.tm_wday, false), tm.tm_mday, ui_month_name(tm.tm_mon, false));
	else
		snprintf(buf, size, "%d %s %d", tm.tm_mday, ui_month_name(tm.tm_mon, false), tm.tm_year + 1900);
}

/* telegram.so's clock is the real one (time() is wrapped in the program only) */
TEST(day_lines)
{
	time_t now = time(NULL), ago = now - 400 * 86400LL, before = now - 3 * 86400;
	char buf[1024], long_ago[64], earlier[64];
	day_text(long_ago, sizeof(long_ago), ago);
	day_text(earlier, sizeof(earlier), before);
	text_message(buf, sizeof(buf), MARA, 1048576, MARA, ago, "Long ago");
	ui_test_td_message(buf);
	text_message(buf, sizeof(buf), MARA, 2097152, MARA, before, "Earlier");
	ui_test_td_message(buf);
	text_message(buf, sizeof(buf), MARA, 3145728, MARA, now - 86400, "Yesterday's");
	ui_test_td_message(buf);
	text_message(buf, sizeof(buf), MARA, 4194304, MARA, now - 60, "Today's");
	ui_test_td_message(buf);
	CHECK(open_mara("Today's"));
	CHECK(strstr(long_ago, " 20") != NULL); /* a year */
	CHECK(above(long_ago, "Long ago"));
	CHECK(above("Long ago", earlier));
	CHECK(above(earlier, "Earlier"));
	CHECK(above("Earlier", "Yesterday"));
	CHECK(above("Yesterday", "Yesterday's"));
	CHECK(above("Yesterday's", "Today"));
	CHECK(above("Today", "Today's"));
}

/* Replies to another chat's messages: who wrote them as TDLib says, their text as given */
TEST(reply_to_another_chat)
{
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":7070,\"title\":\"News\","
	                  "\"type\":" CHANNEL ",\"positions\":[]}}");
	ui_test_td_update("{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":5151,\"title\":\"Friends\","
	                  "\"type\":" GROUP ",\"positions\":[]}}");
#define TEXT(t) "{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\",\"text\":\"" t "\",\"entities\":[]}}"
	mara_wrote(1048576, "Hidden",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":999,\"message_id\":5,\"origin\":{\"@type\":"
	           "\"messageOriginHiddenUser\",\"sender_name\":\"Old Friend\"},\"content\":" TEXT("From elsewhere") "}");
	mara_wrote(2097152, "By her",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":999,\"message_id\":6,\"origin\":{\"@type\":"
	           "\"messageOriginUser\",\"sender_user_id\":101},\"content\":" TEXT("Her old words") "}");
	mara_wrote(3145728, "By a group",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":999,\"message_id\":7,\"origin\":{\"@type\":"
	           "\"messageOriginChat\",\"sender_chat_id\":5151,\"author_signature\":\"\"},\"content\":"
	           TEXT("Group words") "}");
	mara_wrote(4194304, "By a channel",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":999,\"message_id\":8,\"origin\":{\"@type\":"
	           "\"messageOriginChannel\",\"chat_id\":7070,\"message_id\":8,\"author_signature\":\"\"},\"content\":"
	           TEXT("Channel words") "}");
	mara_wrote(5242880, "Unknown", "{\"@type\":\"messageReplyToMessage\",\"chat_id\":999,\"message_id\":9}");
#undef TEXT
	CHECK(open_mara("Unknown"));
	CHECK(same_bubble("Hidden", "Old Friend"));
	CHECK(same_bubble("Hidden", "From elsewhere"));
	CHECK(same_bubble("By her", "Mara Ionescu"));
	CHECK(same_bubble("By her", "Her old words"));
	CHECK(same_bubble("By a group", "Friends"));
	CHECK(same_bubble("By a group", "Group words"));
	CHECK(same_bubble("By a channel", "News"));
	CHECK(same_bubble("By a channel", "Channel words"));
	CHECK(same_bubble("Unknown", "Message"));
	CHECK_INT(ui_test_td_request_count("getRepliedMessage"), 0);
	/* Left with them */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("last seen recently"));
}

/* A message as TDLib sends it new (updateNewMessage) */
static void mara_writes(long long id, const char *text)
{
	char msg[1024], buf[1200];
	text_message(msg, sizeof(msg), MARA, id, MARA, time(NULL), text);
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateNewMessage\",\"message\":%s}", msg);
	ui_test_td_update(buf);
}

/* New messages, one coming late (an older id), edited, deleted */
TEST(messages_change)
{
	CHECK(open_mara("No messages"));
	mara_writes(3145728, "Hello");
	CHECK(ui_test_wait_for("Hello", 1000));
	CHECK(!ui_test_shows("No messages"));
	mara_writes(1048576, "Earlier");
	CHECK(ui_test_wait_for("Earlier", 1000));
	CHECK(above("Earlier", "Hello"));
	/* Of another chat: not here */
	char msg[1024], buf[1200];
	text_message(msg, sizeof(msg), 5151, 4194304, 303, time(NULL), "Elsewhere");
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateNewMessage\",\"message\":%s}", msg);
	ui_test_td_update(buf);
	ui_test_td_update("{\"@type\":\"updateMessageContent\",\"chat_id\":101,\"message_id\":3145728,\"new_content\":"
	                  "{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\",\"text\":\"Hello there\","
	                  "\"entities\":[]}}}");
	ui_test_td_update("{\"@type\":\"updateMessageContent\",\"chat_id\":5151,\"message_id\":3145728,\"new_content\":"
	                  "{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\",\"text\":\"Not here\","
	                  "\"entities\":[]}}}");
	ui_test_td_update("{\"@type\":\"updateMessageContent\",\"chat_id\":101,\"message_id\":99,\"new_content\":"
	                  "{\"@type\":\"messageText\",\"text\":{\"@type\":\"formattedText\",\"text\":\"Nor here\","
	                  "\"entities\":[]}}}");
	CHECK(ui_test_wait_for("Hello there", 1000));
	CHECK(!ui_test_shows("Hello"));
	CHECK(!ui_test_shows("Elsewhere"));
	CHECK(!ui_test_shows("Not here"));
	CHECK(!ui_test_shows("Nor here"));
	/* Deleted with one not loaded */
	ui_test_td_update("{\"@type\":\"updateDeleteMessages\",\"chat_id\":101,\"message_ids\":[1048576,77],"
	                  "\"is_permanent\":true,\"from_cache\":false}");
	ui_test_wait(500);
	CHECK(!ui_test_shows("Earlier"));
	CHECK(ui_test_shows("Hello there"));
}

/* Mara's `n` messages, "Old 0" the oldest */
static void mara_history(int n)
{
	for (int i = 0; i < n; i++) {
		char text[32];
		snprintf(text, sizeof(text), "Old %d", i);
		mara_wrote((i + 1) * 1048576LL, text, NULL);
	}
}

/* The list scrolled up until older messages are asked for (the n-th getChatHistory) */
static bool scroll_up_until_asked(unsigned n)
{
	for (int i = 0; i < 10 && ui_test_td_request_count("getChatHistory") < n; i++)
		ui_test_swipe(360, 400, 360, 1300, 200);
	return ui_test_td_request_count("getChatHistory") == n;
}

/* Older messages as the list scrolls up; a page that failed is asked again */
TEST(older_messages_on_scroll)
{
	mara_history(80);
	CHECK(open_mara("Old 79"));
	CHECK(!ui_test_shows("Old 0"));
	CHECK_INT(ui_test_td_request_count("getChatHistory"), 1);
	ui_test_td_fail("getChatHistory", 500, "Internal Server Error");
	CHECK(scroll_up_until_asked(2));
	ui_test_wait(300);
	CHECK(!ui_test_shows("Old 0"));
	CHECK(scroll_up_until_asked(3));
	CHECK(ui_test_wait_for("Old 0", 1000));
	CHECK(requested("getChatHistory", "\"from_message_id\":32505856")); /* the oldest shown */
}

/* The message a reply answers comes once the chat is closed: nothing to show it in */
TEST(replied_after_leaving)
{
	i_wrote(1048576, "Are you free on Sunday?");
	mara_history(60);
	mara_wrote(100 * 1048576LL, "Yes!",
	           "{\"@type\":\"messageReplyToMessage\",\"chat_id\":101,\"message_id\":1048576}");
	account();
	main_chat(MARA, "Mara Ionescu", 100);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	/* The history answered, then what comes after it held: getRepliedMessage, asked
	 * once the history is in */
	ui_test_td_hold(true);
	CHECK(ui_test_tap_text("Mara Ionescu"));
	ui_test_td_hold(false);
	ui_test_td_hold(true);
	CHECK(ui_test_wait_for("Yes!", 1000));
	CHECK_INT(ui_test_td_request_count("getRepliedMessage"), 0);
	CHECK(ui_test_back());
	ui_test_td_hold(false);
	ui_test_wait(500);
	CHECK_INT(ui_test_td_request_count("getRepliedMessage"), 1);
	CHECK(ui_test_shows("Mara Ionescu"));
	CHECK(!ui_test_shows("Yes!"));
	/* Opened again: asked again, shown */
	CHECK(ui_test_tap_text("Mara Ionescu"));
	CHECK(ui_test_wait_for("Are you free on Sunday?", 1000));
	CHECK(same_bubble("Yes!", "Are you free on Sunday?"));
}

/* Refused by the server, why in words */
TEST(send_refused_reasons)
{
	CHECK(open_mara("No messages"));
	ui_test_td_send_refused(403, "CHAT_WRITE_FORBIDDEN", false);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("One\n"));
	CHECK(ui_test_wait_for("You cannot write in this chat.", 1000));
	ui_test_td_send_refused(400, "PEER_FLOOD", false);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Two\n"));
	CHECK(ui_test_wait_for("Too many messages: try again later.", 1000));
	ui_test_td_send_refused(400, "", false);
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Three\n"));
	ui_test_wait(500);
	CHECK_INT(ui_test_find_all("Not sent", NULL, 0), 1);
}

/* Refused before it was sent: said in the chat */
TEST(send_refused_at_once)
{
	CHECK(open_mara("No messages"));
	ui_test_td_fail("sendMessage", 400, "CHAT_RESTRICTED");
	CHECK(ui_test_tap_field("Message"));
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_wait_for(LV_SYMBOL_WARNING " Not sent: CHAT_RESTRICTED", 1000));
}

/* Refused once the chat was left: nothing to say it in */
TEST(send_refused_after_leaving)
{
	CHECK(open_mara("No messages"));
	ui_test_td_fail("sendMessage", 400, "CHAT_RESTRICTED");
	CHECK(ui_test_tap_field("Message"));
	ui_test_td_hold(true);
	CHECK(ui_test_type("Hello\n"));
	CHECK(ui_test_back());
	ui_test_td_hold(false);
	ui_test_wait(1500); /* polled every second while the screen slid */
	CHECK(requested("sendMessage", "Hello"));
	CHECK(ui_test_shows("last seen recently"));
	CHECK(!ui_test_shows_part("CHAT_RESTRICTED"));
}

static bool two_photos(void)
{
	return ui_test_find_widgets(&lv_3dtexture_class, NULL, 0) == 2;
}

/* Photos within three quarters of the width and 640 px high, never enlarged; one not
 * downloaded yet does not open */
TEST(photo_sizes)
{
	mkdir("/var/lib/telegram", 0755);
	jpeg_write("/var/lib/telegram/tall.jpg", 300, 1200, 0xc04040);
	jpeg_write("/var/lib/telegram/small.jpg", 200, 150, 0xc04040);
	ui_test_td_file(811, "/var/lib/telegram/tall.jpg");
	ui_test_td_file(812, "/var/lib/telegram/small.jpg");
	mara_sent(1048576, 60, "{\"@type\":\"messagePhoto\",\"photo\":{\"@type\":\"photo\",\"sizes\":[{\"@type\":"
	                       "\"photoSize\",\"type\":\"y\",\"width\":300,\"height\":1200,\"photo\":" REMOTE(811) "}]},"
	                       "\"caption\":{\"@type\":\"formattedText\",\"text\":\"Tall\",\"entities\":[]}}");
	mara_sent(2097152, 60, "{\"@type\":\"messagePhoto\",\"photo\":{\"@type\":\"photo\",\"sizes\":[{\"@type\":"
	                       "\"photoSize\",\"type\":\"s\",\"width\":200,\"height\":150,\"photo\":" REMOTE(812) "}]},"
	                       "\"caption\":{\"@type\":\"formattedText\",\"text\":\"Small\",\"entities\":[]}}");
	mara_sent(3145728, 60, "{\"@type\":\"messagePhoto\",\"photo\":{\"@type\":\"photo\",\"sizes\":[{\"@type\":"
	                       "\"photoSize\",\"type\":\"s\",\"width\":200,\"height\":150,\"photo\":" REMOTE(813) "}]},"
	                       "\"caption\":{\"@type\":\"formattedText\",\"text\":\"Coming\",\"entities\":[]}}");
	CHECK(open_mara("Coming"));
	CHECK(ui_test_wait_until(two_photos, 3000));
	lv_obj_t *found[2];
	if (ui_test_find_widgets(&lv_3dtexture_class, found, 2) != 2)
		return;
	CHECK(lv_obj_get_width(found[0]) == 160 && lv_obj_get_height(found[0]) == 640);
	CHECK(lv_obj_get_width(found[1]) == 200 && lv_obj_get_height(found[1]) == 150);
	/* The third's placeholder, above the Gallery's button */
	lv_obj_t *icons[2];
	CHECK_INT(ui_test_find_all(LV_SYMBOL_IMAGE, icons, 2), 2);
	ui_test_tap(lv_obj_get_parent(y_of("Coming") > 0 ? icons[0] : icons[1]));
	CHECK(!ui_test_shows("Photo"));
	CHECK(ui_test_shows("Coming"));
}

/* Switching themes: the pin in the new one's colors */
TEST(pin_in_both_themes)
{
	account();
	chat_in("chatListMain", 5151, "Book club", GROUP, 200, true, "null", 0, "null");
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Book club", 1000));
	lv_obj_t *pin = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(pin && ui_test_shows_color(pin, UI_COLOR_DIM));
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Display"));
	CHECK(ui_test_tap_text("Light"));
	CHECK(ui_light);
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Book club", 1000));
	pin = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(pin && ui_test_shows_color(pin, UI_COLOR_DIM));
}

/* The photo chosen from the Gallery, sent with its size */
static bool photo_chosen(void)
{
	if (!ui_test_tap_text(LV_SYMBOL_IMAGE) || !ui_test_wait_for("Send a photo", 1000))
		return false;
	lv_obj_t *thumb;
	if (ui_test_find_widgets(&lv_3dtexture_class, &thumb, 1) != 1)
		return false;
	ui_test_tap(thumb);
	if (!ui_test_tap_text(LV_SYMBOL_OK))
		return false;
	ui_test_wait(300);
	return true;
}

TEST(send_a_photo)
{
	photo_write(PHOTOS "/a.jpg", 320, 240);
	CHECK(open_mara("No messages"));
	CHECK(photo_chosen());
	CHECK(requested("sendMessage", "\"@type\":\"inputMessagePhoto\""));
	CHECK(requested("sendMessage", "\"path\":\"" PHOTOS "/a.jpg\"},\"width\":320,\"height\":240"));
	CHECK(requested("sendMessage", "\"chat_id\":101"));
	CHECK(!strstr(ui_test_td_requested("sendMessage"), "reply_to"));
	CHECK(ui_test_shows("Mara Ionescu"));
}

/* A screenshot: as a JPEG */
TEST(send_a_screenshot)
{
	mkdir(PHOTOS, 0755);
	FILE *f = fopen(PHOTOS "/shot.ppm", "w");
	fprintf(f, "P6\n64 48\n255\n");
	for (int i = 0; i < 64 * 48; i++)
		fwrite("\x40\x80\xc0", 1, 3, f);
	fclose(f);
	CHECK(open_mara("No messages"));
	CHECK(photo_chosen());
	CHECK(requested("sendMessage", "\"path\":\"/var/lib/telegram/sent/shot.jpg\"},\"width\":64,\"height\":48"));
	struct stat st;
	CHECK(stat("/var/lib/telegram/sent/shot.jpg", &st) == 0 && st.st_size > 0);
}

/* While replying: the photo is the reply */
TEST(send_a_photo_as_reply)
{
	photo_write(PHOTOS "/a.jpg", 320, 240);
	mara_wrote(1048576, "Lunch tomorrow?", NULL);
	CHECK(open_mara("Lunch tomorrow?"));
	CHECK(swipe_left("Lunch tomorrow?", 200));
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 2);
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(photo_chosen());
	CHECK(requested("sendMessage", "\"reply_to\":{\"@type\":\"inputMessageReplyToMessage\",\"message_id\":1048576}"));
	CHECK_INT(ui_test_find_all("Lunch tomorrow?", NULL, 0), 1);
}

/* A JPEG with a stray byte and markers without a length (a restart, TEM) between two
 * of its segments, which libjpeg skips (the Gallery shows it): its size read past them */
TEST(send_a_photo_with_stray_bytes)
{
	photo_write(PHOTOS "/a.jpg", 320, 240);
	FILE *f = fopen(PHOTOS "/a.jpg", "rb");
	static uint8_t data[65536];
	size_t n = f ? fread(data, 1, sizeof(data), f) : 0;
	if (f)
		fclose(f);
	CHECK(n > 6 && n < sizeof(data));
	size_t at = 4 + (data[4] << 8 | data[5]); /* after the first segment */
	f = fopen(PHOTOS "/a.jpg", "wb");
	fwrite(data, 1, at, f);
	fwrite("\0\xff\xd0\xff\x01", 1, 5, f);
	fwrite(data + at, 1, n - at, f);
	fclose(f);
	CHECK(open_mara("No messages"));
	CHECK(photo_chosen());
	CHECK(requested("sendMessage", "\"path\":\"" PHOTOS "/a.jpg\"},\"width\":320,\"height\":240"));
}

/* Notifications, more: those active when TDLib starts, edited, of other kinds, many,
 * withdrawn on logging out */

/* A notification of Ana's message (JSON) */
static void ana_note(char *buf, size_t size, int id, const char *text)
{
	long long now = time(NULL);
	snprintf(buf, size,
	         "{\"@type\":\"notification\",\"id\":%d,\"date\":%lld,\"is_silent\":false,\"type\":"
	         "{\"@type\":\"notificationTypeNewMessage\",\"show_preview\":true,\"message\":{\"@type\":\"message\","
	         "\"id\":%d,\"chat_id\":4242,\"sender_id\":{\"@type\":\"messageSenderUser\",\"user_id\":4242},"
	         "\"is_outgoing\":false,\"date\":%lld,\"content\":{\"@type\":\"messageText\",\"text\":"
	         "{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}}}}}", id, now, id * 1048576, now, text);
}

/* updateActiveNotifications: Ana's group with these messages */
static void ana_active(const char *const *texts, int n)
{
	char buf[8192], note[1024];
	size_t len = snprintf(buf, sizeof(buf),
	                      "{\"@type\":\"updateActiveNotifications\",\"groups\":[{\"@type\":\"notificationGroup\","
	                      "\"id\":1,\"type\":{\"@type\":\"notificationGroupTypeMessages\"},\"chat_id\":4242,"
	                      "\"total_count\":%d,\"notifications\":[", n);
	for (int i = 0; i < n; i++) {
		ana_note(note, sizeof(note), i + 1, texts[i]);
		len += snprintf(buf + len, sizeof(buf) - len, "%s%s", i ? "," : "", note);
	}
	snprintf(buf + len, sizeof(buf) - len, "]}]}");
	ui_test_td_update(buf);
}

/* Those TDLib still has when it starts: listed, without a banner; sent again, replaced */
TEST(active_notifications_at_start)
{
	CHECK(in_notes());
	const char *texts[] = { "Are you coming?", "We are at the bar" };
	ana_active(texts, 2);
	ui_test_wait(2000);
	CHECK(banner("We are at the bar") == NULL);
	CHECK(in_status_bar(LV_SYMBOL_BELL));
	shade();
	CHECK(ui_test_shows("Ana"));
	CHECK(ui_test_shows("We are at the bar"));
	CHECK(ui_test_shows("2"));
	ana_active(texts, 1);
	ui_test_wait(1100);
	CHECK(ui_test_shows("Are you coming?"));
	CHECK(!ui_test_shows("We are at the bar"));
	CHECK(!ui_test_shows("2"));
}

/* A message edited: its notification follows */
TEST(notification_edited)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	char note[1024], buf[1200];
	ana_note(note, sizeof(note), 1, "Are you coming tonight?");
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateNotification\",\"notification_group_id\":1,\"notification\":%s}",
	         note);
	ui_test_td_update(buf);
	/* Of a group not known: nothing */
	snprintf(buf, sizeof(buf), "{\"@type\":\"updateNotification\",\"notification_group_id\":9,\"notification\":%s}",
	         note);
	ui_test_td_update(buf);
	ui_test_wait(5000);
	shade();
	CHECK(ui_test_shows("Are you coming tonight?"));
	CHECK(!ui_test_shows("Are you coming?"));
}

/* Not of a message (a call): "Message" */
TEST(notification_of_another_kind)
{
	CHECK(in_notes());
	ui_test_td_update("{\"@type\":\"updateNotificationGroup\",\"notification_group_id\":3,\"type\":{\"@type\":"
	                  "\"notificationGroupTypeCalls\"},\"chat_id\":4242,\"notification_settings_chat_id\":4242,"
	                  "\"notification_sound_id\":\"0\",\"total_count\":1,\"added_notifications\":[{\"@type\":"
	                  "\"notification\",\"id\":1,\"date\":1793966400,\"is_silent\":false,\"type\":{\"@type\":"
	                  "\"notificationTypeNewCall\",\"call_id\":5}}],\"removed_notification_ids\":[]}");
	CHECK(banner_comes("Message", 2000));
	CHECK(banner("Ana") != NULL);
}

/* More than TDLib keeps per group: the latest, counted all */
TEST(many_notifications)
{
	CHECK(in_notes());
	for (int i = 1; i <= 12; i++) {
		char text[32];
		snprintf(text, sizeof(text), "Message %d", i);
		ana_writes(i, text, i);
	}
	CHECK(banner_comes("Message 12", 2000));
	ui_test_wait(4500);
	shade();
	CHECK(ui_test_shows("Message 12"));
	CHECK(ui_test_shows("12"));
}

/* Logging out: they go */
TEST(notifications_withdrawn_on_log_out)
{
	CHECK(in_notes());
	ana_writes(1, "Are you coming?", 1);
	CHECK(banner_comes("Are you coming?", 2000));
	/* Into the app through her chat (the fake reads nothing) */
	CHECK(tap_banner("Are you coming?"));
	CHECK(ui_test_back());
	CHECK(ui_test_wait_for("Chats", 1000));
	CHECK(ui_test_tap_text("Profile"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_tap_text("Log out"));
	CHECK(ui_test_wait_for("Your phone number", 1000));
	ui_test_wait(1100);
	CHECK(!in_status_bar(LV_SYMBOL_BELL));
	shade();
	CHECK(!ui_test_shows("Are you coming?"));
}

/* Instant View, more: every kind of block, styled text, photos, what is cut */

#define PARAGRAPH(t) "{\"@type\":\"pageBlockParagraph\",\"text\":" PLAIN(t) "}"
#define CAPTION(t) "{\"@type\":\"pageBlockCaption\",\"text\":" PLAIN(t) ",\"credit\":" PLAIN("") "}"
#define CELL(t, header) "{\"@type\":\"pageBlockTableCell\",\"text\":" PLAIN(t) ",\"is_header\":" header "," \
	"\"colspan\":1,\"rowspan\":1,\"align\":{\"@type\":\"pageBlockHorizontalAlignmentLeft\"}," \
	"\"valign\":{\"@type\":\"pageBlockVerticalAlignmentTop\"}}"

/* The instant view of COFFEE with these blocks (JSON, comma separated) */
static void coffee_blocks(const char *blocks)
{
	size_t n = strlen(blocks) + 256;
	char *view = malloc(n);
	snprintf(view, n, "{\"@type\":\"webPageInstantView\",\"version\":2,\"view_count\":0,\"is_rtl\":false,"
	         "\"is_full\":true,\"blocks\":[%s]}", blocks);
	ui_test_td_instant_view(COFFEE, view);
	free(view);
}

static bool instant_view_open(const char *shown)
{
	mara_links(1048576, 2);
	return open_mara("Exploring the science behind adding salt to coffee") && ui_test_tap_text("Instant View") &&
	       ui_test_wait_for(shown, 1000);
}

TEST(instant_view_blocks)
{
	coffee_blocks(
		"{\"@type\":\"pageBlockKicker\",\"kicker\":" PLAIN("SCIENCE") "},"
		"{\"@type\":\"pageBlockSubtitle\",\"subtitle\":" PLAIN("Why it works") "},"
		"{\"@type\":\"pageBlockSubheader\",\"subheader\":" PLAIN("A subheader") "},"
		"{\"@type\":\"pageBlockSectionHeading\",\"text\":" PLAIN("Heading one") ",\"size\":1},"
		"{\"@type\":\"pageBlockSectionHeading\",\"text\":" PLAIN("Heading two") ",\"size\":2},"
		"{\"@type\":\"pageBlockSectionHeading\",\"text\":" PLAIN("Heading three") ",\"size\":3},"
		"{\"@type\":\"pageBlockPreformatted\",\"text\":" PLAIN("int salt = 1;") ",\"language\":\"c\"},"
		"{\"@type\":\"pageBlockPullQuote\",\"text\":" PLAIN("Pulled out") ",\"credit\":" PLAIN("Someone") "},"
		"{\"@type\":\"pageBlockCover\",\"cover\":" PARAGRAPH("Under the cover") "},"
		"{\"@type\":\"pageBlockCollage\",\"blocks\":[" PARAGRAPH("In a collage") "],\"caption\":"
			CAPTION("The collage") "},"
		"{\"@type\":\"pageBlockSlideshow\",\"blocks\":[],\"caption\":" CAPTION("The slideshow") "},"
		"{\"@type\":\"pageBlockEmbeddedPost\",\"url\":\"\",\"author\":\"A poster\",\"author_photo\":null,"
			"\"date\":0,\"blocks\":[" PARAGRAPH("Posted words") "],\"caption\":" CAPTION("The post") "},"
		"{\"@type\":\"pageBlockDetails\",\"header\":" PLAIN("More") ",\"blocks\":[" PARAGRAPH("Details inside") "],"
			"\"is_open\":false},"
		"{\"@type\":\"pageBlockTable\",\"caption\":" PLAIN("Salt by country") ",\"cells\":["
			"[" CELL("Country", "true") "," CELL("Grams", "true") "],[],"
			"[" CELL("Ethiopia", "false") "," CELL("0.5", "false") "]],\"is_bordered\":true,\"is_striped\":false},"
		"{\"@type\":\"pageBlockRelatedArticles\",\"header\":" PLAIN("Related") ",\"articles\":["
			"{\"@type\":\"pageBlockRelatedArticle\",\"url\":\"https://example.com/a\",\"title\":\"Sugar in tea\"},"
			"{\"@type\":\"pageBlockRelatedArticle\",\"url\":\"https://example.com/b\"}]},"
		"{\"@type\":\"pageBlockAudio\",\"audio\":null,\"caption\":" CAPTION("A song") "},"
		"{\"@type\":\"pageBlockDocument\",\"caption\":" CAPTION("") "},"
		"{\"@type\":\"pageBlockMap\",\"location\":null,\"zoom\":12,\"width\":100,\"height\":100,\"caption\":"
			CAPTION("") "},"
		"{\"@type\":\"pageBlockEmbedded\",\"url\":\"\",\"html\":\"\",\"caption\":" CAPTION("") "},"
		"{\"@type\":\"pageBlockMathematicalExpression\",\"expression\":\"E = mc^2\"},"
		"{\"@type\":\"pageBlockChatLink\",\"title\":\"Coffee lovers\",\"photo\":null,\"username\":\"coffee\"},"
		"{\"@type\":\"pageBlockAnchor\",\"name\":\"end\"}");
	CHECK(instant_view_open("SCIENCE"));
	CHECK(ui_test_shows("Why it works"));
	CHECK(ui_test_shows("A subheader"));
	CHECK(ui_test_font("Heading one") == UI_FONT_HEADLINE);
	CHECK(ui_test_font("Heading two") == UI_FONT_TITLE);
	CHECK(ui_test_font("Heading three") == UI_FONT_BODY_LARGE);
	CHECK(ui_test_font("int salt = 1;") == &ui_font_mono_20);
	CHECK(ui_test_shows("Pulled out"));
	CHECK(ui_test_shows("Someone"));
	CHECK(ui_test_shows("Under the cover"));
	CHECK(ui_test_shows("In a collage"));
	CHECK(ui_test_shows("The collage"));
	CHECK(ui_test_shows("The slideshow"));
	CHECK(ui_test_shows("A poster"));
	CHECK(above("A poster", "Posted words"));
	CHECK(ui_test_shows("The post"));
	CHECK(above("More", "Details inside"));
	CHECK(ui_test_shows("Salt by country"));
	/* A row of cells per row */
	CHECK(y_of("Country") == y_of("Grams"));
	CHECK(y_of("Ethiopia") == y_of("0.5"));
	CHECK(above("Country", "Ethiopia"));
	CHECK(ui_test_shows("Related"));
	CHECK(ui_test_shows("Sugar in tea"));
	CHECK(ui_test_shows("https://example.com/b"));
	CHECK(ui_test_shows(LV_SYMBOL_AUDIO "  Audio"));
	CHECK(ui_test_shows("A song"));
	CHECK(ui_test_shows(LV_SYMBOL_FILE "  File"));
	CHECK(ui_test_shows(LV_SYMBOL_GPS "  Map"));
	CHECK(ui_test_shows(LV_SYMBOL_IMAGE "  Embedded content"));
	CHECK(ui_test_font("E = mc^2") == &ui_font_mono_20);
	CHECK(ui_test_shows("Coffee lovers"));
}

/* Text styles: one span group of their runs */
TEST(instant_view_styles)
{
	coffee_blocks(
		"{\"@type\":\"pageBlockParagraph\",\"text\":{\"@type\":\"richTexts\",\"texts\":["
		PLAIN("Run ") ","
		"{\"@type\":\"richTextFixed\",\"text\":" PLAIN("code") "},"
		PLAIN(", ") ","
		"{\"@type\":\"richTextStrikethrough\",\"text\":" PLAIN("old") "},"
		PLAIN(", ") ","
		"{\"@type\":\"richTextUnderline\",\"text\":" PLAIN("new") "},"
		PLAIN(", ") ","
		"{\"@type\":\"richTextCustomEmoji\",\"custom_emoji_id\":\"1\",\"alternative_text\":\"(cup)\"},"
		PLAIN(" ") ","
		"{\"@type\":\"richTextMathematicalExpression\",\"expression\":\"x+1\"},"
		PLAIN(" ") ","
		"{\"@type\":\"richTextItalic\",\"text\":" PLAIN("leaning") "},"
		"{\"@type\":\"richTextIcon\",\"document\":null,\"width\":1,\"height\":1}"
		"]}}");
	CHECK(instant_view_open("Run code, old, new, (cup) x+1 leaning"));
	lv_obj_t *group = ui_test_find("Run code, old, new, (cup) x+1 leaning");
	CHECK(group && lv_obj_check_type(group, &lv_spangroup_class));
	if (!group || !lv_obj_check_type(group, &lv_spangroup_class))
		return;
	CHECK(lv_spangroup_get_span_count(group) >= 9);
	/* The fixed-width run in monospace, struck and underlined ones decorated */
	lv_span_t *code = lv_spangroup_get_child(group, 1), *old = lv_spangroup_get_child(group, 3);
	lv_span_t *added = lv_spangroup_get_child(group, 5);
	lv_style_value_t v;
	CHECK(code && lv_style_get_prop(lv_span_get_style(code), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND &&
	      v.ptr == &ui_font_mono_20);
	CHECK(old && lv_style_get_prop(lv_span_get_style(old), LV_STYLE_TEXT_DECOR, &v) == LV_STYLE_RES_FOUND &&
	      v.num == LV_TEXT_DECOR_STRIKETHROUGH);
	CHECK(added && lv_style_get_prop(lv_span_get_style(added), LV_STYLE_TEXT_DECOR, &v) == LV_STYLE_RES_FOUND &&
	      v.num == LV_TEXT_DECOR_UNDERLINE);
}

/* Blocks nested deeper than it draws are left out */
TEST(instant_view_too_deep)
{
	char blocks[4096] = "";
	/* 7 quotes deep, then 8 */
	for (int depth = 7; depth <= 8; depth++) {
		size_t n = strlen(blocks);
		if (n)
			blocks[n++] = ',';
		for (int i = 0; i < depth; i++)
			n += snprintf(blocks + n, sizeof(blocks) - n, "{\"@type\":\"pageBlockBlockQuote\",\"blocks\":[");
		n += snprintf(blocks + n, sizeof(blocks) - n, "%s", depth == 7 ? PARAGRAPH("Deep") : PARAGRAPH("Too deep"));
		for (int i = 0; i < depth; i++)
			n += snprintf(blocks + n, sizeof(blocks) - n, "],\"credit\":" PLAIN("") "}");
	}
	coffee_blocks(blocks);
	CHECK(instant_view_open("Deep"));
	CHECK(!ui_test_shows("Too deep"));
}

/* Photos: one smaller than the screen at its size, already downloaded; one without sizes
 * is not drawn */
TEST(instant_view_small_photo)
{
	photo_write("/var/lib/telegram/small.jpg", 320, 240);
	coffee_blocks(
		"{\"@type\":\"pageBlockPhoto\",\"photo\":{\"@type\":\"photo\",\"has_stickers\":false,\"sizes\":["
		"{\"@type\":\"photoSize\",\"type\":\"m\",\"width\":320,\"height\":240,\"photo\":{\"@type\":\"file\","
		"\"id\":710,\"local\":{\"@type\":\"localFile\",\"path\":\"/var/lib/telegram/small.jpg\","
		"\"is_downloading_completed\":true}}}]},\"caption\":" CAPTION("Small one") ",\"url\":\"\"},"
		"{\"@type\":\"pageBlockPhoto\",\"photo\":{\"@type\":\"photo\",\"sizes\":[]},\"caption\":"
		CAPTION("No photo") ",\"url\":\"\"}");
	mara_links(1048576, 2);
	CHECK(open_mara("Exploring the science behind adding salt to coffee"));
	unsigned before = photos_shown();
	CHECK(ui_test_tap_text("Instant View"));
	CHECK(ui_test_wait_for("Small one", 1000));
	CHECK(ui_test_shows("No photo"));
	CHECK_INT(photos_shown(), before + 1);
	CHECK_INT(ui_test_td_request_count("downloadFile"), 0);
	lv_obj_t *found[4];
	unsigned n = ui_test_find_widgets(&lv_3dtexture_class, found, 4);
	CHECK(n >= 1 && lv_obj_get_width(found[n - 1]) == 320);
}

/* The profile and contacts, more */

/* Spaces around a name are left out */
TEST(edit_name_trimmed)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Name"));
	CHECK(clear_field("Elian"));
	CHECK(ui_test_type("  Eli  "));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(ui_test_wait_for("Eli Doran", 1000));
	CHECK(requested("setName", "\"first_name\":\"Eli\""));
}

/* Initials in capitals */
TEST(profile_initials_upper_case)
{
	ui_test_td_account("elian", "doran", "elian", "40712345678");
	ui_test_td_bio("Feature phones are back");
	CHECK(open_profile());
	CHECK(ui_test_shows("ED"));
}

/* The same birthday: nothing sent */
TEST(edit_birthday_unchanged)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Birthday"));
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	CHECK(!ui_test_shows("Day"));
	CHECK(ui_test_shows("14 March 1990"));
	CHECK_INT(ui_test_td_request_count("setBirthdate"), 0);
}

/* Enter in the last field saves; the keyboard's hide key hides it */
TEST(edit_birthday_with_enter)
{
	account();
	CHECK(open_profile());
	CHECK(ui_test_tap_text("Birthday"));
	CHECK(ui_test_tap_field("14"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(ui_test_textarea() == NULL);
	CHECK(clear_field("1990"));
	CHECK(ui_test_type("1991\n"));
	CHECK(ui_test_wait_for("14 March 1991", 1000));
	CHECK(requested("setBirthdate", "\"year\":1991"));
}

/* Refused: no photo */
TEST(profile_photo_refused)
{
	account();
	photo_write(PHOTOS "/a.jpg", 320, 240);
	ui_test_td_fail("setProfilePhoto", 400, "PHOTO_INVALID_DIMENSIONS");
	CHECK(open_profile());
	CHECK(ui_test_tap_text("ED"));
	CHECK(ui_test_wait_for("Profile photo", 1000));
	lv_obj_t *thumb;
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, &thumb, 1), 1);
	ui_test_tap(thumb);
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	ui_test_wait(500);
	CHECK_INT(ui_test_td_request_count("setProfilePhoto"), 1);
	CHECK(ui_test_shows("Feature phones are back"));
	CHECK_INT(photos_shown(), 0);
	CHECK(ui_test_shows("ED"));
}

/* Not loaded: still loading, asked again the next time the app opens */
TEST(contacts_failed)
{
	account();
	ui_test_td_contact(101, "Mara", "Ionescu", NULL, NULL);
	ui_test_td_fail("getContacts", 500, "Internal Server Error");
	CHECK(open_contacts());
	ui_test_wait(500);
	CHECK(ui_test_shows("Loading contacts..."));
	CHECK(!ui_test_shows("No contacts"));
	CHECK(ui_test_back());
	CHECK(open_contacts());
	CHECK(ui_test_wait_for("Mara Ionescu", 1000));
	CHECK_INT(ui_test_td_request_count("getContacts"), 2);
}
