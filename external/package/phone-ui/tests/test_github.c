/*
 * GitHub (apps/github), the plugin app: github.so built for the host (GITHUB_APP,
 * the Makefile's), installed in /var/lib/apps/github and loaded by phone-ui
 * (external.c) as on the phone. GitHub's REST API is served by the tests
 * (fake_curl.c), and its GraphQL API for discussions: the token's form, the
 * notifications, marking them read, pull requests, issues and discussions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "json.h"
#include "ui.h"
#include "ui_symbols.h"
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
	"  \"repository\": {\"full_name\": \"lvgl/lvgl\"}},"
	" {\"id\": \"104\", \"unread\": true, \"reason\": \"comment\","
	"  \"updated_at\": \"2026-11-02T12:00:00Z\","
	"  \"subject\": {\"title\": \"Flashing \\\"over the air\\\"?\", \"type\": \"Discussion\", \"url\": null},"
	"  \"repository\": {\"full_name\": \"elian/pinephone\"}}]";

static void signed_in(void)
{
	ui_test_write(TOKEN_FILE, TOKEN "\n");
}

/* Signed in: GitHub starts on the repositories */
static bool open_notifications(void)
{
	return ui_test_open("GitHub") && ui_test_tap_text("Notifications");
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
	CHECK(ui_test_wait_for("Repositories", WAIT));
	CHECK(ui_test_tap_text("Notifications"));
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
	CHECK(open_notifications());
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
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("No notifications", WAIT));
}

/* Offline, then the refresh button once it is back */
TEST(offline_then_refresh)
{
	signed_in();
	CHECK(open_notifications());
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
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("GitHub refused this token.", WAIT));
	CHECK(ui_test_shows("Sign in to GitHub"));
}

/* Tapping an unread notification marks its thread read (a release: opens nothing) */
TEST(tap_marks_read)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(API "/notifications/threads/*", 205, "text/plain", "");
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("v9.3", WAIT));
	CHECK(ui_test_tap_text("v9.3"));
	ui_test_wait(300);
	const struct ui_test_request *r = ui_test_requested(API "/notifications/threads/103");
	CHECK(r && !strcmp(r->method, "PATCH"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	/* A read one is not marked again */
	CHECK(ui_test_tap_text("Modem does not wake from suspend"));
	ui_test_wait(300);
	CHECK(!ui_test_requested(API "/notifications/threads/102"));
}

/* The header's button marks them all read, at once */
TEST(mark_all_read)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(open_notifications());
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

/* A pull request: the notification's subject, its conversation, the account */
#define PR API "/repos/elian/pinephone/pulls/7"
#define COMMENTS API "/repos/elian/pinephone/issues/7/comments"

static const char *pr_json(const char *state, bool merged, bool draft)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf),
		 "{\"number\": 7, \"title\": \"Add a GitHub app\", \"state\": \"%s\", \"merged\": %s,"
		 " \"draft\": %s, \"user\": {\"login\": \"elian\"}, \"created_at\": \"2026-11-04T08:00:00Z\","
		 " \"body\": \"Adds a GitHub app.\\r\\n\\r\\n<!-- the template's hint -->\\r\\nNotifications first.\","
		 " \"base\": {\"ref\": \"main\"}, \"head\": {\"ref\": \"github-app\"}}",
		 state, merged ? "true" : "false", draft ? "true" : "false");
	return buf;
}

static void serve_pr(const char *pr)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(API "/notifications/threads/*", 205, "text/plain", "");
	ui_test_serve(API "/user", 200, "application/json", "{\"login\": \"elian\"}");
	ui_test_serve(PR, 200, "application/json", pr);
	ui_test_serve(COMMENTS "?*", 200, "application/json",
		      "[{\"id\": 1, \"user\": {\"login\": \"alice\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		      "  \"body\": \"Looks good, one nit.\"},"
		      " {\"id\": 2, \"user\": {\"login\": \"elian\"}, \"created_at\": \"2026-11-04T11:00:00Z\","
		      "  \"body\": \"Fixed, thanks!\"}]");
	ui_test_serve(PR "/reviews*", 200, "application/json",
		      "[{\"id\": 5, \"user\": {\"login\": \"bob\"}, \"state\": \"CHANGES_REQUESTED\","
		      "  \"submitted_at\": \"2026-11-04T09:00:00Z\", \"body\": \"Please add tests.\"},"
		      " {\"id\": 6, \"user\": {\"login\": \"alice\"}, \"state\": \"APPROVED\","
		      "  \"submitted_at\": \"2026-11-04T12:00:00Z\", \"body\": \"\"}]");
}

static bool open_pr(void)
{
	return open_notifications() && ui_test_wait_for("Add a GitHub app", WAIT) &&
	       ui_test_tap_text("Add a GitHub app") && ui_test_wait_for("Fixed, thanks!", WAIT);
}

/* Tapping a pull request's notification opens it: title, key, state, branches,
 * the description, then the comments and reviews in their order */
TEST(pr_opens)
{
	serve_pr(pr_json("open", false, false));
	CHECK(open_pr());
	CHECK(ui_test_requested(API "/notifications/threads/101"));
	CHECK(ui_test_shows("Add a GitHub app"));
	CHECK(ui_test_shows("elian/pinephone#7"));
	CHECK(ui_test_shows("Open"));
	CHECK(ui_test_shows("Into main from github-app"));
	/* The description in one bubble, a label per paragraph, without the template's
	 * comment */
	lv_obj_t *p1 = ui_test_find("Adds a GitHub app."), *p2 = ui_test_find("Notifications first.");
	CHECK(p1 && p2 && lv_obj_get_parent(p1) == lv_obj_get_parent(p2));
	CHECK(!ui_test_shows_part("template"));
	CHECK(ui_test_shows("bob requested changes"));
	CHECK(ui_test_shows("Please add tests."));
	CHECK(ui_test_shows("alice"));
	CHECK(ui_test_shows("Looks good, one nit."));
	CHECK(ui_test_shows("alice approved"));
	/* In the order they came */
	lv_obj_t *a = ui_test_find("bob requested changes"), *b = ui_test_find("Looks good, one nit."),
		 *c = ui_test_find("Fixed, thanks!"), *d = ui_test_find("alice approved");
	if (!a || !b || !c || !d)
		return;
	lv_area_t ra, rb, rc, rd;
	lv_obj_get_coords(a, &ra);
	lv_obj_get_coords(b, &rb);
	lv_obj_get_coords(c, &rc);
	lv_obj_get_coords(d, &rd);
	CHECK(ra.y1 < rb.y1 && rb.y1 < rc.y1 && rc.y1 < rd.y1);
	/* The account's own comment on the right, the others' on the left */
	CHECK(rc.x1 > 360);
	CHECK(rb.x1 < 100);
}

TEST(pr_states)
{
	serve_pr(pr_json("closed", true, false));
	CHECK(open_pr());
	CHECK(ui_test_shows("Merged"));
	CHECK(ui_test_back());
	ui_test_serve(PR, 200, "application/json", pr_json("closed", false, false));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Closed", WAIT));
	CHECK(ui_test_back());
	ui_test_serve(PR, 200, "application/json", pr_json("open", false, true));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Draft", WAIT));
}


/* A comment sent from the compose bar, shown once GitHub has it */
TEST(pr_comment)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(COMMENTS, 201, "application/json",
		      "{\"id\": 3, \"user\": {\"login\": \"elian\"}, \"created_at\": \"2026-11-05T09:00:00Z\","
		      " \"body\": \"Merging \\\"now\\\"\"}");
	CHECK(open_pr());
	CHECK(ui_test_tap_field("Comment"));
	CHECK(ui_test_type("Merging \"now\""));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("Merging \"now\"", WAIT));
	const struct ui_test_request *r = ui_test_requested(COMMENTS);
	CHECK(r && !strcmp(r->method, "POST"));
	CHECK(r && r->body && !strcmp(r->body, "{\"body\":\"Merging \\\"now\\\"\"}"));
	CHECK(!ui_test_shows_part("Not sent"));
}

TEST(pr_comment_refused)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(COMMENTS, 403, "application/json", "{\"message\": \"Resource not accessible\"}");
	CHECK(open_pr());
	CHECK(ui_test_tap_field("Comment"));
	CHECK(ui_test_type("Hello"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for(LV_SYMBOL_WARNING " Not sent: Resource not accessible", WAIT));
}

