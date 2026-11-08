/*
 * Email (apps/email), the plugin app: email.so built for the host (EMAIL_APP, the
 * Makefile's), installed in /var/lib/apps/email and loaded by phone-ui (external.c)
 * as on the phone. Setting up the account: the provider's servers looked up in
 * Thunderbird's database (fake_curl.c) and the password tried on the IMAP server
 * (fake_imap.c, a server on the sandbox's loopback), the account kept in /var/lib/app-data,
 * removed again. The inbox: its last messages listed, kept, refreshed.
 */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ui.h"
#include "ui_symbols.h"
#include "ui_test.h"

#define APP "/var/lib/apps/email"
#define ACCOUNT_FILE "/var/lib/app-data/email/account.json"
#define FOLDERS_FILE "/var/lib/app-data/email/folders.json"
#define FOLDER_FILE "/var/lib/app-data/email/folder"
#define INBOX_FILE "/var/lib/app-data/email/folders/INBOX.json"
#define OLD_INBOX_FILE "/var/lib/app-data/email/inbox.json" /* an earlier version's */
#define BODIES "/var/lib/app-data/email/bodies/INBOX"
#define OLD_BODIES "/var/lib/app-data/email/bodies"
#define AUTOCONFIG "https://autoconfig.thunderbird.net/v1.1/"
#define IMAP "imap://127.0.0.1:10143/" /* the fake server, on the sandbox's loopback */
#define ADDRESS "anna@example.com"
#define PASSWORD "s3cret"
#define NOW 1794063600 /* Saturday 7 November 2026, 15:00 UTC */
#define WAIT 2000

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

/* As `just app email` installs it */
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "") || !copy(EMAIL_APP "/app.json", APP "/app.json") ||
	    !copy(EMAIL_APP "/email.so", APP "/email.so"))
		TEST_FAIL("cannot install %s", EMAIL_APP);
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	static char buf[65536]; /* a full inbox cache */
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Thunderbird's entry for example.com (autoconfig.thunderbird.net): POP3 listed before
 * IMAP, the IMAP server the fake one on the loopback address (plain, as only there), the
 * SMTP one with STARTTLS (taken on the TLS port) */
static const char autoconfig[] =
	"<?xml version=\"1.0\"?>\n"
	"<clientConfig version=\"1.1\">\n"
	"  <emailProvider id=\"example.com\">\n"
	"    <domain>example.com</domain>\n"
	"    <displayName>Example Mail</displayName>\n"
	"    <incomingServer type=\"pop3\">\n"
	"      <hostname>pop.example.com</hostname>\n"
	"      <port>995</port>\n"
	"      <socketType>SSL</socketType>\n"
	"      <authentication>password-cleartext</authentication>\n"
	"      <username>%EMAILADDRESS%</username>\n"
	"    </incomingServer>\n"
	"    <incomingServer type=\"imap\">\n"
	"      <hostname>127.0.0.1</hostname>\n"
	"      <port>10143</port>\n"
	"      <socketType>plain</socketType>\n"
	"      <authentication>password-cleartext</authentication>\n"
	"      <username>%EMAILADDRESS%</username>\n"
	"    </incomingServer>\n"
	"    <outgoingServer type=\"smtp\">\n"
	"      <hostname>smtp.example.com</hostname>\n"
	"      <port>587</port>\n"
	"      <socketType>STARTTLS</socketType>\n"
	"      <authentication>password-cleartext</authentication>\n"
	"      <username>%EMAILADDRESS%</username>\n"
	"    </outgoingServer>\n"
	"  </emailProvider>\n"
	"</clientConfig>\n";

static const char account_json[] =
	"{\"address\": \"" ADDRESS "\", \"user\": \"" ADDRESS "\", \"password\": \"" PASSWORD "\","
	" \"imap\": \"" IMAP "\", \"smtp\": \"smtps://smtp.example.com:465/\"}\n";

/* An inbox cache of one message, the UIDs the fake server's */
static const char inbox_json[] =
	"{\"uidvalidity\": 1703602304, \"more\": false, \"mails\": [{\"uid\": 90, \"date\": 1793000000, \"seen\": true,"
	" \"from\": \"Old Sender\", \"address\": \"old@example.com\", \"subject\": \"From the cache\"}]}\n";
/* As the version before folders kept it */
static const char old_inbox_json[] =
	"[{\"uid\": 90, \"date\": 1793000000, \"seen\": true, \"from\": \"Old Sender\","
	" \"address\": \"old@example.com\", \"subject\": \"From the cache\"}]\n";

static void known_provider(void)
{
	ui_test_serve(AUTOCONFIG "example.com", 200, "text/xml", autoconfig);
}

/* Three messages, the newest last as the server lists them: a subject in UTF-8 and one
 * in Latin-1 (RFC 2047), a sender without a name, one unread */
static void three_messages(void)
{
	ui_test_imap_message("GitHub", "noreply@github.com", "Build failed on main", "05-Nov-2026 09:30:00 +0000", true);
	ui_test_imap_message(NULL, "marie@example.fr", "=?ISO-8859-1?Q?Caf=E9_du_matin?=", "20-Dec-2025 18:00:00 +0000",
	                     true);
	ui_test_imap_message("Ion Popescu", "ion@example.ro", "=?UTF-8?B?QnVuxIM=?= ziua", "07-Nov-2026 14:05:33 +0100",
	                     false);
}

/* The form filled in and sent with Enter on the password */
static bool sign_in(const char *address, const char *password)
{
	if (!ui_test_tap_field("Email address") || !ui_test_type(address))
		return false;
	if (!ui_test_tap_field("Password"))
		return false;
	return ui_test_type(password) && ui_test_type("\n");
}

TEST(asks_for_an_account)
{
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Your email account"));
	CHECK(ui_test_tap_field("Email address"));
	CHECK(ui_test_tap_field("Password"));
	CHECK(ui_test_shows("Sign in"));
	CHECK(!ui_test_shows("IMAP server"));
	CHECK_INT((int)ui_test_request_count(), 0);
}

/* The servers from Thunderbird's database, the password tried on the IMAP one, kept;
 * then the inbox, empty */
