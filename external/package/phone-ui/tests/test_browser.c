/*
 * Web Lite (browser/) with pages served by the tests (fake_curl.c). Pages are drawn
 * into textures, not labels: the tests check the title, the address, what was
 * requested, and pixels.
 */

#define _GNU_SOURCE /* open_memstream */
#include <curl/curl.h>
#include <webp/encode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
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

/* A column flex container's items stretched to its width, not to its widest item's
 * (triliumnotes.org's sections went off the screen to the right) */
TEST(column_flex_within_screen)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><body style=\"margin: 0\"><main style=\"display: flex; flex-direction: column\">"
	              "<div style=\"width: 700px; height: 20px\"></div>"
	              "<section style=\"display: flex; justify-content: flex-end; height: 400px; background: #00ff00\">"
	              "<div style=\"width: 40px; background: #ff0000\"></div></section></main></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(500);
	CHECK(is_color(ui_test_pixel(100, 500), 0x00ff00));
	/* Its right end at the screen's */
	CHECK(is_color(ui_test_pixel(700, 500), 0xff0000));
}

/* The page's title, not that of an SVG image in it */
TEST(svg_title_not_the_page_title)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><title>Notes app</title></head><body>"
	              "<svg><title>An icon</title></svg><p>Text</p></body></html>");
	ui_test_serve("https://example.org/untitled", 200, HTML,
	              "<html><body><svg><title>Another icon</title></svg><p>Text</p></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Notes app"));
	CHECK(!ui_test_shows("An icon"));
	CHECK(go("https://example.org/untitled"));
	CHECK(!ui_test_shows("Another icon"));
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

/* WebP images (triliumnotes.org's screenshots): lossy and lossless, the wider one
 * decoded at the screen's width */
TEST(webp_images)
{
	uint8_t *px = malloc(1440 * 600 * 3);
	for (int i = 0; i < 1440 * 600; i++)
		memcpy(px + i * 3, "\x00\x00\xff", 3); /* RGB: blue */
	uint8_t *lossless = NULL;
	size_t lossless_size = WebPEncodeLosslessRGB(px, 1440, 600, 1440 * 3, &lossless);
	for (int i = 0; i < 400 * 300; i++)
		memcpy(px + i * 3, "\xff\x00\x00", 3); /* red */
	uint8_t *lossy = NULL;
	size_t lossy_size = WebPEncodeRGB(px, 400, 300, 400 * 3, 90, &lossy);
	free(px);
	CHECK(lossless_size > 0 && lossy_size > 0);
	ui_test_serve_data("https://example.org/wide.webp", 200, "image/webp", lossless, lossless_size);
	ui_test_serve_data("https://example.org/photo.webp", 200, "image/webp", lossy, lossy_size);
	WebPFree(lossless);
	WebPFree(lossy);
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><body style=\"margin: 0\"><img src=\"/photo.webp\"><br><img src=\"/wide.webp\"></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(1500);
	/* The photo at the top (400 x 300 CSS pixels: 600 x 450), the wide one under it */
	lv_color_t c = ui_test_pixel(300, 380);
	CHECK(c.red > 200 && c.green < 60 && c.blue < 60);
	CHECK(is_color(ui_test_pixel(700, 380), 0xffffff));
	CHECK(is_color(ui_test_pixel(360, 900), 0x0000ff));
}

/* A picture at width: 100%; height: 100% in a frame of auto height (triliumnotes.org's
 * screenshots): its height is auto, it keeps its ratio, what follows comes under it.
 * In a block, then as a column flex item, where the frame's minimum height is not
 * the picture's natural one */
static void framed_picture(bool flex)
{
	size_t n;
	void *png = ui_test_png(1600, 400, 0xff0000, &n); /* 480 x 120 CSS pixels: 720 x 180 */
	ui_test_serve_data("https://example.org/wide.png", 200, "image/png", png, n);
	free(png);
	char html[512];
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\"><main style=\"display: %s; "
	         "flex-direction: column\"><div style=\"overflow: hidden\"><img src=\"/wide.png\" "
	         "style=\"display: block; width: 100%%; height: 100%%\"></div>"
	         "<div style=\"height: 300px; background: #00ff00\"></div></main></body></html>",
	         flex ? "flex" : "block");
	ui_test_serve("https://example.org/", 200, HTML, html);
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(1500);
	/* The page from y = 152: the picture to 332, then the green block */
	CHECK(is_color(ui_test_pixel(360, 250), 0xff0000));
	CHECK(is_color(ui_test_pixel(360, 400), 0x00ff00));
}

TEST(picture_height_percentage_of_auto)
{
	framed_picture(false);
}

TEST(picture_height_percentage_in_column_flex)
{
	framed_picture(true);
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

/* In Romanian: the bar's placeholder, loading, and the titles of the start and error pages
 * (their text is drawn into the page, not checked) */
TEST(romanian)
{
	ui_language_set("ro", NULL);
	ui_test_settle();
	ui_test_serve("https://example.org/", 200, HTML, "<html><body>No title</body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK_STR(lv_textarea_get_placeholder_text(address_field()), "Adresă sau căutare");
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Se încarcă..."));
	ui_test_network_hold(false);
	net_wait();
	CHECK(ui_test_shows("Web Lite"));
	CHECK(go("ftp://example.org/file"));
	CHECK(ui_test_shows("Neîncărcată"));
	CHECK(go("https://example.org/missing"));
	CHECK(ui_test_shows("Neîncărcată"));
}

/* The box of the colored pixels in the page's view (emoji: the text is white on
 * black), false if none */
static bool colored_box(lv_area_t *box)
{
	bool any = false;
	for (int32_t y = 150; y < 1300; y++)
		for (int32_t x = 0; x < 720; x++) {
			lv_color_t c = ui_test_pixel(x, y);
			int hi = LV_MAX(c.red, LV_MAX(c.green, c.blue)), lo = LV_MIN(c.red, LV_MIN(c.green, c.blue));
			if (hi - lo <= 100)
				continue;
			if (!any) {
				box->x1 = box->x2 = x;
				box->y1 = box->y2 = y;
				any = true;
			}
			box->x1 = LV_MIN(box->x1, x);
			box->x2 = LV_MAX(box->x2, x);
			box->y1 = LV_MIN(box->y1, y);
			box->y2 = LV_MAX(box->y2, y);
		}
	return any;
}

/* Emoji drawn in their colors, a joined one (woman, joiner, laptop) as its one
 * picture: about as wide as high */
TEST(emoji)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><title>Emoji</title></head>"
	              "<body style=\"margin: 0; background: #000; color: #fff; font-size: 32px\">"
	              "<p>Apple \xf0\x9f\x8d\x8e</p></body></html>");
	ui_test_serve("https://example.org/joined", 200, HTML,
	              "<html><head><title>Joined</title></head>"
	              "<body style=\"margin: 0; background: #000; color: #fff; font-size: 32px\">"
	              "<p>\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb</p></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Emoji"));
	lv_area_t box;
	CHECK(colored_box(&box));
	CHECK(go("https://example.org/joined"));
	CHECK(ui_test_shows("Joined"));
	CHECK(colored_box(&box));
	CHECK(lv_area_get_width(&box) < lv_area_get_height(&box) * 3 / 2);
}
