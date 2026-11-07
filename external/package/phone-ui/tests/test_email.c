/*
 * Email (apps/email), the plugin app: email.so built for the host (EMAIL_APP, the
 * Makefile's), installed in /var/lib/apps/email and loaded by phone-ui (external.c)
 * as on the phone. Setting up the account: the provider's servers looked up in
 * Thunderbird's database and the password tried on the IMAP server, both served by
 * the tests (fake_curl.c), the account kept in /var/lib/app-data, removed again.
 */

#include <stdio.h>
#include <string.h>

#include <curl/curl.h>

#include "test.h"
#include "ui.h"
#include "ui_test.h"

#define APP "/var/lib/apps/email"
#define ACCOUNT_FILE "/var/lib/app-data/email/account.json"
#define AUTOCONFIG "https://autoconfig.thunderbird.net/v1.1/"
#define IMAP "imaps://imap.example.com:993/"
#define ADDRESS "anna@example.com"
#define PASSWORD "s3cret"
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
	static char buf[1024];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Thunderbird's entry for example.com (autoconfig.thunderbird.net): POP3 listed before
 * IMAP, the IMAP server over TLS, the SMTP one with STARTTLS */
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
	" \"imap\": \"" IMAP "\", \"smtp\": \"smtp://smtp.example.com:587/\"}\n";

static void known_provider(void)
{
	ui_test_serve(AUTOCONFIG "example.com", 200, "text/xml", autoconfig);
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

/* The servers from Thunderbird's database, the password tried on the IMAP one, kept */
TEST(signs_in)
{
	known_provider();
	ui_test_serve(IMAP, 0, NULL, "");
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, PASSWORD));
	CHECK(ui_test_wait_for("Signed in.", WAIT));
	CHECK(ui_test_shows(ADDRESS));
	CHECK(!ui_test_shows("Your email account"));

	const struct ui_test_request *r = ui_test_requested("imaps://*");
	CHECK(r);
	if (!r)
		return;
	CHECK_STR(r->url, IMAP);
	CHECK_STR(r->method, "NOOP");
	CHECK_STR(r->user, ADDRESS);
	CHECK_STR(r->password, PASSWORD);

	const char *saved = read_file(ACCOUNT_FILE);
	CHECK(saved);
	if (!saved)
		return;
	CHECK(strstr(saved, "\"address\": \"" ADDRESS "\""));
	CHECK(strstr(saved, "\"user\": \"" ADDRESS "\""));
	CHECK(strstr(saved, "\"password\": \"" PASSWORD "\""));
	CHECK(strstr(saved, "\"imap\": \"" IMAP "\""));
	CHECK(strstr(saved, "\"smtp\": \"smtp://smtp.example.com:587/\""));
}

TEST(password_refused)
{
	known_provider();
	ui_test_serve_error(IMAP, CURLE_LOGIN_DENIED);
	CHECK(ui_test_open("Email"));
	CHECK(sign_in(ADDRESS, "wrong"));
	CHECK(ui_test_wait_for("imap.example.com refused this password.", WAIT));
	CHECK(ui_test_shows("Sign in"));
	CHECK(!read_file(ACCOUNT_FILE));
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
	ui_test_serve("imaps://mail.example.org:993/", 0, NULL, "");
	CHECK(ui_test_open("Email"));
	CHECK(sign_in("bob@example.org", PASSWORD));
	CHECK(ui_test_wait_for("No settings are known for example.org. Enter its IMAP and SMTP servers.", WAIT));
	CHECK(ui_test_tap_field("IMAP server"));
	CHECK(ui_test_type("mail.example.org"));
	CHECK(ui_test_tap_field("SMTP server"));
	CHECK(ui_test_type("mail.example.org:2525\n"));
	CHECK(ui_test_wait_for("Signed in.", WAIT));

	const struct ui_test_request *r = ui_test_requested("imaps://*");
	CHECK(r);
	if (r) {
		CHECK_STR(r->user, "bob@example.org");
		CHECK_STR(r->password, PASSWORD);
	}
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

TEST(account_kept)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows(ADDRESS));
	CHECK(ui_test_shows("Signed in."));
	CHECK(!ui_test_shows("Your email account"));
	CHECK_INT((int)ui_test_request_count(), 0);
}

TEST(account_removed)
{
	ui_test_write(ACCOUNT_FILE, account_json);
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_tap_text("Remove account"));
	CHECK(ui_test_shows("Cancel"));
	CHECK(ui_test_tap_text("Remove"));
	CHECK(ui_test_wait_for("Your email account", WAIT));
	CHECK(!read_file(ACCOUNT_FILE));
	CHECK(ui_test_back());
	/* Gone for good: opened again, the form */
	CHECK(ui_test_open("Email"));
	CHECK(ui_test_shows("Your email account"));
}