TEST(signs_in)
{
	known_provider();
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("No messages", WAIT));
	CHECK(!ui_test_shows("Your email account"));
	CHECK(ui_test_imap_logins() >= 1);
	CHECK(strstr(ui_test_imap_commands(), "LOGIN "));
	CHECK(strstr(ui_test_imap_commands(), "SELECT INBOX\n"));

	const char *saved = read_file(ACCOUNT_FILE);
	CHECK(saved);
	if (!saved)
		return;
	CHECK(strstr(saved, "\"address\": \"" ADDRESS "\""));
	CHECK(strstr(saved, "\"user\": \"" ADDRESS "\""));
	CHECK(strstr(saved, "\"password\": \"" PASSWORD "\""));
	CHECK(strstr(saved, "\"imap\": \"" IMAP "\""));
	CHECK(strstr(saved, "\"smtp\": \"smtps://smtp.example.com:465/\""));
}

TEST(password_refused)
{
	known_provider();
	ui_test_imap_server(IMAP, ADDRESS, "another one");
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, "wrong"));
	CHECK(ui_test_wait_for("127.0.0.1 refused the password.", WAIT));
	CHECK(ui_test_shows("Sign in"));
	CHECK(!read_file(ACCOUNT_FILE));
	CHECK_INT((int)ui_test_imap_logins(), 0);
}

TEST(server_unreachable)
{
	known_provider();
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
	CHECK(!read_file(ACCOUNT_FILE));
}

/* A server that answers, then hangs up when it is sent the password */
TEST(server_hangs_up)
{
	known_provider();
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_refuse("LOGIN", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
	CHECK(ui_test_shows("Sign in"));
	CHECK(!read_file(ACCOUNT_FILE));
}

TEST(offline)
{
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("No internet connection.", WAIT));
	CHECK(!ui_test_shows("IMAP server"));
}

/* A provider the database does not know: its servers asked, ports as usual unless given */
TEST(unknown_provider_asks_for_servers)
{
	ui_test_serve(AUTOCONFIG "example.org", 404, "text/html", "Not found");
	ui_test_imap_server(IMAP, "bob@example.org", PASSWORD);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in("bob@example.org", PASSWORD));
	CHECK(ui_test_wait_for("No settings are known for example.org. Enter its IMAP and SMTP servers.", WAIT));
	CHECK(ui_test_tap_field("IMAP server"));
	CHECK(ui_test_type("127.0.0.1:10143"));
	CHECK(ui_test_tap_field("SMTP server"));
	CHECK(ui_test_type("mail.example.org:2525\n"));
	CHECK(ui_test_wait_for("No messages", WAIT));
	CHECK(ui_test_imap_logins() >= 1);
	const char *saved = read_file(ACCOUNT_FILE);
	CHECK(saved);
	if (!saved)
		return;
	CHECK(strstr(saved, "\"imap\": \"" IMAP "\""));
	CHECK(strstr(saved, "\"smtp\": \"smtps://mail.example.org:2525/\""));
}

TEST(fields_needed)
{
	known_provider();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text("Sign in"));
	CHECK(ui_test_shows("Enter your email address."));
	CHECK(ui_test_tap_field("Email address"));
	CHECK(ui_test_type("anna\n")); /* no domain */
	CHECK(ui_test_shows("Enter your email address."));
	CHECK(ui_test_type("@example.com\n"));
	CHECK(ui_test_shows("Enter your password."));
	CHECK_INT((int)ui_test_request_count(), 0);
}

/* Opened with an account: the inbox, its last messages asked for, newest first */
TEST(inbox_listed)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_wall_clock(NOW);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_shows("Bun\xc4\x83 ziua"));
	CHECK(ui_test_shows("13:05")); /* in the phone's time zone */
	CHECK(ui_test_shows("GitHub"));
	CHECK(ui_test_shows("Build failed on main"));
	CHECK(ui_test_shows("Thu"));
	CHECK(ui_test_shows("marie@example.fr")); /* no name */
	CHECK(ui_test_shows("Caf\xc3\xa9 du matin"));
	CHECK(ui_test_shows("20 Dec 2025"));
	CHECK(!ui_test_shows("Your email account"));
	CHECK(!ui_test_shows("No messages"));
	CHECK(strstr(ui_test_imap_commands(), "SELECT INBOX\n"));
	CHECK(strstr(ui_test_imap_commands(), "FETCH 1:3 (UID FLAGS INTERNALDATE ENVELOPE)\n"));

	/* Newest first: the rows, three levels up from their labels */
	lv_obj_t *newest = ui_test_find("Ion Popescu"), *oldest = ui_test_find("marie@example.fr");
	CHECK(newest && oldest &&
	      lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(newest)))) <
	          lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(oldest)))));

	/* Kept for the next time */
	const char *saved = read_file(INBOX_FILE);
	CHECK(saved && strstr(saved, "\"uid\": 103") && strstr(saved, "\"seen\": false") &&
	      strstr(saved, "\"subject\": \"Bun\xc4\x83 ziua\""));
}

/* Dovecot sends long and 8-bit strings as literals, which span lines */
TEST(inbox_with_literals)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_literals(true);
	three_messages();
	ui_test_imap_message("Ana-Maria \"Ami\" Pop", "ami@example.ro",
	                     "O \xc3\xaentrebare lung\xc4\x83 despre \"telefon\", cu ghilimele", "07-Nov-2026 14:30:00 +0100",
	                     false);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ana-Maria \"Ami\" Pop", WAIT));
	CHECK(ui_test_shows("O \xc3\xaentrebare lung\xc4\x83 despre \"telefon\", cu ghilimele"));
	CHECK(ui_test_shows("Ion Popescu"));
	CHECK(ui_test_shows("Bun\xc4\x83 ziua"));
	CHECK(ui_test_shows("Caf\xc3\xa9 du matin"));
}

/* Only the last 50 of a full inbox */
TEST(inbox_last_fifty)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 1231; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(strstr(ui_test_imap_commands(), "FETCH 1185:1234 (UID FLAGS INTERNALDATE ENVELOPE)\n"));
	lv_obj_t *rows[64];
	CHECK_INT((int)ui_test_find_all("Someone", rows, 64), 47);
}

