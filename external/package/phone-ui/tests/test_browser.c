/*
 * Web Lite (browser/) with pages served by the tests (fake_curl.c). Pages are drawn
 * into textures, not labels: the tests check the title, the address, what was
 * requested, and pixels.
 */

#define _GNU_SOURCE /* open_memstream */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define SEARCH "https://lite.duckduckgo.com/lite/?q="
#define HTML "text/html; charset=utf-8"
/* A page that is one link: a tap anywhere on it follows it */
#define LINK_PAGE(title, href) "<html><head><title>" title "</title></head><body style=\"margin: 0\">" \
	"<a href=\"" href "\" style=\"display: block; height: 3000px\">Next</a></body></html>"

TEST_SETUP
{
	ui_test_start();
}

static void net_wait(void)
{
	ui_test_wait(300);
	ui_test_settle();
}

static lv_obj_t *address_field(void)
{
	return ui_test_find_widget(&lv_textarea_class);
}

static const char *address(void)
{
	lv_obj_t *ta = address_field();
	return ta ? lv_textarea_get_text(ta) : "";
}

/* Typed in the address bar, then Enter */
static bool go(const char *text)
{
	lv_obj_t *ta = address_field();
	if (!ta)
		return false;
	ui_test_tap(ta);
	lv_textarea_set_text(ta, "");
	bool ok = ui_test_type(text) && ui_test_type("\n");
	net_wait();
	return ok;
}

/* The middle of the page's view */
static lv_color_t page_pixel(void)
{
	return ui_test_pixel(360, 600);
}

static bool is_color(lv_color_t c, uint32_t rgb)
{
	return c.red == (rgb >> 16 & 0xff) && c.green == (rgb >> 8 & 0xff) && c.blue == (rgb & 0xff);
}

TEST(start_page)
{
	CHECK(ui_test_open("Web Lite"));
	ui_test_wait(200);
	CHECK(ui_test_shows("Web Lite"));
	CHECK_STR(address(), "");
	CHECK_INT(ui_test_request_count(), 0);
}

TEST(address_typed)
{
	ui_test_serve("https://example.org", 200, HTML,
	              "<html><head><title>Example Domain</title></head>"
	              "<body style=\"background: #ff0000\"><h1>Example</h1></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("example.org"));
	CHECK(ui_test_requested("https://example.org") != NULL);
	CHECK(ui_test_shows("Example Domain"));
	CHECK_STR(address(), "https://example.org");
	CHECK(is_color(page_pixel(), 0xff0000));
}

TEST(words_searched)
{
	ui_test_serve(SEARCH "*", 200, HTML, "<title>DuckDuckGo</title>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("pine phone"));
	CHECK(ui_test_requested(SEARCH "pine%20phone") != NULL);
	CHECK(ui_test_shows("DuckDuckGo"));
}

TEST(loading_and_stop)
{
	ui_test_serve("https://example.org/", 200, HTML, "<title>Example</title>");
	CHECK(ui_test_open("Web Lite"));
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Loading..."));
	CHECK(ui_test_tap_text(LV_SYMBOL_CLOSE)); /* stop */
	CHECK(ui_test_shows("Web Lite"));
	ui_test_network_hold(false);
	net_wait();
	CHECK(!ui_test_shows("Example"));
}

TEST(offline)
{
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Not loaded"));
	CHECK_STR(address(), "https://example.org/");
}

TEST(not_found)
{
	ui_test_serve("https://example.org/nope", 404, HTML, "<title>404</title>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/nope"));
	CHECK(ui_test_shows("Not loaded"));
}

TEST(not_a_web_page)
{
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("ftp://example.org/file"));
	CHECK(ui_test_shows("Not loaded"));
	CHECK_INT(ui_test_request_count(), 0);
}

TEST(file_kinds)
{
	ui_test_serve("https://example.org/a.pdf", 200, "application/pdf", "%PDF-1.4");
	ui_test_serve("https://example.org/a.txt", 200, "text/plain", "plain <text> & more");
	size_t n;
	void *png = ui_test_png(200, 200, 0x0000ff, &n);
	ui_test_serve_data("https://example.org/a.png", 200, "image/png", png, n);
	free(png);
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/a.pdf"));
	CHECK(ui_test_shows("Not loaded"));
	CHECK(go("https://example.org/a.txt"));
	CHECK(ui_test_shows("Web Lite"));
	CHECK(go("https://example.org/a.png"));
	ui_test_wait(1500); /* the image fetched again, then laid out */
	CHECK(ui_test_request_count() >= 4);
}

/* Back and reload go to the address after the redirect */
TEST(redirect)
{
	ui_test_serve_redirect("http://example.org/", "https://example.org/");
	ui_test_serve("https://example.org/", 200, HTML, "<title>Secure</title>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("http://example.org/"));
	CHECK(ui_test_shows("Secure"));
	CHECK_STR(address(), "https://example.org/");
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	net_wait();
	CHECK_STR(ui_test_request(-1)->url, "https://example.org/");
}

TEST(links_and_back)
{
	ui_test_serve("https://example.org/", 200, HTML, LINK_PAGE("First", "/second"));
	ui_test_serve("https://example.org/second", 200, HTML, LINK_PAGE("Second", "#top"));
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("First"));
	ui_test_tap_at(360, 600);
	net_wait();
	CHECK(ui_test_requested("https://example.org/second") != NULL);
	CHECK(ui_test_shows("Second"));
	/* A place on the same page: not fetched */
	unsigned n = ui_test_request_count();
	ui_test_tap_at(360, 600);
	net_wait();
	CHECK_INT(ui_test_request_count(), n);

	lv_obj_t *back[4];
	unsigned count = ui_test_find_all(LV_SYMBOL_LEFT, back, 4);
	CHECK(count >= 2);
	if (count < 2)
		return;
	ui_test_tap(back[count - 1]); /* the bar's, below the page */
	net_wait();
	CHECK(ui_test_shows("First"));
	ui_test_tap(back[count - 1]);
	net_wait();
	CHECK(ui_test_shows("Web Lite")); /* the start page */
	ui_test_tap(back[count - 1]);
	CHECK(ui_at_home());
}