TEST(pr_offline)
{
	serve_pr(pr_json("open", false, false));
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	ui_test_serve_error(API "/repos/*", 6 /* CURLE_COULDNT_RESOLVE_HOST */);
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
}

/* A long comment cut, with an ellipsis (a label is a texture as large) */
TEST(pr_long_comment)
{
	serve_pr(pr_json("open", false, false));
	static char comments[4096];
	char body[3004]; /* and the ellipsis expected */
	memset(body, 'a', 3001); /* one byte over: the ellipsis is longer */
	body[3001] = '\0';
	snprintf(comments, sizeof(comments),
		 "[{\"id\": 1, \"user\": {\"login\": \"alice\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		 "  \"body\": \"%s\"}]", body);
	ui_test_serve(COMMENTS "?*", 200, "application/json", comments);
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("alice", WAIT));
	memcpy(body + 3000, "\xe2\x80\xa6", 4);
	CHECK(ui_test_shows(body));
}

/* Markdown: the comment's widgets, by what they show */
static const char markdown[] =
	"# Heading\n"
	"\n"
	"Some **bold**, *italic*, `code` and a [link](https://example.org).\n"
	"A line break.\n"
	"\n"
	"- one\n"
	"- two\n"
	"  1. nested\n"
	"\n"
	"- [x] done\n"
	"- [ ] to do\n"
	"\n"
	"> quoted\n"
	"\n"
	"```c\n"
	"int x = 1;\n"
	"```\n"
	"\n"
	"<details>raw &amp; kept</details>\n";

static bool open_markdown(void)
{
	static char comments[2048], escaped[1024];
	json_escape(escaped, sizeof(escaped), markdown);
	snprintf(comments, sizeof(comments),
		 "[{\"id\": 1, \"user\": {\"login\": \"alice\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		 "  \"body\": \"%s\"}]", escaped);
	ui_test_serve(COMMENTS "?*", 200, "application/json", comments);
	return open_notifications() && ui_test_wait_for("Add a GitHub app", WAIT) &&
	       ui_test_tap_text("Add a GitHub app") && ui_test_wait_for("Heading", WAIT);
}

/* The span of a span group showing `text` */
static lv_span_t *span(lv_obj_t *group, const char *text)
{
	for (uint32_t i = 0; group && i < lv_spangroup_get_span_count(group); i++) {
		lv_span_t *s = lv_spangroup_get_child(group, (int32_t)i);
		if (!strcmp(lv_span_get_text(s), text))
			return s;
	}
	return NULL;
}

static const lv_font_t *span_font(lv_obj_t *group, const char *text)
{
	lv_span_t *s = span(group, text);
	lv_style_value_t v;
	if (!s || lv_style_get_prop(lv_span_get_style(s), LV_STYLE_TEXT_FONT, &v) != LV_STYLE_RES_FOUND)
		return group ? lv_obj_get_style_text_font(group, 0) : NULL;
	return v.ptr;
}

static const lv_font_t *font_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	return label ? lv_obj_get_style_text_font(label, 0) : NULL;
}

TEST(pr_markdown)
{
	serve_pr(pr_json("open", false, false));
	CHECK(open_markdown());

	/* A heading: bold, larger */
	const lv_font_t *h = font_of("Heading");
	CHECK(h && h != &ui_font_32 && h->line_height == ui_font_32.line_height);

	/* Inline styles, a line break as written (GitHub's comments keep them) */
	lv_obj_t *p = ui_test_find("Some bold, italic, code and a link.\nA line break.");
	CHECK(p);
	const lv_font_t *body = span_font(p, "Some ");
	CHECK(body == &ui_font_24);
	const lv_font_t *bold = span_font(p, "bold"), *italic = span_font(p, "italic"), *code = span_font(p, "code");
	CHECK(bold && bold != body && bold->line_height == ui_font_24.line_height);
	CHECK(italic && italic != body && italic != bold);
	CHECK(code && code != body && code->line_height == ui_font_20.line_height);
	lv_span_t *link = span(p, "link");
	lv_style_value_t v;
	CHECK(link && lv_style_get_prop(lv_span_get_style(link), LV_STYLE_TEXT_COLOR, &v) == LV_STYLE_RES_FOUND &&
	      lv_color_to_u32(v.color) == lv_color_to_u32(lv_color_hex(UI_COLOR_ACCENT)));

	/* Lists: bullets, numbers, nested; tasks with their box */
	CHECK(ui_test_shows("one"));
	CHECK(ui_test_shows("two"));
	CHECK(ui_test_shows("1."));
	CHECK(ui_test_shows("nested"));
	lv_obj_t *bullets[4];
	CHECK_INT(ui_test_find_all("\xe2\x80\xa2", bullets, 4), 2);
	CHECK(ui_test_shows("done"));
	CHECK(ui_test_shows("to do"));
	CHECK_INT(ui_test_find_all(LV_SYMBOL_OK, bullets, 4), 1);

	/* A quote: a bar on its left */
	lv_obj_t *q = ui_test_find("quoted");
	CHECK(q && lv_obj_get_style_border_width(lv_obj_get_parent(q), 0) > 0);

	/* Code: monospace, not wrapped */
	CHECK(font_of("int x = 1;") == code);

	/* HTML without its rendering from GitHub (body_html) is shown as written */
	CHECK(ui_test_shows_part("<details>raw &amp; kept</details>"));
}

/* Where a pixel of about `rgb` is shown inside obj's area (on screen), false if none */
static bool shows_color(lv_obj_t *obj, uint32_t rgb)
{
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	for (int32_t y = LV_MAX(a.y1, 0); y <= LV_MIN(a.y2, 1439); y += 2)
		for (int32_t x = LV_MAX(a.x1, 0); x <= LV_MIN(a.x2, 719); x += 2) {
			uint32_t c = lv_color_to_u32(ui_test_pixel(x, y)) & 0xffffff;
			int dr = (int)(c >> 16) - (int)(rgb >> 16), dg = (int)(c >> 8 & 0xff) - (int)(rgb >> 8 & 0xff),
			    db = (int)(c & 0xff) - (int)(rgb & 0xff);
			if (abs(dr) < 16 && abs(dg) < 16 && abs(db) < 16)
				return true;
		}
	return false;
}

/* A message with HTML or a table: GitHub's rendering of it (body_html, asked for with
 * the full media type) laid out as a page in the bubble, its images fetched */
TEST(pr_html)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(COMMENTS "?*", 200, "application/json",
		      "[{\"id\": 1, \"user\": {\"login\": \"codecov\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		      "  \"body\": \"<table><tr><td>Coverage</td></tr></table>\","
		      "  \"body_html\": \"<table><tr><td style=\\\"background: #ff0000\\\">Coverage</td></tr></table>"
		      "<p><img src=\\\"https://camo.example/graph.png\\\"></p>\"},"
		      " {\"id\": 2, \"user\": {\"login\": \"alice\"}, \"created_at\": \"2026-11-04T11:00:00Z\","
		      "  \"body\": \"Plain **Markdown**\", \"body_html\": \"<p>Plain <strong>Markdown</strong></p>\"}]");
	size_t size;
	void *png = ui_test_png(60, 30, 0x00ff00, &size);
	ui_test_serve_data("https://camo.example/graph.png", 200, "image/png", png, size);
	free(png);
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("codecov", WAIT));
	const struct ui_test_request *r = ui_test_requested(COMMENTS "?*");
	CHECK(r && strstr(r->headers, "Accept: application/vnd.github.full+json\n"));
	CHECK(ui_test_requested(PR) && strstr(ui_test_requested(PR)->headers, "full+json"));

	/* Markdown alone stays widgets */
	CHECK(ui_test_shows("Plain Markdown"));
	CHECK(!ui_test_shows_part("Coverage"));

	/* The page: the cell's red, then the image once it came */
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (!page)
		return;
	lv_obj_scroll_to_view_recursive(page, LV_ANIM_OFF);
	ui_test_wait(500);
	CHECK(ui_test_requested("https://camo.example/graph.png"));
	page = ui_test_find_widget(&lv_3dtexture_class);
	lv_obj_t *html = lv_obj_get_parent(page);
	CHECK(shows_color(html, 0xff0000));
	CHECK(shows_color(html, 0x00ff00));
	/* In the bubble, no wider than a bubble's text */
	CHECK(lv_obj_get_width(html) <= 720 * 3 / 4 - 48);
}

/* A Markdown table: a grid of its cells, the header's bold, widgets even with GitHub's
 * HTML at hand */