/* That many rows of "Someone" shown (ui_test_wait_until()) */
static unsigned someone_wanted;
static bool someone_rows(void)
{
	lv_obj_t *rows[128];
	return ui_test_find_all("Someone", rows, 128) == someone_wanted;
}

/* A row at the end asks for the 50 before those shown: the server says how many are
 * below the oldest (SEARCH), the last 50 of them fetched and appended */
TEST(inbox_load_more)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 1231; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_shows("Load more"));
	CHECK(ui_test_tap_text("Load more"));
	CHECK(ui_test_wait_for("Load more", WAIT)); /* Loading... meanwhile */
	someone_wanted = 97;
	CHECK(ui_test_wait_until(someone_rows, WAIT));
	lv_obj_t *rows[128];
	CHECK_INT((int)ui_test_find_all("Someone", rows, 128), 97);
	CHECK(strstr(ui_test_imap_commands(), "SEARCH UID 1:1284\n"));
	CHECK(strstr(ui_test_imap_commands(), "FETCH 1135:1184 (UID FLAGS INTERNALDATE ENVELOPE)\n"));
	/* Below those shown, the newest at the top still */
	lv_obj_t *newest = ui_test_find("Ion Popescu");
	CHECK(newest && lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(newest)))) <
	                    lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(rows[96])))));
	CHECK(ui_test_shows("Load more")); /* more below */
	/* Kept, as the others */
	const char *saved = read_file(INBOX_FILE);
	CHECK(saved && strstr(saved, "\"uid\": 1235,") && !strstr(saved, "\"uid\": 1234,"));
}

/* The first message reached: no more to load */
TEST(inbox_load_more_reaches_the_start)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 57; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Load more"));
	someone_wanted = 57;
	CHECK(ui_test_wait_until(someone_rows, WAIT));
	CHECK(strstr(ui_test_imap_commands(), "FETCH 1:10 (UID FLAGS INTERNALDATE ENVELOPE)\n"));
	CHECK(!ui_test_shows("Load more"));
	CHECK(!ui_test_shows("Loading..."));
}

/* A small inbox, all of it listed: nothing more to load */
TEST(inbox_whole_has_no_load_more)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!ui_test_shows("Load more"));
}

/* What was loaded stays below the newest when they are listed again (opened again,
 * from the cache at once, or pulled down) */
TEST(inbox_loaded_kept)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_message("First Sender", "first@example.com", "The very first", "01-Jan-2025 00:00:00 +0000", true);
	for (int i = 0; i < 56; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!ui_test_shows("The very first"));
	CHECK(ui_test_tap_text("Load more"));
	CHECK(ui_test_wait_for("The very first", WAIT));
	CHECK(ui_test_back());
	ui_test_imap_message("Late Sender", "late@example.com", "Just arrived", "07-Nov-2026 16:00:00 +0000", false);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("The very first")); /* the cache */
	CHECK(!ui_test_shows("Load more"));
	CHECK(ui_test_wait_for("Just arrived", WAIT)); /* the newest listed again */
	CHECK(ui_test_shows("The very first"));
	CHECK(ui_test_shows("Ion Popescu"));
	lv_obj_t *rows[64];
	CHECK_INT((int)ui_test_find_all("Someone", rows, 64), 56);
	CHECK(!ui_test_shows("Load more"));
	const char *saved = read_file(INBOX_FILE);
	CHECK(saved && strstr(saved, "\"more\": false") && strstr(saved, "\"uid\": 101,") && strstr(saved, "\"uid\": 161,"));
}

/* The server refusing: said in the row, which asks again when tapped */
TEST(inbox_load_more_refused)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 57; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	ui_test_imap_refuse("SEARCH", "NO Search failed");
	CHECK(ui_test_tap_text("Load more"));
	CHECK(ui_test_wait_for("127.0.0.1 refused the request.", WAIT));
	ui_test_imap_refuse(NULL, NULL);
	CHECK(ui_test_tap_text("127.0.0.1 refused the request."));
	someone_wanted = 57;
	CHECK(ui_test_wait_until(someone_rows, WAIT));
}

TEST(inbox_empty)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("No messages", WAIT));
	CHECK(!strstr(ui_test_imap_commands(), "FETCH"));
}

/* Offline: what was kept, and why nothing new */
TEST(inbox_offline_shows_the_cache)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(ui_test_shows("From the cache"));
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(!ui_test_shows("No messages"));
}

TEST(inbox_refreshed_by_pulling)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!ui_test_shows("Old Sender")); /* the server's list replaces the cache */
	ui_test_imap_message("Late Sender", "late@example.com", "Just arrived", "07-Nov-2026 16:00:00 +0000", false);
	ui_test_pull_down();
	CHECK(ui_test_wait_for("Just arrived", WAIT));
	CHECK(ui_test_shows("Ion Popescu"));
}

/* The account's screen, from the header: the address, removing it */
TEST(account_removed)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text(UI_SYMBOL_USER));
	CHECK(ui_test_shows(ADDRESS));
	CHECK(ui_test_tap_text("Remove account"));
	CHECK(ui_test_shows("Cancel"));
	CHECK(ui_test_tap_text("Remove"));
	CHECK(ui_test_wait_for("Your email account", WAIT));
	CHECK(!read_file(ACCOUNT_FILE));
	CHECK(!read_file(INBOX_FILE));
	CHECK(ui_test_back());
	/* Gone for good: opened again, the form */
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Your email account"));
}

/* Reading */

static bool html_shown(void)
{
	return ui_test_find_widget(&lv_3dtexture_class) != NULL;
}

static bool cache_says_103_read(void)
{
	const char *saved = read_file(INBOX_FILE);
	return saved && strstr(saved, "\"uid\": 103, \"date\": 1794056733, \"seen\": true");
}

/* A message with plain text only: its subject, sender, date and text; asked for by
 * section, marked read on the server and in the list */
