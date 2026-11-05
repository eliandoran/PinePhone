/*
 * GitHub (apps/github), the plugin app: github.so built for the host (GITHUB_APP,
 * the Makefile's), installed in /var/lib/apps/github and loaded by phone-ui
 * (external.c) as on the phone. GitHub's REST API is served by the tests
 * (fake_curl.c): the token's form, the notifications, marking them read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define APP "/var/lib/apps/github"
#define TOKEN_FILE "/var/lib/app-data/github/token"
#define API "https://api.github.com"
#define TOKEN "ghp_test123"
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

/* As `just app github` installs it */
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "") || !copy(GITHUB_APP "/app.json", APP "/app.json") ||
	    !copy(GITHUB_APP "/github.so", APP "/github.so"))
		TEST_FAIL("cannot install %s", GITHUB_APP);
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	static char buf[256];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static const char notifications[] =
	"[{\"id\": \"101\", \"unread\": true, \"reason\": \"review_requested\","
	"  \"updated_at\": \"2026-11-05T09:30:00Z\","
	"  \"subject\": {\"title\": \"Add a GitHub app\", \"type\": \"PullRequest\","
	"    \"url\": \"https://api.github.com/repos/elian/pinephone/pulls/7\"},"
	"  \"repository\": {\"full_name\": \"elian/pinephone\"}},"
	" {\"id\": \"102\", \"unread\": false, \"reason\": \"mention\","
	"  \"updated_at\": \"2026-11-04T18:00:00Z\","
	"  \"subject\": {\"title\": \"Modem does not wake from suspend\", \"type\": \"Issue\","
	"    \"url\": \"https://api.github.com/repos/megi/linux/issues/42\"},"
	"  \"repository\": {\"full_name\": \"megi/linux\"}},"
	" {\"id\": \"103\", \"unread\": true, \"reason\": \"subscribed\","
	"  \"updated_at\": \"2026-11-03T12:00:00Z\","
	"  \"subject\": {\"title\": \"v9.3\", \"type\": \"Release\", \"url\": null},"
	"  \"repository\": {\"full_name\": \"lvgl/lvgl\"}}]";

static void signed_in(void)
{
	ui_test_write(TOKEN_FILE, TOKEN "\n");
}

/* Without a token: the form asks for one, which is kept and sent from then on */
TEST(sign_in)
{
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_shows("Sign in to GitHub"));
	CHECK(ui_test_request_count() == 0);
	CHECK(ui_test_tap_field("Token"));
	CHECK(ui_test_type(TOKEN "\n"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK_STR(read_file(TOKEN_FILE), TOKEN "\n");
	const struct ui_test_request *r = ui_test_requested(API "/notifications*");
	CHECK(r);
	if (!r)
		return;
	CHECK_STR(r->method, "GET");
	CHECK(strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	CHECK(strstr(r->headers, "Accept: application/vnd.github+json\n"));
	CHECK(!ui_test_shows("Sign in to GitHub"));
}

/* Each notification: its repository, title, kind and reason; read ones too */
TEST(notifications_listed)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_shows("elian/pinephone"));
	CHECK(ui_test_shows("Pull request \xc2\xb7 Review requested"));
	CHECK(ui_test_shows("Modem does not wake from suspend"));
	CHECK(ui_test_shows("Issue \xc2\xb7 Mentioned"));
	CHECK(ui_test_shows("v9.3"));
	CHECK(ui_test_shows("Release \xc2\xb7 Subscribed"));
	const struct ui_test_request *r = ui_test_requested(API "/notifications*");
	CHECK(r && strstr(r->url, "all=true"));
}

TEST(no_notifications)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", "[]");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("No notifications", WAIT));
}

/* Offline, then the refresh button once it is back */
TEST(offline_then_refresh)
{
	signed_in();
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(!ui_test_shows("Cannot reach GitHub"));
}

/* A revoked token: back to the form, saying so */
TEST(token_refused)
{
	signed_in();
	ui_test_serve(API "/notifications*", 401, "application/json", "{\"message\": \"Bad credentials\"}");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("GitHub refused this token.", WAIT));
	CHECK(ui_test_shows("Sign in to GitHub"));
}

/* Tapping an unread notification marks its thread read */
TEST(tap_marks_read)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(API "/notifications/threads/*", 205, "text/plain", "");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	ui_test_wait(300);
	const struct ui_test_request *r = ui_test_requested(API "/notifications/threads/101");
	CHECK(r && !strcmp(r->method, "PATCH"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	/* A read one is not marked again */
	unsigned count = ui_test_request_count();
	CHECK(ui_test_tap_text("Modem does not wake from suspend"));
	ui_test_wait(300);
	CHECK_INT(ui_test_request_count(), count);
}

/* The header's button marks them all read, at once */
TEST(mark_all_read)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	ui_test_serve(API "/notifications", 205, "text/plain", "");
	CHECK(ui_test_tap_text(LV_SYMBOL_OK));
	ui_test_wait(300);
	const struct ui_test_request *r = ui_test_request(-1);
	CHECK(r && !strcmp(r->url, API "/notifications"));
	CHECK(r && !strcmp(r->method, "PUT"));
	CHECK(r && r->body && strstr(r->body, "\"read\":true"));
	/* Nothing left to mark */
	unsigned count = ui_test_request_count();
	CHECK(ui_test_tap_text("v9.3"));
	ui_test_wait(300);
	CHECK_INT(ui_test_request_count(), count);
}