TEST(pr_markdown_table)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(COMMENTS "?*", 200, "application/json",
		      "[{\"id\": 1, \"user\": {\"login\": \"alice\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		      "  \"body\": \"Results:\\n\\n| Name | Value |\\n|---|---|\\n| first | 1 |\\n| b | **22** |\\n\","
		      "  \"body_html\": \"<p>Results:</p><table></table>\"}]");
	CHECK(open_notifications());
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Results:", WAIT));
	CHECK(!ui_test_shows_part("|"));
	CHECK(!ui_test_find_widget(&lv_3dtexture_class));
	lv_obj_t *name = ui_test_find("Name"), *value = ui_test_find("Value"), *first = ui_test_find("first"),
		 *one = ui_test_find("1"), *b = ui_test_find("b"), *two = ui_test_find("22");
	CHECK(name && value && first && one && b && two);
	if (!name || !value || !first || !one || !b || !two)
		return;
	lv_area_t an, av, af, a1, ab, a2;
	lv_obj_get_coords(name, &an);
	lv_obj_get_coords(value, &av);
	lv_obj_get_coords(first, &af);
	lv_obj_get_coords(one, &a1);
	lv_obj_get_coords(b, &ab);
	lv_obj_get_coords(two, &a2);
	/* Rows and columns lined up */
	CHECK(an.y1 == av.y1 && af.y1 == a1.y1 && ab.y1 == a2.y1);
	CHECK(an.y1 < af.y1 && af.y1 < ab.y1);
	/* As high as their text */
	CHECK(af.y1 - an.y1 < 60);
	CHECK(an.x1 == af.x1 && af.x1 == ab.x1);
	CHECK(av.x1 == a1.x1 && a1.x1 == a2.x1);
	CHECK(av.x1 > af.x2);
	/* The header in bold, as the bold cell */
	CHECK(lv_obj_get_style_text_font(name, 0) != &ui_font_24);
}

/* A real bot's comment (github/), as Markdown: a file's text, malloc'd */
static char *fixture(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), GITHUB_FIXTURES "/%s", name);
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *text = calloc(1, 65536);
	if (text)
		text[fread(text, 1, 65535, f)] = '\0';
	fclose(f);
	return text;
}

static bool open_comment(const char *body)
{
	size_t n = strlen(body) * 6 + 1;
	char *escaped = malloc(n), *comments = malloc(n + 256);
	json_escape(escaped, n, body);
	snprintf(comments, n + 256,
		 "[{\"id\": 1, \"user\": {\"login\": \"codecov\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		 "  \"body\": \"%s\"}]", escaped);
	ui_test_serve(COMMENTS "?*", 200, "application/json", comments);
	free(escaped);
	free(comments);
	return open_notifications() && ui_test_wait_for("Add a GitHub app", WAIT) &&
	       ui_test_tap_text("Add a GitHub app") && ui_test_wait_for("codecov", WAIT);
}

/* Codecov's report: a heading, emoji by their names, a table of links */
TEST(pr_codecov)
{
	serve_pr(pr_json("open", false, false));
	char *body = fixture("codecov.md");
	CHECK(body);
	if (!body)
		return;
	CHECK(open_comment(body));
	free(body);
	/* The heading, the emoji by their names, the table after the paragraph (GitHub
	 * needs no blank line before it), all of it: its links' addresses do not count */
	CHECK(ui_test_shows("Codecov Report"));
	CHECK(ui_test_shows_part("\xe2\x9d\x8c Patch coverage is 90.34749%")); /* ❌ */
	CHECK(ui_test_shows_part("\xe2\x9c\x85 All tests successful.")); /* ✅ */
	CHECK(!ui_test_shows_part(":x:"));
	CHECK(!ui_test_shows_part(":warning:"));
	CHECK(!ui_test_shows_part("|"));
	CHECK(ui_test_shows("packages/trilium-core/src/services/media_probe.ts"));
	CHECK(ui_test_shows("78.14%"));
	CHECK(ui_test_shows_part("16 Missing and 17 partials"));
	CHECK(ui_test_shows("apps/client/src/menus/link_context_menu.ts"));
	CHECK(ui_test_shows_part("Thoughts on this report?"));
	CHECK(!ui_test_shows_part("\xe2\x80\xa6 ")); /* not cut */
}

/* An issue: the notification's subject, its comments, no reviews */
#define ISSUE API "/repos/megi/linux/issues/42"

static const char *issue_json(const char *state, const char *reason)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf),
		 "{\"number\": 42, \"title\": \"Modem does not wake from suspend\", \"state\": \"%s\","
		 " \"state_reason\": %s, \"user\": {\"login\": \"megi\"}, \"created_at\": \"2026-11-03T08:00:00Z\","
		 " \"body\": \"It **hangs** after a while.\","
		 " \"labels\": [{\"name\": \"bug\", \"color\": \"d73a4a\"}, {\"name\": \"modem\", \"color\": \"0e8a16\"}]}",
		 state, reason);
	return buf;
}

static void serve_issue(const char *issue)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(ISSUE, 200, "application/json", issue);
	ui_test_serve(ISSUE "/comments?*", 200, "application/json",
		      "[{\"id\": 9, \"user\": {\"login\": \"elian\"}, \"created_at\": \"2026-11-04T10:00:00Z\","
		      "  \"body\": \"Same here.\"}]");
}

static bool open_issue(void)
{
	return open_notifications() && ui_test_wait_for("Modem does not wake from suspend", WAIT) &&
	       ui_test_tap_text("Modem does not wake from suspend") && ui_test_wait_for("Same here.", WAIT);
}

/* Tapping an issue's notification opens it: its title, key, state, labels, the
 * description and the comments as a conversation */
TEST(issue_opens)
{
	serve_issue(issue_json("open", "null"));
	CHECK(open_issue());
	CHECK(ui_test_shows("Issue"));
	CHECK(ui_test_shows("megi/linux#42"));
	CHECK(ui_test_shows("Open"));
	CHECK(ui_test_shows("bug"));
	CHECK(ui_test_shows("modem"));
	CHECK(ui_test_shows("megi"));
	CHECK(ui_test_shows("It hangs after a while."));
	const struct ui_test_request *r = ui_test_requested(ISSUE);
	CHECK(r && strstr(r->headers, "full+json"));
	CHECK(!ui_test_requested(API "/repos/megi/linux/pulls/*"));
	/* The account's comment on the right */
	lv_obj_t *mine = ui_test_find("Same here.");
	lv_area_t a;
	if (mine) {
		lv_obj_get_coords(mine, &a);
		CHECK(a.x1 > 360);
	}
}

TEST(issue_states)
{
	serve_issue(issue_json("closed", "\"completed\""));
	CHECK(open_issue());
	CHECK(ui_test_shows("Closed"));
	CHECK(ui_test_back());
	ui_test_serve(ISSUE, 200, "application/json", issue_json("closed", "\"not_planned\""));
	CHECK(ui_test_tap_text("Modem does not wake from suspend"));
	CHECK(ui_test_wait_for("Not planned", WAIT));
}

TEST(issue_comment)
{
	serve_issue(issue_json("open", "null"));
	ui_test_serve(ISSUE "/comments", 201, "application/json",
		      "{\"id\": 10, \"user\": {\"login\": \"elian\"}, \"created_at\": \"2026-11-05T09:00:00Z\","
		      " \"body\": \"Fixed by a patch.\"}");
	CHECK(open_issue());
	CHECK(ui_test_tap_field("Comment"));
	CHECK(ui_test_type("Fixed by a patch."));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("Fixed by a patch.", WAIT));
	const struct ui_test_request *r = ui_test_requested(ISSUE "/comments");
	CHECK(r && !strcmp(r->method, "POST"));
}

/* A discussion: GitHub's notification has no URL for it and its REST API none
 * either, so it is found with GraphQL's search (its repository and title), then
 * loaded by its number */
#define GRAPHQL API "/graphql"
#define TOPIC "Flashing \"over the air\"?"

static const char discussion_search[] =
	"{\"data\": {\"search\": {\"nodes\": ["
	" {\"number\": 3, \"title\": \"Flashing over the air, again\", \"updatedAt\": \"2026-11-02T13:00:00Z\"},"
	" {\"number\": 2, \"title\": \"Flashing \\\"over the air\\\"?\", \"updatedAt\": \"2026-01-02T12:00:00Z\"},"
	" {\"number\": 12, \"title\": \"Flashing \\\"over the air\\\"?\", \"updatedAt\": \"2026-11-02T12:00:00Z\"}]}}}";