TEST(message_plain_text)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body("Salut Anna,\n\nNe vedem m\xc3\xa2ine la 10?\n\nIon", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_for("Salut Anna,\n\nNe vedem m\xc3\xa2ine la 10?\n\nIon", WAIT));
	CHECK(ui_test_shows("Bun\xc4\x83 ziua"));
	CHECK(ui_test_shows("Ion Popescu <ion@example.ro>"));
	CHECK(ui_test_shows("7 Nov 2026, 13:05"));
	CHECK(strstr(ui_test_imap_commands(), "UID FETCH 103 (BODYSTRUCTURE)\nUID FETCH 103 (BODY.PEEK[1])\n"
	                                       "UID STORE 103 +FLAGS (\\Seen)\n"));
	/* Kept, and read now, once the server took the mark */
	CHECK(read_file(BODIES "/103.txt"));
	CHECK(ui_test_wait_until(cache_says_103_read, WAIT));
	CHECK(ui_test_back());
}

/* With an HTML part too, the page: colors drawn, the plain text not shown */
TEST(message_html)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write("/var/lib/phone-ui/settings", "light=1\n"); /* its colors as they are */
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body("The plain version", "<p>Hello</p><div style=\"background: #ff0000; height: 60px\"></div>"
	                                       "<div style=\"background: #00ff00; height: 60px\"></div>");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_until(html_shown, WAIT));
	ui_test_wait(100); /* a frame with it drawn */
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (!page)
		return;
	lv_obj_t *html = lv_obj_get_parent(page);
	CHECK(ui_test_shows_color(html, 0xff0000));
	CHECK(ui_test_shows_color(html, 0x00ff00));
	CHECK(!ui_test_shows_part("The plain version"));
	CHECK(strstr(ui_test_imap_commands(), "UID FETCH 103 (BODY.PEEK[2])\n"));
	CHECK(read_file(BODIES "/103.html"));
}

TEST(message_without_text)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_for("This message has no text.", WAIT));
}

/* Read before: from the copy kept, offline too */
TEST(message_from_the_copy_kept)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_write(BODIES "/90.txt", "The body kept on the phone");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text("Old Sender"));
	CHECK(ui_test_shows("The body kept on the phone"));
	CHECK(ui_test_shows("From the cache"));
	CHECK(!ui_test_shows_part("Cannot reach")); /* nothing to ask: it was read already */
}

/* A message that goes from the inbox takes its kept body with it */
TEST(kept_bodies_follow_the_inbox)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(BODIES "/90.txt", "An old body");
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!read_file(BODIES "/90.txt"));
}

/* A body kept from a real message, EMAIL_BODY=<file> (HTML by its name; a path the
 * sandbox sees, under tests/build/), shown and screenshotted in both themes:
 * build/email-body-dark.png and -light.png. For looking at how real mail renders;
 * nothing without the variable. */
TEST(render_a_kept_body)
{
	const char *path = getenv("EMAIL_BODY");
	if (!path)
		return;
	FILE *f = fopen(path, "r");
	CHECK(f);
	if (!f)
		return;
	static char body[1 << 20];
	size_t n = fread(body, 1, sizeof(body) - 1, f);
	body[n] = '\0';
	fclose(f);
	bool html = strstr(path, ".html") != NULL;
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_write(html ? BODIES "/90.html" : BODIES "/90.txt", body);
	const char *themes[] = {"dark", "light"};
	for (int i = 0; i < 2; i++) {
		if (i) {
			ui_set_light(true, NULL);
			ui_test_wait(1000);
		}
		CHECK(ui_test_open("Email"));
		CHECK(ui_test_tap_text("Old Sender"));
		if (html)
			CHECK(ui_test_wait_until(html_shown, WAIT));
		ui_test_wait(500);
		char out[64];
		snprintf(out, sizeof(out), "build/email-body-%s.png", themes[i]);
		CHECK(ui_test_screenshot(out));
		/* Dragged up: does it scroll? */
		lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
		if (page) {
			lv_obj_t *html_obj = lv_obj_get_parent(page), *list = lv_obj_get_parent(lv_obj_get_parent(html_obj));
			lv_area_t a;
			lv_obj_get_coords(html_obj, &a);
			ui_test_swipe(360, 1300, 360, 500, 300);
			printf("render: html %d x %d at y %d, scroll right %d; after a drag up the list is at %d, the page at x %d\n",
			       (int)lv_obj_get_width(html_obj), (int)lv_obj_get_height(html_obj), (int)a.y1,
			       (int)lv_obj_get_scroll_right(html_obj), (int)lv_obj_get_scroll_y(list), (int)lv_obj_get_scroll_x(html_obj));
		}
		CHECK(ui_test_back());
		CHECK(ui_test_back());
	}
}

/* In the dark theme (the default), the page is drawn dark: its white the card's color,
 * nothing white left */
TEST(message_html_dark)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body(NULL, "<div style=\"background: #ffffff; height: 200px\"></div>"); /* no text: its ink is white */
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_until(html_shown, WAIT));
	ui_test_wait(100); /* a frame with it drawn */
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (!page)
		return;
	lv_obj_t *html = lv_obj_get_parent(page);
	CHECK(!ui_light);
	CHECK(!ui_test_shows_color(html, 0xffffff));
	CHECK(ui_test_shows_color(html, UI_COLOR_CARD));
}

/* A page laid out wider than the card (a word litehtml cannot break, a long URL as text) is
 * scaled down to fit it, not scrolled sideways; a table's width attribute is capped by the
 * stylesheet already. Dragged up and down, the message scrolls as a whole */
TEST(wide_message_fits_the_card)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write("/var/lib/phone-ui/settings", "light=1\n"); /* its red as it is */
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	static char html[8192];
	size_t n = (size_t)snprintf(html, sizeof(html), "<table width=\"900\"><tr><td>Wide</td></tr></table>"
	                                                  "<table width=\"600\"><tr><td>https://example.org/a-very-long-address-that-no-browser-would-break-at-all"
	                                                  "<span style=\"background: #ff0000\">END</span></td></tr></table>");
	for (int i = 0; i < 60 && n < sizeof(html) - 64; i++)
		n += (size_t)snprintf(html + n, sizeof(html) - n, "<p>Line %d of the message</p>", i);
	ui_test_imap_body(NULL, html);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_until(html_shown, WAIT));
	ui_test_wait(100);
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (!page)
		return;
	lv_obj_t *html_obj = lv_obj_get_parent(page);
	lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(html_obj));
	CHECK_INT((int)lv_obj_get_scroll_right(html_obj), 0); /* fits */
	CHECK_INT((int)lv_obj_get_width(html_obj), 720 - 2 * 16 - 2 * 12);
	CHECK(lv_obj_get_width(page) == lv_obj_get_width(html_obj)); /* the quads scaled to it */
	CHECK(ui_test_shows_color(html_obj, 0xff0000)); /* the table's right edge in view */
	lv_area_t a;
	lv_obj_get_coords(html_obj, &a);
	ui_test_swipe(360, LV_MIN(a.y2, 1300), 360, LV_MAX(a.y1, 400), 300);
	CHECK(lv_obj_get_scroll_y(list) > 0);
}

