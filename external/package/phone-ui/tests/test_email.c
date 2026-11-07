/*
 * Email (apps/email), the plugin app: email.so built for the host (EMAIL_APP, the
 * Makefile's), installed in /var/lib/apps/email and loaded by phone-ui (external.c)
 * as on the phone. Setting up the account: the provider's servers looked up in
 * Thunderbird's database (fake_curl.c) and the password tried on the IMAP server
 * (fake_imap.c, on fake_curl.c's sockets), the account kept in /var/lib/app-data,
 * removed again. The inbox: its last messages listed, kept, refreshed.
 */

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

#include "test.h"
#include "ui.h"
#include "ui_symbols.h"
#include "ui_test.h"

#define APP "/var/lib/apps/email"
#define ACCOUNT_FILE "/var/lib/app-data/email/account.json"
#define INBOX_FILE "/var/lib/app-data/email/inbox.json"
#define BODIES "/var/lib/app-data/email/bodies"
#define AUTOCONFIG "https://autoconfig.thunderbird.net/v1.1/"
#define IMAP "imaps://imap.example.com:993/"
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
	static char buf[4096];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Thunderbird's entry for example.com (autoconfig.thunderbird.net): POP3 listed before
 * IMAP, the IMAP server over TLS, the SMTP one with STARTTLS (taken on the TLS port) */
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
	"      <hostname>imap.example.com</hostname>\n"
	"      <port>993</port>\n"
	"      <socketType>SSL</socketType>\n"
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

static const char inbox_json[] =
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
	CHECK(strstr(ui_test_imap_commands(), "LOGIN \"" ADDRESS "\" \"" PASSWORD "\"\n"));
	CHECK(strstr(ui_test_imap_commands(), "SELECT INBOX\n"));
	const struct ui_test_request *r = ui_test_requested("imaps://*");
	CHECK(r && strcmp(r->url, IMAP) == 0 && strcmp(r->method, "CONNECT") == 0);

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
	CHECK(ui_test_wait_for("imap.example.com refused this password.", WAIT));
	CHECK(ui_test_shows("Sign in"));
	CHECK(!read_file(ACCOUNT_FILE));
	CHECK_INT((int)ui_test_imap_logins(), 0);
}

TEST(certificate_refused)
{
	known_provider();
	ui_test_serve_error(IMAP, CURLE_PEER_FAILED_VERIFICATION);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("The certificate of imap.example.com cannot be trusted.", WAIT));
	CHECK(!read_file(ACCOUNT_FILE));
}

TEST(server_unreachable)
{
	known_provider();
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("Cannot reach imap.example.com.", WAIT));
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
	ui_test_imap_server("imaps://mail.example.org:993/", "bob@example.org", PASSWORD);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in("bob@example.org", PASSWORD));
	CHECK(ui_test_wait_for("No settings are known for example.org. Enter its IMAP and SMTP servers.", WAIT));
	CHECK(ui_test_tap_field("IMAP server"));
	CHECK(ui_test_type("mail.example.org"));
	CHECK(ui_test_tap_field("SMTP server"));
	CHECK(ui_test_type("mail.example.org:2525\n"));
	CHECK(ui_test_wait_for("No messages", WAIT));
	CHECK(ui_test_imap_logins() >= 1);
	const char *saved = read_file(ACCOUNT_FILE);
	CHECK(saved);
	if (!saved)
		return;
	CHECK(strstr(saved, "\"imap\": \"imaps://mail.example.org:993/\""));
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
	CHECK(strstr(ui_test_imap_commands(), "SELECT INBOX\nFETCH 1:3 (UID FLAGS INTERNALDATE ENVELOPE)\n"));

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

/* libcurl loses what arrives before the first read, the greeting in practice: nothing waits
 * for it */
TEST(inbox_without_greeting)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	ui_test_imap_server(IMAP, ADDRESS, PASSWORD);
	ui_test_imap_greeting(false);
	three_messages();
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_wait_for("Ion Popescu", WAIT));
	CHECK(ui_test_shows("Build failed on main"));
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
	CHECK(ui_test_wait_for("Cannot reach imap.example.com.", WAIT));
	CHECK(ui_test_shows("Old Sender"));
	CHECK(!ui_test_shows("No messages"));
}

static bool logged_in_twice(void)
{
	return ui_test_imap_logins() >= 2;
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
	CHECK(ui_test_wait_until(logged_in_twice, WAIT));
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
	/* Kept, and read now */
	CHECK(read_file(BODIES "/103.txt"));
	CHECK(ui_test_back());
	const char *saved = read_file(INBOX_FILE);
	CHECK(saved && strstr(saved, "\"uid\": 103, \"date\": 1794056733, \"seen\": true"));
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