/* Answered, in Q&A: alice's answer and bob's reply to it, then elian's comment;
 * the reply came after that comment, and still shows under the answer */
static const char discussion[] =
	"{\"data\": {\"repository\": {\"discussion\": {\"id\": \"D_kwDO12\", \"number\": 12,"
	" \"title\": \"Flashing \\\"over the air\\\"?\", \"closed\": false, \"stateReason\": null,"
	" \"isAnswered\": true, \"category\": {\"name\": \"Q&A\"},"
	" \"author\": {\"login\": \"carol\"}, \"createdAt\": \"2026-11-01T08:00:00Z\","
	" \"body\": \"Can I flash **without** USB?\", \"bodyHTML\": \"<p>Can I flash <strong>without</strong> USB?</p>\","
	" \"comments\": {\"nodes\": ["
	"  {\"id\": \"DC_1\", \"isAnswer\": true, \"author\": {\"login\": \"alice\"},"
	"   \"createdAt\": \"2026-11-01T09:00:00Z\", \"body\": \"Yes: just flash.\","
	"   \"replies\": {\"nodes\": [{\"id\": \"DC_3\", \"author\": {\"login\": \"bob\"},"
	"    \"createdAt\": \"2026-11-02T09:00:00Z\", \"body\": \"Works here.\"}]}},"
	"  {\"id\": \"DC_2\", \"isAnswer\": false, \"author\": {\"login\": \"elian\"},"
	"   \"createdAt\": \"2026-11-01T10:00:00Z\", \"body\": \"Documented now.\","
	"   \"replies\": {\"nodes\": []}}]}}}}}";

static void serve_discussion(const char *detail)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(GRAPHQL, 200, "application/json", detail);
	ui_test_serve_once(GRAPHQL, 200, "application/json", discussion_search);
}

static bool open_discussion(const char *until)
{
	return open_notifications() && ui_test_wait_for(TOPIC, WAIT) && ui_test_tap_text(TOPIC) &&
	       ui_test_wait_for(until, WAIT);
}

/* The n-th request to GraphQL, NULL: fewer */
static const struct ui_test_request *graphql(int n)
{
	for (int i = 0; ui_test_request(i); i++)
		if (!strcmp(ui_test_request(i)->url, GRAPHQL) && n-- == 0)
			return ui_test_request(i);
	return NULL;
}

static bool body_has(const struct ui_test_request *r, const char *part)
{
	return r && r->body && strstr(r->body, part);
}

/* Tapping a discussion's notification opens it: title, key, state, category, then
 * the description and the comments, each followed by its replies */
TEST(discussion_opens)
{
	serve_discussion(discussion);
	CHECK(open_discussion("Documented now."));
	CHECK(ui_test_requested(API "/notifications/threads/104"));

	/* Searched for by its repository and title, its quotes left out (GitHub's search
	 * cannot have them in a phrase) */
	const struct ui_test_request *search = graphql(0), *load = graphql(1);
	CHECK(search && !strcmp(search->method, "POST"));
	CHECK(search && strstr(search->headers, "Authorization: Bearer " TOKEN "\n"));
	CHECK(body_has(search, "DISCUSSION"));
	CHECK(body_has(search, "repo:elian/pinephone in:title \\\"Flashing  over the air ?\\\""));
	/* The latest with that title exactly */
	CHECK(body_has(load, "\"number\":12"));
	CHECK(body_has(load, "\"owner\":\"elian\""));
	CHECK(body_has(load, "\"name\":\"pinephone\""));

	CHECK(ui_test_shows("Discussion"));
	CHECK(ui_test_shows("elian/pinephone#12"));
	CHECK(ui_test_shows("Answered"));
	CHECK(ui_test_shows("Q&A"));
	CHECK(ui_test_shows("carol"));
	CHECK(ui_test_shows("Can I flash without USB?"));
	CHECK(ui_test_shows(LV_SYMBOL_OK " Answer"));
	CHECK(ui_test_shows("bob, replying to alice"));

	lv_obj_t *a = ui_test_find("Can I flash without USB?"), *b = ui_test_find("Yes: just flash."),
		 *c = ui_test_find("Works here."), *d = ui_test_find("Documented now.");
	CHECK(a && b && c && d);
	if (!a || !b || !c || !d)
		return;
	lv_area_t ra, rb, rc, rd;
	lv_obj_get_coords(a, &ra);
	lv_obj_get_coords(b, &rb);
	lv_obj_get_coords(c, &rc);
	lv_obj_get_coords(d, &rd);
	CHECK(ra.y1 < rb.y1 && rb.y1 < rc.y1 && rc.y1 < rd.y1);
	/* A reply set in from its comment; the account's own on the right */
	CHECK(rc.x1 > rb.x1);
	CHECK(rd.x1 > 360);
}

/* The text of `discussion` with `from` replaced by `to` */
static const char *discussion_with(const char *from, const char *to)
{
	static char buf[sizeof(discussion) + 256];
	const char *s = strstr(discussion, from);
	if (!s)
		return discussion;
	snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(s - discussion), discussion, to, s + strlen(from));
	return buf;
}

TEST(discussion_closed)
{
	serve_discussion(discussion_with("\"closed\": false, \"stateReason\": null",
					 "\"closed\": true, \"stateReason\": \"OUTDATED\""));
	CHECK(open_discussion("Documented now."));
	CHECK(ui_test_shows("Outdated"));
	CHECK(!ui_test_shows("Answered"));
	CHECK(ui_test_back());
	serve_discussion(discussion_with("\"closed\": false, \"stateReason\": null",
					 "\"closed\": true, \"stateReason\": \"RESOLVED\""));
	CHECK(ui_test_tap_text(TOPIC));
	CHECK(ui_test_wait_for("Closed", WAIT));
}

/* Gone, or in a private repository the token cannot see: no such title */
TEST(discussion_not_found)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(GRAPHQL, 200, "application/json",
		      "{\"data\": {\"search\": {\"nodes\": [{\"number\": 3, \"title\": \"Something else\","
		      " \"updatedAt\": \"2026-11-02T13:00:00Z\"}]}}}");
	CHECK(open_discussion("Discussion not found"));
	CHECK(graphql(0) && !graphql(1));
}

/* GraphQL's errors come with 200: their message is shown */
TEST(discussion_error)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(GRAPHQL, 200, "application/json",
		      "{\"errors\": [{\"type\": \"INSUFFICIENT_SCOPES\","
		      " \"message\": \"Your token has not been granted the required scopes.\"}]}");
	CHECK(open_discussion("Your token has not been granted the required scopes."));
}

TEST(discussion_offline)
{
	serve_pr(pr_json("open", false, false));
	CHECK(open_discussion("Cannot reach GitHub"));
}

/* A comment sent from the compose bar with GraphQL's mutation, shown once GitHub has it */
TEST(discussion_comment)
{
	serve_discussion(discussion);
	CHECK(open_discussion("Documented now."));
	ui_test_serve(GRAPHQL, 200, "application/json",
		      "{\"data\": {\"addDiscussionComment\": {\"comment\": {\"id\": \"DC_4\","
		      " \"author\": {\"login\": \"elian\"}, \"createdAt\": \"2026-11-05T09:00:00Z\","
		      " \"body\": \"Thanks \\\"all\\\"\"}}}}");
	CHECK(ui_test_tap_field("Comment"));
	CHECK(ui_test_type("Thanks \"all\""));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("Thanks \"all\"", WAIT));
	ui_test_wait(300);
	CHECK(!ui_test_shows("Sending..."));
	const struct ui_test_request *r = graphql(2);
	CHECK(body_has(r, "addDiscussionComment"));
	CHECK(body_has(r, "\"id\":\"D_kwDO12\""));
	CHECK(body_has(r, "\"body\":\"Thanks \\\"all\\\"\""));
	CHECK(!ui_test_shows_part("Not sent"));
}