/* A long message of plain text, nothing in it clickable: dragged up, it scrolls */
TEST(tall_message_scrolls)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	static char text[4096];
	size_t n = 0;
	for (int i = 0; i < 80 && n < sizeof(text) - 64; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n, "Line %d of the message\n", i);
	ui_test_imap_body(text, NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_for(text, WAIT));
	lv_obj_t *label = ui_test_find(text);
	CHECK(label);
	if (!label)
		return;
	lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(label));
	ui_test_swipe(360, 1300, 360, 500, 300);
	CHECK(lv_obj_get_scroll_y(list) > 0);
}

/* Setting up, as providers and people do it */

/* Thunderbird's entry with the user name as the address's local part, no SMTP server and
 * no port, its text spread over lines: signed in as "anna", sending left for later */
TEST(provider_with_local_part_and_no_smtp)
{
	ui_test_serve(AUTOCONFIG "example.com", 200, "text/xml",
	              "<clientConfig version=\"1.1\"><emailProvider id=\"example.com\">\n"
	              "  <incomingServer type=\"imap\">\n"
	              "    <hostname>\n      127.0.0.1\n    </hostname>\n"
	              "    <port>10143</port>\n"
	              "    <socketType>plain</socketType>\n"
	              "    <username>%EMAILLOCALPART%</username>\n"
	              "  </incomingServer>\n"
	              "</emailProvider></clientConfig>\n");
	ui_test_imap_server(IMAP, "anna", PASSWORD);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("No messages", WAIT));
	CHECK(strstr(ui_test_imap_commands(), "LOGIN anna "));
	const char *saved = read_file(ACCOUNT_FILE);
	CHECK(saved && strstr(saved, "\"user\": \"anna\"") && strstr(saved, "\"imap\": \"" IMAP "\"") &&
	      strstr(saved, "\"smtp\": \"\""));
}

/* The servers asked for: blank ones are not tried, the field left blank gets the keyboard */
TEST(servers_needed)
{
	ui_test_serve(AUTOCONFIG "example.org", 404, "text/html", "Not found");
	CHECK(ui_test_open("Email"));
	CHECK(sign_in("bob@example.org", PASSWORD));
	CHECK(ui_test_wait_for("No settings are known for example.org. Enter its IMAP and SMTP servers.", WAIT));
	CHECK(ui_test_tap_field("IMAP server"));
	CHECK(ui_test_type("  \n"));
	CHECK(ui_test_shows("Enter the mail servers."));
	CHECK(ui_test_type("mail.example.org\n"));
	CHECK(ui_test_shows("Enter the mail servers."));
	lv_obj_t *ta = ui_test_textarea();
	CHECK(ta && strcmp(lv_textarea_get_placeholder_text(ta), "SMTP server") == 0);
	CHECK(!ui_test_requested("imaps://*"));
}

/* The keyboard's own key hides it */
TEST(keyboard_hidden_by_its_key)
{
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_field("Email address"));
	CHECK(ui_test_textarea());
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	CHECK(!ui_test_textarea());
}

/* Signed in, but nowhere to keep the account (the data partition broken): said so, still
 * on the form */
TEST(account_not_saved)
{
	known_provider();
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	CHECK(ui_test_write("/var/lib/app-data/email", "not a folder"));
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("The account could not be saved.", WAIT));
	CHECK(ui_test_shows("Sign in"));
}

/* The same when the new file cannot replace the old one */
TEST(account_not_saved_over_a_folder)
{
	known_provider();
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	CHECK(ui_test_write(ACCOUNT_FILE "/in-the-way", ""));
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Your email account"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("The account could not be saved.", WAIT));
}

/* An account kept without its IMAP server is of no use: set up again */
TEST(incomplete_account_set_up_again)
{
	ui_test_write(ACCOUNT_FILE, "{\"address\": \"" ADDRESS "\", \"password\": \"" PASSWORD "\"}\n");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Your email account"));
	CHECK_INT((int)ui_test_request_count(), 0);
}

TEST(account_removal_cancelled)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text(UI_SYMBOL_USER));
	CHECK(ui_test_shows("127.0.0.1"));
	CHECK(ui_test_tap_text("Remove account"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows("Cancel"));
	CHECK(read_file(ACCOUNT_FILE));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Old Sender"));
}

/* The inbox when the server says no */

static void inbox_with_cache(void)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
}

TEST(inbox_select_refused)
{
	inbox_with_cache();
	ui_test_imap_refuse("SELECT", "NO [INUSE] Mailbox is locked");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("127.0.0.1 refused the request.", WAIT));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(!strstr(ui_test_imap_commands(), "FETCH"));
}

TEST(inbox_fetch_refused)
{
	inbox_with_cache();
	ui_test_imap_refuse("FETCH", "BAD Internal error");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("127.0.0.1 refused the request.", WAIT));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(!ui_test_shows("Ion Popescu"));
}

/* A server with no room for one more connection greets with BYE */
TEST(inbox_server_says_bye)
{
	inbox_with_cache();
	ui_test_imap_greeting_text("* BYE Too many connections from your address");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
	CHECK(!strstr(ui_test_imap_commands(), "LOGIN"));
}

/* Logged in already by the connection itself (PREAUTH): no LOGIN */
TEST(inbox_preauthenticated)
{
	inbox_with_cache();
	ui_test_imap_greeting_text("* PREAUTH IMAP4rev1 server logged in as anna");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!strstr(ui_test_imap_commands(), "LOGIN"));
	CHECK(strstr(ui_test_imap_commands(), "SELECT INBOX\n"));
}

