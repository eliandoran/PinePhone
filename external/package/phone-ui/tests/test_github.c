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
#include "json.h"
#include "ui.h"
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

/* Tapping an unread notification marks its thread read (a release: opens nothing) */
TEST(tap_marks_read)
{
	signed_in();
	ui_test_serve(API "/notifications*", 200, "application/json", notifications);
	ui_test_serve(API "/notifications/threads/*", 205, "text/plain", "");
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("v9.3", WAIT));
	CHECK(ui_test_tap_text("v9.3"));
	ui_test_wait(300);
	const struct ui_test_request *r = ui_test_requested(API "/notifications/threads/103");
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
	return ui_test_open("GitHub") && ui_test_wait_for("Add a GitHub app", WAIT) &&
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

/* Other notifications open nothing yet: only marked read */
TEST(issue_opens_nothing)
{
	serve_pr(pr_json("open", false, false));
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_wait_for("Modem does not wake from suspend", WAIT));
	CHECK(ui_test_tap_text("Modem does not wake from suspend"));
	CHECK(ui_test_shows("Add a GitHub app"));
	CHECK(!ui_test_requested(API "/repos/*"));
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
	CHECK(ui_test_open("GitHub"));
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
	CHECK(ui_test_open("GitHub"));
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
	return ui_test_open("GitHub") && ui_test_wait_for("Add a GitHub app", WAIT) &&
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
	CHECK(ui_test_open("GitHub"));
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
	CHECK(ui_test_open("GitHub"));
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
	return ui_test_open("GitHub") && ui_test_wait_for("Add a GitHub app", WAIT) &&
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