TEST(discussion_comment_refused)
{
	serve_discussion(discussion);
	CHECK(open_discussion("Documented now."));
	ui_test_serve(GRAPHQL, 200, "application/json",
		      "{\"data\": {\"addDiscussionComment\": null},"
		      " \"errors\": [{\"type\": \"FORBIDDEN\", \"message\": \"Discussion is locked\"}]}");
	CHECK(ui_test_tap_field("Comment"));
	CHECK(ui_test_type("Hello"));
	CHECK(ui_test_tap_text(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for(LV_SYMBOL_WARNING " Not sent: Discussion is locked", WAIT));
}

/* The bottom bar: Repositories, which it starts on, then Notifications; the
 * account's own repositories, most recently pushed first */
#define REPOS API "/user/repos*"

static const char repos[] =
	"[{\"name\": \"pinephone\", \"full_name\": \"elian/pinephone\", \"private\": false, \"fork\": false,"
	"  \"archived\": false, \"description\": \"Feature phone firmware for the PinePhone\","
	"  \"language\": \"C\", \"stargazers_count\": 12, \"forks_count\": 3,"
	"  \"html_url\": \"https://github.com/elian/pinephone\", \"pushed_at\": \"2026-11-05T09:30:00Z\"},"
	" {\"name\": \"dotfiles\", \"full_name\": \"elian/dotfiles\", \"private\": true, \"fork\": false,"
	"  \"archived\": false, \"description\": null, \"language\": \"Nix\", \"stargazers_count\": 0,"
	"  \"forks_count\": 0, \"html_url\": \"https://github.com/elian/dotfiles\","
	"  \"pushed_at\": \"2026-11-04T18:00:00Z\"},"
	" {\"name\": \"lvgl\", \"full_name\": \"elian/lvgl\", \"private\": false, \"fork\": true,"
	"  \"archived\": true, \"description\": \"Embedded graphics library\", \"language\": null,"
	"  \"stargazers_count\": 0, \"pushed_at\": \"2026-01-03T12:00:00Z\"}]";

static bool open_repos(void)
{
	return ui_test_open("GitHub") && ui_test_wait_for("pinephone", WAIT);
}

/* Signed in: the bar at the bottom, Repositories first and shown; the
 * notifications load only when asked for */
TEST(tabs_shown)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	CHECK(ui_test_shows("Repositories"));
	CHECK(ui_test_shows("Notifications"));
	CHECK(ui_test_shows(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_shows(LV_SYMBOL_BELL));
	CHECK(!ui_test_requested(API "/notifications*"));
	lv_obj_t *repos_tab = ui_test_find("Repositories"), *notifications_tab = ui_test_find("Notifications");
	CHECK(repos_tab && notifications_tab);
	if (!repos_tab || !notifications_tab)
		return;
	lv_area_t a, b;
	lv_obj_get_coords(repos_tab, &a);
	lv_obj_get_coords(notifications_tab, &b);
	/* At the bottom, below the list; Repositories on the left */
	CHECK(a.y1 > 1200);
	CHECK(a.x2 < b.x1);
	CHECK(ui_test_tap_text("Notifications"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
}

/* Signed out: no bar, only the form */
TEST(no_tabs_signed_out)
{
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_shows("Sign in to GitHub"));
	CHECK(!ui_test_shows("Repositories"));
	CHECK(!ui_test_shows("Notifications"));
}

/* Each repository: its name, description, what it is, its language and stars, when
 * it was last pushed to */
TEST(repositories_listed)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	const struct ui_test_request *r = ui_test_requested(REPOS);
	CHECK(r && !strcmp(r->method, "GET"));
	CHECK(r && strstr(r->url, "affiliation=owner"));
	CHECK(r && strstr(r->url, "sort=pushed"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	CHECK(ui_test_shows("pinephone"));
	CHECK(ui_test_shows("Feature phone firmware for the PinePhone"));
	CHECK(ui_test_shows("C \xc2\xb7 \xe2\x98\x85 12"));
	CHECK(ui_test_shows("dotfiles"));
	CHECK(ui_test_shows("Private \xc2\xb7 Nix"));
	CHECK(ui_test_shows("lvgl"));
	CHECK(ui_test_shows("Fork \xc2\xb7 Archived"));
	/* In GitHub's order */
	lv_obj_t *a = ui_test_find("pinephone"), *b = ui_test_find("dotfiles"), *c = ui_test_find("lvgl");
	if (a && b && c) {
		lv_area_t ra, rb, rc;
		lv_obj_get_coords(a, &ra);
		lv_obj_get_coords(b, &rb);
		lv_obj_get_coords(c, &rc);
		CHECK(ra.y1 < rb.y1 && rb.y1 < rc.y1);
	}
	/* The notifications and their mark-all button are on the other tab */
	CHECK(!ui_test_shows(LV_SYMBOL_OK));
	CHECK(ui_test_tap_text("Notifications"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_shows(LV_SYMBOL_OK));
	CHECK(!ui_test_shows("Feature phone firmware for the PinePhone"));
	/* And back, without loading them again */
	unsigned count = ui_test_request_count();
	CHECK(ui_test_tap_text("Repositories"));
	CHECK(ui_test_shows("Feature phone firmware for the PinePhone"));
	CHECK(!ui_test_shows("Add a GitHub app"));
	ui_test_wait(300);
	CHECK_INT(ui_test_request_count(), count);
}

TEST(no_repositories)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", "[]");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("No repositories", WAIT));
}

/* Offline, then the refresh button loads the tab shown again */
TEST(repositories_refresh)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	ui_test_serve(REPOS, 200, "application/json", repos);
	unsigned before = ui_test_request_count();
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	CHECK(ui_test_wait_for("Feature phone firmware for the PinePhone", WAIT));
	/* Not the notifications */
	for (unsigned i = before; ui_test_request(i); i++)
		CHECK(!strstr(ui_test_request(i)->url, "/notifications"));
}

/* Kept between openings: shown at once, loaded again */
TEST(repositories_kept)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	CHECK(ui_test_back());
	ui_test_network_hold(true);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_shows("pinephone"));
	CHECK(!ui_test_shows("Loading..."));
	ui_test_network_hold(false);
}

/* A revoked token on the repositories' tab: back to the form */
TEST(repositories_token_refused)
{
	signed_in();
	ui_test_serve(REPOS, 401, "application/json", "{\"message\": \"Bad credentials\"}");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("GitHub refused this token.", WAIT));
	CHECK(ui_test_shows("Sign in to GitHub"));
	CHECK(!ui_test_shows("Repositories"));
}

/* Tapping a repository opens it: its name, description, address, stars and forks,
 * from the list (nothing asked again), and nothing else */
TEST(repository_opens)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	unsigned count = ui_test_request_count();
	CHECK(ui_test_tap_text("pinephone"));
	CHECK(ui_test_shows("Repository"));
	CHECK(ui_test_shows("pinephone"));
	CHECK(ui_test_shows("Feature phone firmware for the PinePhone"));
	CHECK(ui_test_shows("https://github.com/elian/pinephone"));
	CHECK(ui_test_shows("Stars"));
	CHECK(ui_test_shows("12"));
	CHECK(ui_test_shows("Forks"));
	CHECK(ui_test_shows("3"));
	CHECK(!ui_test_shows_part("C \xc2\xb7"));
	CHECK(!ui_test_shows("Thu"));
	CHECK(!ui_test_shows("dotfiles"));
	CHECK(!ui_test_find_widget(&lv_textarea_class));
	ui_test_wait(300);
	CHECK_INT(ui_test_request_count(), count);
	/* Back to the list */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("dotfiles"));
}

/* Without a description: none shown; no stars or forks: 0 */
TEST(repository_without_description)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	CHECK(ui_test_tap_text("dotfiles"));
	CHECK(ui_test_shows("Repository"));
	CHECK(ui_test_shows("https://github.com/elian/dotfiles"));
	lv_obj_t *zeros[4];
	CHECK_INT(ui_test_find_all("0", zeros, 4), 2);
	CHECK(!ui_test_shows("Private \xc2\xb7 Nix"));
}

/* A repository's README, from its button: GitHub's Markdown as Markdown is in
 * comments (markdown.c), on a page of its own */
#define README API "/repos/elian/pinephone/readme"

static bool open_readme(const char *until)
{
	return open_repos() && ui_test_tap_text("pinephone") && ui_test_tap_text("README") &&
	       ui_test_wait_for(until, WAIT);
}