/* An account with a server in clear: the password never goes there */
TEST(inbox_never_in_clear)
{
	ui_test_write(ACCOUNT_FILE, "{\"address\": \"" ADDRESS "\", \"user\": \"" ADDRESS "\", \"password\": \"" PASSWORD
	                            "\", \"imap\": \"imap://imap.example.com:143/\", \"smtp\": \"\"}\n");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Cannot reach imap.example.com.", WAIT));
	CHECK_INT((int)ui_test_request_count(), 0);
}

/* Senders and subjects as mail has them: a name with quotes (escaped in a quoted string),
 * no sender at all, several (the first shown), a charset with a language (RFC 2231),
 * "=?" that starts no encoded word */
TEST(inbox_senders_and_subjects)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_message("Ana \"Ami\" Pop", "ami@example.ro", "Quoted", "07-Nov-2026 10:00:00 +0000", true);
	ui_test_imap_message(NULL, "", "Without a sender", "07-Nov-2026 10:01:00 +0000", true);
	ui_test_imap_message("Team", "lead@example.com, second@example.com", "From two", "07-Nov-2026 10:02:00 +0000", true);
	ui_test_imap_message("Ion", "ion@example.ro", "=?UTF-8*ro?Q?Bun=C4=83_diminea=C8=9Ba?=", "07-Nov-2026 10:03:00 +0000",
	                     true);
	ui_test_imap_message("Shop", "shop@example.com", "Price =?? 5 euro", "07-Nov-2026 10:04:00 +0000", true);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ana \"Ami\" Pop", WAIT));
	CHECK(ui_test_shows("Without a sender"));
	CHECK(ui_test_shows("Team"));
	CHECK(ui_test_shows("From two"));
	CHECK(ui_test_shows("Bun\xc4\x83 diminea\xc8\x9b" "a"));
	CHECK(ui_test_shows("Price =?? 5 euro"));
	CHECK(ui_test_tap_text("Team"));
	CHECK(ui_test_shows("Team <lead@example.com>"));
}

/* A subject longer than kept: cut between characters, not inside one (a stray byte of a
 * cut "ă" shows as a box) */
TEST(long_subject_cut_between_characters)
{
	static char odd[256], even[256], odd_cut[256], even_cut[256];
	size_t n = 0;
	for (int i = 0; i < 90; i++, n += 2)
		memcpy(odd + n, "\xc4\x83", 2); /* 159 bytes kept: 79 of them and half of one */
	memcpy(odd_cut, odd, 158);
	even[0] = 'a';
	memcpy(even + 1, odd, n); /* 159 bytes kept: "a" and 79 of them, whole */
	memcpy(even_cut, even, 159);
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_message("Odd", "odd@example.com", odd, "07-Nov-2026 10:00:00 +0000", true);
	ui_test_imap_message("Even", "even@example.com", even, "07-Nov-2026 10:01:00 +0000", true);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Odd", WAIT));
	CHECK(ui_test_shows(odd_cut));
	CHECK(ui_test_shows(even_cut));
}

/* Reading, when it does not go well */

/* Not read before, the server unreachable: why nothing shows */
TEST(message_offline)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text("Old Sender"));
	CHECK(ui_test_shows("From the cache"));
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
}

/* Each step of reading refused, or the server hanging up on it: why nothing shows */
TEST(message_refused_or_hung_up)
{
	static const struct {
		const char *command, *answer;
	} steps[] = {
		{ "UID FETCH 103 (BODYSTRUCTURE)", "NO Server busy" },
		{ "UID FETCH 103 (BODY.PEEK", "BAD Internal error" },
		{ "UID FETCH 103 (BODY.PEEK", NULL },
	};
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body("The text", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	for (unsigned i = 0; i < sizeof(steps) / sizeof(*steps); i++) {
		ui_test_imap_refuse(steps[i].command, steps[i].answer);
		CHECK(ui_test_tap_text("Ion Popescu"));
		if (!ui_test_wait_for(steps[i].answer ? "127.0.0.1 refused the request." : "Cannot reach 127.0.0.1.", WAIT))
			TEST_FAIL("step %u: %s", i, steps[i].command);
		CHECK(!ui_test_shows("The text"));
		CHECK(ui_test_back());
	}
	CHECK(!strstr(ui_test_imap_commands(), "UID STORE"));
}

static bool marked_read(void)
{
	return strstr(ui_test_imap_commands(), "UID STORE 103 +FLAGS (\\Seen)\n") != NULL;
}

/* Kept from before but still unread (read offline): shown at once, only marked read */
TEST(message_kept_marked_read)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(BODIES "/103.txt", "Kept on the phone");
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_shows("Kept on the phone"));
	CHECK(ui_test_wait_until(marked_read, WAIT));
	CHECK(!strstr(ui_test_imap_commands(), "BODYSTRUCTURE"));
}

/* Read on another device: fetched, not marked again; a sender without a name is their
 * address alone */
TEST(message_read_elsewhere)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_message(NULL, "marie@example.fr", "Bonjour", "05-Nov-2026 09:30:00 +0000", true);
	ui_test_imap_body("Salut !", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("marie@example.fr", WAIT));
	CHECK(ui_test_tap_text("marie@example.fr"));
	CHECK(ui_test_wait_for("Salut !", WAIT));
	CHECK(ui_test_shows("marie@example.fr"));
	CHECK(!ui_test_shows_part("<"));
	ui_test_wait(300);
	CHECK(!strstr(ui_test_imap_commands(), "UID STORE"));
}

/* A very long text: its first 64 KB shown */
TEST(message_long_text_cut)
{
	static char text[70000], cut[65537];
	size_t n = 0;
	for (int i = 0; n < sizeof(text) - 40; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n, "Line %d of a long message\n", i);
	memcpy(cut, text, 65536);
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body(text, NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_for(cut, WAIT));
}

/* Nowhere to keep it (the data partition broken): shown all the same */
TEST(message_shown_when_not_kept)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(BODIES, "not a folder");
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_body("Not kept", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text("Ion Popescu"));
	CHECK(ui_test_wait_for("Not kept", WAIT));
	CHECK(ui_test_wait_until(marked_read, WAIT));
}