/* Style sheets before the page shows, @import too; images after */
TEST(style_sheets_and_images)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><title>Styled</title><link rel=\"stylesheet\" href=\"/a.css\"></head>"
	              "<body style=\"margin: 0\"><img src=\"/wide.png\"><img src=\"img/b.png\"></body></html>");
	ui_test_serve("https://example.org/a.css", 200, "text/css", "@import \"b.css\"; h1 { color: red }");
	ui_test_serve("https://example.org/b.css", 200, "text/css", "body { background: #00ff00 }");
	size_t n;
	void *png = ui_test_png(1440, 600, 0x0000ff, &n); /* scaled down to the screen */
	ui_test_serve_data("https://example.org/wide.png", 200, "image/png", png, n);
	free(png);
	ui_test_serve("https://example.org/img/b.png", 200, "image/png", "not a PNG");
	CHECK(ui_test_open("Web Lite"));
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	ui_test_network_hold(false);
	net_wait();
	CHECK(ui_test_requested("https://example.org/a.css") != NULL);
	CHECK(ui_test_requested("https://example.org/b.css") != NULL);
	CHECK(ui_test_shows("Styled"));
	ui_test_wait(1500);
	CHECK(ui_test_requested("https://example.org/wide.png") != NULL);
	CHECK(ui_test_requested("https://example.org/img/b.png") != NULL);
	/* The image at the top, the page's background below it */
	CHECK(is_color(ui_test_pixel(360, 200), 0x0000ff));
	CHECK(is_color(ui_test_pixel(360, 1100), 0x00ff00));
}

static char *base64(const void *data, size_t n)
{
	static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char *s = data;
	char *out = malloc(n * 4 / 3 + 5), *o = out;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = s[i] << 16 | (i + 1 < n ? s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
		*o++ = a[v >> 18 & 63];
		*o++ = a[v >> 12 & 63];
		*o++ = i + 1 < n ? a[v >> 6 & 63] : '=';
		*o++ = i + 2 < n ? a[v & 63] : '=';
	}
	*o = '\0';
	return out;
}

/* A JPEG, and a PNG in the page itself */
TEST(jpeg_and_data_images)
{
	uint8_t *px = malloc(400 * 300 * 4);
	for (int i = 0; i < 400 * 300; i++)
		((uint32_t *)px)[i] = 0xffff0000; /* BGRX in memory: red */
	char *jpeg = NULL;
	size_t jpeg_size = 0;
	FILE *f = open_memstream(&jpeg, &jpeg_size);
	CHECK(ui_jpeg_write(f, px, 400, 300, 400 * 4, 95, true));
	fclose(f);
	free(px);
	ui_test_serve_data("https://example.org/photo.jpg", 200, "image/jpeg", jpeg, jpeg_size);
	free(jpeg);

	size_t n;
	void *png = ui_test_png(60, 60, 0x00ff00, &n);
	char *b64 = base64(png, n);
	free(png);
	char html[4096];
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\"><img src=\"/photo.jpg\"><br>"
	         "<img src=\"data:image/png;base64,%s\" width=\"200\" height=\"200\"></body></html>", b64);
	free(b64);
	ui_test_serve("https://example.org/", 200, HTML, html);
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(1500);
	CHECK(ui_test_requested("https://example.org/photo.jpg") != NULL);
	CHECK_INT(ui_test_request_count(), 2); /* not the data: one */
	lv_color_t c = ui_test_pixel(100, 200);
	CHECK(c.red > 200 && c.green < 60 && c.blue < 60);
}

/* A long page: drawn in tiles as it scrolls */
TEST(long_page_scrolled)
{
	char *html = malloc(64 * 1024), *p = html;
	p += sprintf(p, "<html><head><title>Long</title></head><body>");
	for (int i = 0; i < 300; i++)
		p += sprintf(p, "<p>Paragraph %d of a long page, with enough words to wrap a line.</p>", i);
	sprintf(p, "</body></html>");
	ui_test_serve("https://example.org/long", 200, HTML, html);
	free(html);
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/long"));
	CHECK(ui_test_shows("Long"));
	for (int i = 0; i < 5; i++)
		ui_test_swipe(360, 1100, 360, 300, 300);
	ui_test_wait(500);
	lv_obj_t *tiles[32];
	CHECK(ui_test_find_widgets(&lv_3dtexture_class, tiles, 32) > 0);
}

/* Closed while a page loads: nothing arrives in the freed screen */
TEST(closed_while_loading)
{
	ui_test_serve("https://example.org/", 200, HTML, "<title>Example</title>");
	CHECK(ui_test_open("Web Lite"));
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	net_wait();
	CHECK(ui_at_home());
}