TEST(readme_markdown)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 200, "application/vnd.github.raw+json",
		      "# Welcome\n\nSome **bold** text.\n\n- one\n- two\n\n```\nmake\n```\n");
	CHECK(open_readme("Welcome"));
	const struct ui_test_request *r = ui_test_requested(README);
	CHECK(r && !strcmp(r->method, "GET"));
	CHECK(r && strstr(r->headers, "Accept: application/vnd.github.raw+json\n"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	CHECK(ui_test_shows("README"));
	CHECK(ui_test_shows("Some bold text."));
	CHECK(ui_test_shows("one"));
	CHECK(ui_test_shows("two"));
	CHECK(ui_test_shows("make"));
	const lv_font_t *h = font_of("Welcome");
	CHECK(h && h->line_height == ui_font_32.line_height && h != &ui_font_32);
	/* Markdown alone: widgets, nothing asked again */
	CHECK(!ui_test_find_widget(&lv_3dtexture_class));
	ui_test_wait(300);
	int asked = 0;
	for (int i = 0; ui_test_request(i); i++)
		asked += !strcmp(ui_test_request(i)->url, README);
	CHECK_INT(asked, 1);
	/* The full width, not a bubble's */
	lv_obj_t *text = ui_test_find("Some bold text.");
	CHECK(text && lv_obj_get_x(lv_obj_get_parent(text)) < 100);
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Forks"));
}

/* With HTML (a centered logo): GitHub's rendering of it as a page, its relative
 * images from the repository */
TEST(readme_html)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 200, "application/vnd.github.html+json",
		      "<div id=\"readme\"><article><table><tr><td style=\"background: #ff0000\">Logo</td></tr></table>"
		      "<p><img src=\"docs/logo.png\"></p></article></div>");
	ui_test_serve_once(README, 200, "application/vnd.github.raw+json",
			   "<p align=\"center\"><img src=\"docs/logo.png\"></p>\n\nText\n");
	size_t size;
	void *png = ui_test_png(60, 30, 0x00ff00, &size);
	ui_test_serve_data("https://github.com/elian/pinephone/raw/HEAD/docs/logo.png", 200, "image/png", png, size);
	free(png);
	CHECK(open_readme("README"));
	ui_test_wait(500);
	const struct ui_test_request *r = ui_test_requested(README);
	CHECK(r && strstr(r->headers, "Accept: application/vnd.github.html+json\n"));
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (!page)
		return;
	CHECK(ui_test_requested("https://github.com/elian/pinephone/raw/HEAD/docs/logo.png"));
	ui_test_wait(300);
	page = ui_test_find_widget(&lv_3dtexture_class);
	lv_obj_t *html = lv_obj_get_parent(page);
	CHECK(shows_color(html, 0xff0000));
	CHECK(shows_color(html, 0x00ff00));
	/* Wider than a bubble */
	CHECK(lv_obj_get_width(html) > 720 * 3 / 4);
}

TEST(readme_missing)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 404, "application/json", "{\"message\": \"Not Found\"}");
	CHECK(open_readme("No README"));
}

TEST(readme_offline)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_readme("Cannot reach GitHub"));
}

/* A revoked token: back to the form */
TEST(readme_token_refused)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 401, "application/json", "{\"message\": \"Bad credentials\"}");
	CHECK(open_readme("GitHub refused this token."));
	CHECK(ui_test_shows("Sign in to GitHub"));
}

/* Whether obj's area (on screen) has a colored pixel: emoji, where text is white or
 * gray */
static bool shows_colored(lv_obj_t *obj)
{
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	for (int32_t y = LV_MAX(a.y1, 0); y <= LV_MIN(a.y2, 1439); y++)
		for (int32_t x = LV_MAX(a.x1, 0); x <= LV_MIN(a.x2, 719); x++) {
			uint32_t c = lv_color_to_u32(ui_test_pixel(x, y));
			int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
			if (LV_MAX(r, LV_MAX(g, b)) - LV_MIN(r, LV_MIN(g, b)) > 100)
				return true;
		}
	return false;
}

/* Emoji in headings as in paragraphs: drawn in their colors */
TEST(readme_emoji)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 200, "application/vnd.github.raw+json",
		      "# \xf0\x9f\x9a\x80 Rocket\n\n## Fire \xf0\x9f\x94\xa5\n\n### Apple \xf0\x9f\x8d\x8e\n\n"
		      "Text \xf0\x9f\x8d\x8e\n");
	CHECK(open_readme("Text \xf0\x9f\x8d\x8e"));
	ui_test_wait(100);
	ui_test_screenshot("build/readme-emoji.png");
	const char *texts[] = {"\xf0\x9f\x9a\x80 Rocket", "Fire \xf0\x9f\x94\xa5", "Apple \xf0\x9f\x8d\x8e",
			       "Text \xf0\x9f\x8d\x8e"};
	for (size_t i = 0; i < LV_ARRAYLEN(texts); i++) {
		lv_obj_t *label = ui_test_find(texts[i]);
		CHECK(label);
		if (label && !shows_colored(label))
			TEST_FAIL("no emoji drawn in \"%s\"", texts[i]);
	}
}

/* A README with HTML: its emoji drawn too (in GitHub's g-emoji, as it renders them) */
TEST(readme_html_emoji)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(README, 200, "application/vnd.github.html+json",
		      "<div id=\"readme\"><article><h1><g-emoji class=\"g-emoji\" alias=\"rocket\">"
		      "\xf0\x9f\x9a\x80</g-emoji> Rocket</h1></article></div>");
	ui_test_serve_once(README, 200, "application/vnd.github.raw+json",
			   "<h1>\xf0\x9f\x9a\x80 Rocket</h1>\n");
	CHECK(open_readme("README"));
	ui_test_wait(300);
	lv_obj_t *page = ui_test_find_widget(&lv_3dtexture_class);
	CHECK(page);
	if (page && !shows_colored(lv_obj_get_parent(page)))
		TEST_FAIL("no emoji drawn in the README's page");
}

/* From a pull request or an issue, the header's button opens its repository, loaded
 * (often not the account's: not in the list) */
static const char linux_repo[] =
	"{\"name\": \"linux\", \"full_name\": \"megi/linux\", \"private\": false, \"fork\": true,"
	" \"description\": \"Linux kernel for the PinePhone\", \"language\": \"C\","
	" \"stargazers_count\": 120, \"forks_count\": 45, \"html_url\": \"https://github.com/megi/linux\"}";

TEST(pr_repository)
{
	serve_pr(pr_json("open", false, false));
	ui_test_serve(API "/repos/elian/pinephone", 200, "application/json",
		      "{\"name\": \"pinephone\", \"full_name\": \"elian/pinephone\","
		      " \"description\": \"Feature phone firmware for the PinePhone\", \"stargazers_count\": 12,"
		      " \"forks_count\": 3, \"html_url\": \"https://github.com/elian/pinephone\"}");
	CHECK(open_pr());
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Feature phone firmware for the PinePhone", WAIT));
	const struct ui_test_request *r = ui_test_requested(API "/repos/elian/pinephone");
	CHECK(r && !strcmp(r->method, "GET"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	CHECK(ui_test_shows("Repository"));
	CHECK(ui_test_shows("pinephone"));
	CHECK(ui_test_shows("https://github.com/elian/pinephone"));
	CHECK(ui_test_shows("12"));
	CHECK(ui_test_shows("3"));
	CHECK(ui_test_shows("README"));
	/* Back to the pull request */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Into main from github-app"));
}

TEST(issue_repository)
{
	serve_issue(issue_json("open", "null"));
	ui_test_serve(API "/repos/megi/linux", 200, "application/json", linux_repo);
	ui_test_serve(API "/repos/megi/linux/readme", 200, "application/vnd.github.raw+json", "# Megi's kernel\n");
	CHECK(open_issue());
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Linux kernel for the PinePhone", WAIT));
	CHECK(ui_test_shows("linux"));
	CHECK(ui_test_shows("https://github.com/megi/linux"));
	CHECK(ui_test_shows("120"));
	CHECK(ui_test_shows("45"));
	/* Its README too */
	CHECK(ui_test_tap_text("README"));
	CHECK(ui_test_wait_for("Megi's kernel", WAIT));
}

/* Discussions have none */
TEST(discussion_no_repository_button)
{
	serve_discussion(discussion);
	CHECK(open_discussion("Documented now."));
	CHECK(!ui_test_shows(LV_SYMBOL_DIRECTORY));
}

TEST(repository_offline)
{
	serve_issue(issue_json("open", "null"));
	CHECK(open_issue());
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	CHECK(!ui_test_shows("README"));
	CHECK(!ui_test_shows(UI_SYMBOL_BOOKMARK_REGULAR));
}

TEST(repository_not_found)
{
	serve_issue(issue_json("open", "null"));
	ui_test_serve(API "/repos/megi/linux", 404, "application/json", "{\"message\": \"Not Found\"}");
	CHECK(open_issue());
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("Repository not found", WAIT));
}

/* A revoked token: back to the form */
TEST(repository_token_refused)
{
	serve_issue(issue_json("open", "null"));
	CHECK(open_issue());
	ui_test_serve(API "/repos/megi/linux", 401, "application/json", "{\"message\": \"Bad credentials\"}");
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for("GitHub refused this token.", WAIT));
	CHECK(ui_test_shows("Sign in to GitHub"));
}