/* Message 103's structure, its `section` sent as a literal of `data` (after `before`) */
static void raw_body(const char *structure, const char *before, const char *section, const char *data)
{
	static char fetched[4096];
	snprintf(fetched, sizeof(fetched), "%s* 3 FETCH (UID 103 BODY[%s] {%zu}\r\n%s)\r\n", before, section, strlen(data),
	         data);
	ui_test_imap_raw_body(structure, fetched);
}

static bool open_ion(void)
{
	return ui_test_open("Email") && ui_test_wait_for("Ion Popescu", WAIT) && ui_test_tap_text("Ion Popescu");
}

/* Latin-1 in quoted-printable: its escapes, soft line breaks (trailing spaces after the =
 * too) and an = that escapes nothing */
TEST(message_latin1_quoted_printable)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	raw_body("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"ISO-8859-1\") NIL NIL \"QUOTED-PRINTABLE\" 60 2 NIL NIL NIL NIL)", "",
	         "1", "Caf=E9 cr=E8me =\r\nbr=FBl=E9e, 1+1=3D2, =ZZ ok =  \r\nend");
	CHECK(open_ion());
	CHECK(ui_test_wait_for("Caf\xc3\xa9 cr\xc3\xa8me br\xc3\xbbl\xc3\xa9" "e, 1+1=2, =ZZ ok end", WAIT));
}

/* Windows-1252 in base64: its own characters (the euro sign), a NUL byte a space */
TEST(message_windows_1252_base64)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	raw_body("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"windows-1252\") NIL NIL \"BASE64\" 12 1 NIL NIL NIL NIL)", "", "1",
	         "UHJlgCA1AG9r"); /* "Pre\x80 5\0ok" */
	CHECK(open_ion());
	CHECK(ui_test_wait_for("Pre\xe2\x82\xac 5 ok", WAIT));
}

/* A mixed message: the text of its alternative part (numbered 1.1, an enriched one beside
 * it), an attachment after; the server's unasked flags update before the body passed over */
TEST(message_nested_parts)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	raw_body("(((\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 9 1 NIL NIL NIL NIL)"
	         "(\"TEXT\" \"ENRICHED\" (\"CHARSET\" \"us-ascii\") NIL NIL \"7BIT\" 20 1 NIL NIL NIL NIL) \"ALTERNATIVE\" "
	         "(\"BOUNDARY\" \"b2\") NIL NIL NIL)"
	         "(\"APPLICATION\" \"PDF\" (\"NAME\" \"bill.pdf\") NIL NIL \"BASE64\" 4000 NIL (\"ATTACHMENT\" "
	         "(\"FILENAME\" \"bill.pdf\")) NIL NIL) \"MIXED\" (\"BOUNDARY\" \"b1\") NIL NIL NIL)",
	         "* 3 FETCH (FLAGS (\\Seen))\r\n", "1.1", "Your bill");
	CHECK(open_ion());
	CHECK(ui_test_wait_for("Your bill", WAIT));
	CHECK(strstr(ui_test_imap_commands(), "UID FETCH 103 (BODY.PEEK[1.1])\n"));
}

/* A structure it cannot make sense of, or the body not sent: said so */
TEST(message_unreadable_structure)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	raw_body("((\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 9 1)(\"TEXT\")) \"MIXED\")", "", "1", "Hidden");
	CHECK(open_ion());
	CHECK(ui_test_wait_for("This message has no text.", WAIT));
	CHECK(!strstr(ui_test_imap_commands(), "BODY.PEEK"));
}

/* Responses a server garbles among the good ones: libetpan takes none of the listing then,
 * and the inbox says so; what was kept stays */
TEST(inbox_garbled_responses_said_so)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_fetch_extra("* 4 FETCH (UID (104))\r\n"
	                         "* 5 FETCH (UID 105 FLAGS \\Seen)\r\n");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("127.0.0.1 did not answer as a mail server does.", WAIT));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(!ui_test_shows("Ion Popescu"));
}

/* Kept from before, unread, the server unreachable: the copy shown, nothing said */
TEST(message_kept_offline)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, "[{\"uid\": 90, \"date\": 1793000000, \"seen\": false, \"from\": \"Old Sender\","
	                          " \"address\": \"old@example.com\", \"subject\": \"From the cache\"}]\n");
	ui_test_write(BODIES "/90.txt", "Read offline");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text("Old Sender"));
	CHECK(ui_test_shows("Read offline"));
	ui_test_wait(1000); /* the server tried for the mark meanwhile */
	CHECK(!ui_test_shows_part("Cannot reach"));
	CHECK(ui_test_shows("Read offline"));
}

TEST(message_body_not_sent)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_raw_body("(\"TEXT\" \"PLAIN\" NIL NIL NIL \"7BIT\" 9 1 NIL NIL NIL NIL)", "* 3 FETCH (FLAGS (\\Seen))\r\n");
	CHECK(open_ion());
	CHECK(ui_test_wait_for("This message has no text.", WAIT));
	CHECK(ui_test_wait_until(marked_read, WAIT));
}

/* Folders */

/* A server's folders besides the inbox: the special ones marked as Dovecot and Gmail
 * mark them, one of bills with a message unread, a nested one, one named in modified
 * UTF-7 (Călători), one that cannot be selected. Messages added after go to the last */
static void folders(void)
{
	ui_test_imap_folder("Sent", "\\Sent");
	ui_test_imap_folder("Drafts", "\\Drafts");
	ui_test_imap_folder("Trash", "\\Trash");
	ui_test_imap_folder("Junk", "\\Junk");
	ui_test_imap_folder("Archive", "\\Archive");
	ui_test_imap_folder("[Gmail]", "\\Noselect \\HasChildren");
	ui_test_imap_folder("Proiecte/Telefon", "");
	ui_test_imap_folder("C&AQM-l&AQM-tori", "");
	ui_test_imap_folder("Facturi", "");
	ui_test_imap_message("Digi", "facturi@digi.ro", "Factura Digi", "02-Nov-2026 09:00:00 +0000", true);
	ui_test_imap_message("Enel", "facturi@enel.ro", "Factura ta", "01-Nov-2026 09:00:00 +0000", false);
}

/* The row of a label's text, for their order */
static int32_t row_y(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	return label ? lv_obj_get_y(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(label)))) : -1;
}