/* Bookmarks: repositories kept at the top of the Repositories tab, in a section of
 * their own, from a repository's page (the account's or another's) */
#define BOOKMARKS "/var/lib/app-data/github/bookmarks.json"

static char *read_all(const char *path)
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

/* None: the tab as before, without sections */
TEST(no_bookmarks)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	CHECK(!ui_test_shows("Bookmarks"));
	CHECK(!ui_test_shows("Your repositories"));
}

/* Bookmarked from its page: the page says so, the tab shows it first, kept */
TEST(bookmark_added)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_repos());
	CHECK(ui_test_tap_text("dotfiles"));
	/* In the header, at the top right: the outline until bookmarked, then solid */
	lv_obj_t *button = ui_test_find(UI_SYMBOL_BOOKMARK_REGULAR);
	CHECK(button);
	if (button) {
		lv_area_t a;
		lv_obj_get_coords(button, &a);
		CHECK(a.y2 < 150 && a.x1 > 560);
	}
	CHECK(!ui_test_shows("Bookmark"));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOOKMARK_REGULAR));
	CHECK(ui_test_shows(UI_SYMBOL_BOOKMARK));
	CHECK(!ui_test_shows(UI_SYMBOL_BOOKMARK_REGULAR));
	char *saved = read_all(BOOKMARKS);
	CHECK(saved && strstr(saved, "\"full_name\":\"elian/dotfiles\""));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Bookmarks"));
	CHECK(ui_test_shows("Your repositories"));
	/* At the top, above the list's own rows */
	lv_obj_t *bookmark = ui_test_find("elian/dotfiles"), *section = ui_test_find("Your repositories"),
		 *first = ui_test_find("pinephone");
	CHECK(bookmark && section && first);
	if (bookmark && section && first) {
		lv_area_t a, b, c;
		lv_obj_get_coords(bookmark, &a);
		lv_obj_get_coords(section, &b);
		lv_obj_get_coords(first, &c);
		CHECK(a.y1 < b.y1 && b.y1 < c.y1);
	}
	/* Kept: there when GitHub opens again */
	CHECK(ui_test_back());
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_shows("elian/dotfiles"));
}

/* Kept from before, shown at once (offline too); tapping one loads it */
TEST(bookmark_opens)
{
	signed_in();
	ui_test_write(BOOKMARKS,
		      "[{\"full_name\": \"megi/linux\", \"description\": \"Linux kernel for the PinePhone\"}]");
	ui_test_serve(API "/repos/megi/linux", 200, "application/json", linux_repo);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	CHECK(ui_test_shows("Bookmarks"));
	CHECK(ui_test_shows("megi/linux"));
	CHECK(ui_test_shows("Linux kernel for the PinePhone"));
	CHECK(ui_test_tap_text("megi/linux"));
	CHECK(ui_test_wait_for("https://github.com/megi/linux", WAIT));
	CHECK(ui_test_shows("120"));
	CHECK(ui_test_shows(UI_SYMBOL_BOOKMARK));
}

/* Removed from its page: gone from the tab and the file */
TEST(bookmark_removed)
{
	signed_in();
	ui_test_write(BOOKMARKS,
		      "[{\"full_name\": \"megi/linux\", \"description\": \"Linux kernel for the PinePhone\"},"
		      " {\"full_name\": \"elian/dotfiles\", \"description\": \"\"}]");
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(API "/repos/megi/linux", 200, "application/json", linux_repo);
	CHECK(open_repos());
	/* In their order */
	lv_obj_t *a = ui_test_find("megi/linux"), *b = ui_test_find("elian/dotfiles");
	CHECK(a && b);
	if (a && b) {
		lv_area_t ra, rb;
		lv_obj_get_coords(a, &ra);
		lv_obj_get_coords(b, &rb);
		CHECK(ra.y1 < rb.y1);
	}
	CHECK(ui_test_tap_text("megi/linux"));
	CHECK(ui_test_wait_for(UI_SYMBOL_BOOKMARK, WAIT));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOOKMARK));
	CHECK(ui_test_shows(UI_SYMBOL_BOOKMARK_REGULAR));
	CHECK(ui_test_back());
	CHECK(!ui_test_shows("megi/linux"));
	CHECK(ui_test_shows("elian/dotfiles"));
	char *saved = read_all(BOOKMARKS);
	CHECK(saved && !strstr(saved, "megi/linux") && strstr(saved, "elian/dotfiles"));
}

/* From a pull request's repository too */
TEST(bookmark_from_pr)
{
	serve_issue(issue_json("open", "null"));
	ui_test_serve(API "/repos/megi/linux", 200, "application/json", linux_repo);
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_issue());
	CHECK(ui_test_tap_text(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_wait_for(UI_SYMBOL_BOOKMARK_REGULAR, WAIT));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOOKMARK_REGULAR));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Repositories"));
	CHECK(ui_test_shows("megi/linux"));
	CHECK(ui_test_shows("Linux kernel for the PinePhone"));
}

/* In the phone's language: its own catalog (apps/github/po) */
#define SETTINGS "/var/lib/phone-ui/settings"

TEST(romanian_sign_in)
{
	ui_test_write(SETTINGS, "language=ro\n");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_shows("Conectare la GitHub"));
	CHECK(ui_test_shows("Conectare"));
	CHECK(ui_test_shows_part("generează un token"));
	CHECK(ui_test_shows_part(TOKEN_FILE));
}

TEST(romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	serve_pr(pr_json("open", false, false));
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("pinephone", WAIT));
	CHECK(ui_test_shows("Repository-uri"));
	CHECK(ui_test_shows("Notificări"));
	CHECK(ui_test_shows("Privat \xc2\xb7 Nix"));
	CHECK(ui_test_tap_text("Notificări"));
	CHECK(ui_test_wait_for("Pull request \xc2\xb7 Recenzie cerută", WAIT));
	CHECK(ui_test_shows("Issue \xc2\xb7 Menționat"));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Fixed, thanks!", WAIT));
	CHECK(ui_test_shows("Deschis"));
	CHECK(ui_test_shows("În main din github-app"));
	CHECK(ui_test_shows("bob a cerut modificări"));
	CHECK(ui_test_shows("alice a aprobat"));
}

/* A repository's open issues and pull requests, newest first, 30 a page: GitHub's
 * search (its total gives the pages), a bar of pages at the bottom: first, previous,
 * the numbers around the page shown, next, last */
#define SEARCH(kind, n)                                                                        \
	API "/search/issues?q=repo:elian/pinephone+is:" kind "+is:open&sort=created&order=desc"  \
	    "&per_page=30&page=" #n
#define ISSUES_PAGE(n) SEARCH("issue", n)
#define PULLS_PAGE(n) SEARCH("pr", n)

/* A page of the search's results: `count` issues numbered from `first` down, of
 * `total` */
static const char *search_json(int total, int first, int count)
{
	static char buf[65536];
	size_t n = snprintf(buf, sizeof(buf), "{\"total_count\": %d, \"incomplete_results\": false, \"items\": [",
			    total);
	for (int i = 0; i < count; i++) {
		int number = first - i;
		n += snprintf(buf + n, sizeof(buf) - n,
			      "%s{\"number\": %d, \"title\": \"Issue number %d\", \"user\": {\"login\": \"user%d\"},"
			      " \"created_at\": \"2026-11-0%dT10:00:00Z\", \"state\": \"open\"}",
			      i ? "," : "", number, number, number, 1 + i % 5);
	}
	snprintf(buf + n, sizeof(buf) - n, "]}");
	return buf;
}

static bool open_list(const char *which)
{
	return open_repos() && ui_test_tap_text("pinephone") && ui_test_tap_text(which);
}

/* The bar's button showing `text` (a page's number, a symbol), NULL if none */
static lv_obj_t *page_button(const char *text)
{
	lv_obj_t *found[4];
	unsigned n = ui_test_find_all(text, found, 4);
	for (unsigned i = 0; i < n; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (a.y1 > 1200) /* the bar, at the bottom */
			return lv_obj_get_parent(found[i]);
	}
	return NULL;
}

static bool tap_page(const char *text)
{
	lv_obj_t *b = page_button(text);
	if (!b) {
		printf("  no page button \"%s\"\n", text);
		return false;
	}
	ui_test_tap(b);
	return true;
}

static bool page_shown(const char *text)
{
	lv_obj_t *b = page_button(text);
	return b && lv_obj_has_state(b, LV_STATE_CHECKED);
}

static bool page_enabled(const char *text)
{
	lv_obj_t *b = page_button(text);
	return b && !lv_obj_has_state(b, LV_STATE_DISABLED);
}

/* One page: number, author, title; no bar */
TEST(issues_listed)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(3, 12, 3));
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Issue number 12", WAIT));
	const struct ui_test_request *r = ui_test_requested(ISSUES_PAGE(1));
	CHECK(r && !strcmp(r->method, "GET"));
	CHECK(r && strstr(r->headers, "Authorization: Bearer " TOKEN "\n"));
	/* The title: how many */
	CHECK(ui_test_shows("3 issues"));
	CHECK(ui_test_shows("#12 \xc2\xb7 user12"));
	CHECK(ui_test_shows("Issue number 10"));
	CHECK(!page_button(UI_SYMBOL_ANGLES_RIGHT));
	CHECK(!page_button("1"));
}

/* Four pages: their numbers, the first shown; going to one, next, last, first */
TEST(issues_pages)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(95, 95, 30));
	ui_test_serve(ISSUES_PAGE(3), 200, "application/json", search_json(95, 35, 30));
	ui_test_serve(ISSUES_PAGE(4), 200, "application/json", search_json(95, 5, 5));
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Issue number 95", WAIT));
	CHECK(page_shown("1"));
	CHECK(page_button("2") && page_button("3") && page_button("4"));
	CHECK(!page_button("5"));
	/* Nothing before the first */
	CHECK(!page_enabled(UI_SYMBOL_ANGLES_LEFT));
	CHECK(!page_enabled(LV_SYMBOL_LEFT));
	CHECK(page_enabled(LV_SYMBOL_RIGHT));
	CHECK(page_enabled(UI_SYMBOL_ANGLES_RIGHT));
	/* A page: its rows instead, at the top */
	CHECK(tap_page("3"));
	CHECK(ui_test_wait_for("Issue number 35", WAIT));
	CHECK(ui_test_requested(ISSUES_PAGE(3)));
	CHECK(!ui_test_shows("Issue number 95"));
	CHECK(page_shown("3") && !page_shown("1"));
	lv_obj_t *first = ui_test_find("Issue number 35");
	if (first) {
		lv_area_t a;
		lv_obj_get_coords(first, &a);
		CHECK(a.y1 < 400);
	}
	/* The last: nothing after it */
	CHECK(tap_page(UI_SYMBOL_ANGLES_RIGHT));
	CHECK(ui_test_wait_for("Issue number 5", WAIT));
	CHECK(page_shown("4"));
	CHECK(!page_enabled(LV_SYMBOL_RIGHT));
	CHECK(!page_enabled(UI_SYMBOL_ANGLES_RIGHT));
	/* Previous, then the first again */
	CHECK(tap_page(LV_SYMBOL_LEFT));
	CHECK(ui_test_wait_for("Issue number 35", WAIT));
	CHECK(page_shown("3"));
	CHECK(tap_page(UI_SYMBOL_ANGLES_LEFT));
	CHECK(ui_test_wait_for("Issue number 95", WAIT));
	CHECK(page_shown("1"));
}

/* Many pages: five numbers around the one shown; GitHub's search serves 1000 results
 * (34 pages) at most */
TEST(issues_many_pages)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(5000, 5000, 30));
	ui_test_serve(ISSUES_PAGE(34), 200, "application/json", search_json(5000, 4010, 30));
	ui_test_serve(ISSUES_PAGE(32), 200, "application/json", search_json(5000, 4070, 30));
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Issue number 5000", WAIT));
	/* All of them, though the pages stop at 1000 */
	CHECK(ui_test_shows("5000 issues"));
	CHECK(page_button("1") && page_button("5") && !page_button("6"));
	CHECK(tap_page(UI_SYMBOL_ANGLES_RIGHT));
	CHECK(ui_test_wait_for("Issue number 4010", WAIT));
	CHECK(ui_test_requested(ISSUES_PAGE(34)));
	CHECK(page_shown("34"));
	CHECK(page_button("30") && !page_button("29") && !page_button("35"));
	CHECK(tap_page("32"));
	CHECK(ui_test_wait_for("Issue number 4070", WAIT));
	CHECK(page_button("30") && page_button("34"));
}

/* A page that cannot load: said, the bar there to try again */
TEST(issues_page_offline)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(40, 40, 30));
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Issue number 40", WAIT));
	CHECK(tap_page("2"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	CHECK(page_button("2"));
	ui_test_serve(ISSUES_PAGE(2), 200, "application/json", search_json(40, 10, 10));
	CHECK(tap_page("2"));
	CHECK(ui_test_wait_for("Issue number 10", WAIT));
	CHECK(page_shown("2"));
	CHECK(!ui_test_shows("Cannot reach GitHub"));
}

/* Tapping one opens it */
TEST(issue_from_list)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(1, 12, 1));
	ui_test_serve(API "/repos/elian/pinephone/issues/12", 200, "application/json",
		      "{\"number\": 12, \"title\": \"Issue number 12\", \"state\": \"open\", \"user\": {\"login\": \"user12\"},"
		      " \"created_at\": \"2026-11-01T10:00:00Z\", \"body\": \"The first comment.\"}");
	ui_test_serve(API "/repos/elian/pinephone/issues/12/comments?*", 200, "application/json", "[]");
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Issue number 12", WAIT));
	CHECK(ui_test_shows("1 issue"));
	CHECK(ui_test_tap_text("Issue number 12"));
	CHECK(ui_test_wait_for("The first comment.", WAIT));
	CHECK(ui_test_shows("elian/pinephone#12"));
}

/* Pull requests: their own search, a draft said so; tapping one opens it */
TEST(pulls_listed)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(PULLS_PAGE(1), 200, "application/json",
		      "{\"total_count\": 2, \"items\": ["
		      " {\"number\": 8, \"title\": \"Work in progress\", \"user\": {\"login\": \"alice\"},"
		      "  \"created_at\": \"2026-11-05T08:00:00Z\", \"draft\": true,"
		      "  \"pull_request\": {\"url\": \"x\"}},"
		      " {\"number\": 7, \"title\": \"Add a GitHub app\", \"user\": {\"login\": \"elian\"},"
		      "  \"created_at\": \"2026-11-04T08:00:00Z\", \"draft\": false,"
		      "  \"pull_request\": {\"url\": \"x\"}}]}");
	ui_test_serve(PR, 200, "application/json", pr_json("open", false, false));
	ui_test_serve(COMMENTS "?*", 200, "application/json", "[]");
	ui_test_serve(PR "/reviews*", 200, "application/json", "[]");
	CHECK(open_list("Pull requests"));
	CHECK(ui_test_wait_for("Add a GitHub app", WAIT));
	CHECK(ui_test_shows("#7 \xc2\xb7 elian"));
	CHECK(ui_test_shows("#8 \xc2\xb7 alice \xc2\xb7 Draft"));
	CHECK(ui_test_shows("2 pull requests"));
	CHECK(ui_test_tap_text("Add a GitHub app"));
	CHECK(ui_test_wait_for("Into main from github-app", WAIT));
	CHECK(ui_test_requested(PR));
}

TEST(no_open_issues)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 200, "application/json", search_json(0, 0, 0));
	ui_test_serve(PULLS_PAGE(1), 200, "application/json", search_json(0, 0, 0));
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("No open issues", WAIT));
	CHECK(ui_test_shows("Issues"));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Pull requests"));
	CHECK(ui_test_wait_for("No open pull requests", WAIT));
}

TEST(issues_offline)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("Cannot reach GitHub", WAIT));
	CHECK(!page_button("1"));
}

/* A revoked token: back to the form, from however deep */
TEST(issues_token_refused)
{
	signed_in();
	ui_test_serve(REPOS, 200, "application/json", repos);
	ui_test_serve(ISSUES_PAGE(1), 401, "application/json", "{\"message\": \"Bad credentials\"}");
	CHECK(open_list("Issues"));
	CHECK(ui_test_wait_for("GitHub refused this token.", WAIT));
	CHECK(ui_test_shows("Sign in to GitHub"));
}