/* The header's folder button: the server's folders, the inbox first, then the special
 * ones by their role, then the rest by name (decoded, a path with its parts apart),
 * those that cannot hold messages left out, with how many are unread */
TEST(folders_listed)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	folders();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_shows("Inbox")); /* the title */
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Facturi", WAIT));
	CHECK(ui_test_shows("Sent") && ui_test_shows("Drafts") && ui_test_shows("Trash") && ui_test_shows("Junk") &&
	      ui_test_shows("Archive"));
	CHECK(ui_test_shows("C\xc4\x83l\xc4\x83tori"));
	CHECK(ui_test_shows("Proiecte / Telefon"));
	CHECK(!ui_test_shows("[Gmail]"));
	CHECK(row_y("Inbox") < row_y("Sent") && row_y("Sent") < row_y("Drafts") && row_y("Drafts") < row_y("Trash") &&
	      row_y("Trash") < row_y("Junk") && row_y("Junk") < row_y("Archive"));
	CHECK(row_y("Archive") < row_y("C\xc4\x83l\xc4\x83tori") && row_y("C\xc4\x83l\xc4\x83tori") < row_y("Facturi") &&
	      row_y("Facturi") < row_y("Proiecte / Telefon"));
	/* Unread: one in the inbox, one of the bills */
	lv_obj_t *badges[8];
	CHECK_INT((int)ui_test_find_all("1", badges, 8), 2);
	CHECK(strstr(ui_test_imap_commands(), "LIST \"\" \"*\"\n"));
	CHECK(strstr(ui_test_imap_commands(), "STATUS Facturi (UNSEEN)\n"));
	CHECK(!strstr(ui_test_imap_commands(), "STATUS [Gmail]"));
	/* Kept for the next time */
	const char *saved = read_file(FOLDERS_FILE);
	CHECK(saved && strstr(saved, "\"name\": \"Facturi\"") && strstr(saved, "\"name\": \"C&AQM-l&AQM-tori\""));
}

/* A folder tapped: its messages, as the inbox's, under its name; the app opens on it
 * again, and its messages open and are kept apart from the inbox's */
TEST(folder_opened)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	folders();
	ui_test_imap_body("Plata pana pe 15.", NULL);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Facturi", WAIT));
	CHECK(ui_test_tap_text("Facturi"));
	CHECK(ui_test_wait_for("Enel", WAIT));
	CHECK(ui_test_shows("Factura ta") && ui_test_shows("Digi"));
	CHECK(!ui_test_shows("Ion Popescu"));
	CHECK(ui_test_shows("Facturi")); /* the title */
	CHECK(!ui_test_shows("Inbox"));
	CHECK(strstr(ui_test_imap_commands(), "SELECT Facturi\n"));
	CHECK(strstr(ui_test_imap_commands(), "FETCH 1:2 (UID FLAGS INTERNALDATE ENVELOPE)\n"));
	const char *saved = read_file("/var/lib/app-data/email/folders/Facturi.json");
	CHECK(saved && strstr(saved, "\"from\": \"Enel\""));
	saved = read_file(INBOX_FILE);
	CHECK(saved && strstr(saved, "\"from\": \"Ion Popescu\"") && !strstr(saved, "Enel"));
	/* Its message */
	CHECK(ui_test_tap_text("Enel"));
	CHECK(ui_test_wait_for("Plata pana pe 15.", WAIT));
	CHECK(read_file("/var/lib/app-data/email/bodies/Facturi/102.txt"));
	CHECK(!read_file(BODIES "/102.txt"));
	/* Opened again: there, from the cache */
	CHECK(ui_test_back() && ui_test_back());
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Enel") && ui_test_shows("Facturi"));
	CHECK_STR(read_file(FOLDER_FILE), "Facturi");
	/* Back to the inbox, its messages at once */
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Inbox", WAIT));
	CHECK(ui_test_tap_text("Inbox"));
	CHECK(ui_test_shows("Ion Popescu"));
	CHECK(!ui_test_shows("Enel"));
}

/* The folders kept: shown when the server cannot be reached, with why */
TEST(folders_offline)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	folders();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Facturi", WAIT));
	CHECK(ui_test_back());
	ui_test_imap_refuse("LIST", NULL); /* hung up on */
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_shows("Facturi")); /* at once */
	CHECK(ui_test_wait_for("Cannot reach 127.0.0.1.", WAIT));
	CHECK(ui_test_shows("Facturi"));
}

TEST(folders_refused)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	three_messages();
	ui_test_imap_refuse("LIST", "NO LIST failed");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("127.0.0.1 refused the request.", WAIT));
	CHECK(!ui_test_shows("Sent"));
}

/* The cache of a folder whose UIDs the server renumbered (UIDVALIDITY) is dropped:
 * what was loaded below the newest would be other messages */
TEST(folder_cache_kept_while_uids_hold)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE, inbox_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 57; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_shows("Old Sender")); /* older than the 50 listed: kept below them */
}

TEST(folder_cache_dropped_when_uids_change)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(INBOX_FILE,
	              "{\"uidvalidity\": 1, \"more\": false, \"mails\": [{\"uid\": 90, \"date\": 1793000000, \"seen\": true,"
	              " \"from\": \"Old Sender\", \"address\": \"old@example.com\", \"subject\": \"From the cache\"}]}\n");
	ui_test_write(BODIES "/90.txt", "An old body");
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	for (int i = 0; i < 57; i++)
		ui_test_imap_message("Someone", "someone@example.com", "Older", "01-Jan-2026 00:00:00 +0000", true);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Old Sender")); /* until the server says */
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(!ui_test_shows("Old Sender"));
	CHECK(!read_file(BODIES "/90.txt"));
}

/* What an earlier version kept of the inbox is found again */
TEST(inbox_cache_of_an_earlier_version)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_write(OLD_INBOX_FILE, old_inbox_json);
	ui_test_write(OLD_BODIES "/90.txt", "The body kept on the phone");
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(ui_test_tap_text("Old Sender"));
	CHECK(ui_test_wait_for("The body kept on the phone", WAIT));
	CHECK(!read_file(OLD_INBOX_FILE) && read_file(INBOX_FILE));
}
