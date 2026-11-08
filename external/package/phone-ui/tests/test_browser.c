/*
 * Web Lite (browser/) with pages served by the tests (fake_curl.c). Pages are drawn
 * into textures, not labels: the tests check the title, the address, what was
 * requested, and pixels.
 */

#define _GNU_SOURCE /* open_memstream */
#include <curl/curl.h>
#include <webp/encode.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/browser/browser.h"
#include "i18n.h"
#include "test.h"
#include "ui.h"
#include "ui_test.h"

#define SEARCH "https://lite.duckduckgo.com/lite/?q="
#define HTML "text/html; charset=utf-8"
/* A page that is one link: a tap anywhere on it follows it */
#define LINK_PAGE(title, href) "<html><head><title>" title "</title></head><body style=\"margin: 0\">" \
	"<a href=\"" href "\" style=\"display: block; height: 3000px\">Next</a></body></html>"

/* In the light theme: pages as they are (the dark one: dark_pages_in_the_dark_theme) */
TEST_SETUP
{
	ui_test_reset();
	CHECK(ui_test_write("/var/lib/phone-ui/settings", "light=1\n"));
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

/* Style sheets <link>ed in the head are asked for before the page is built: a page
 * with one that fails shows unstyled, a <base> resolves them, one in the body styles it */
TEST(linked_style_sheets)
{
	ui_test_serve("https://example.org/a/", 200, HTML,
	              "<html><head><title>Base</title><base href=\"https://example.org/b/\">"
	              "<link rel=\"stylesheet\" href=\"s.css\"></head><body></body></html>");
	ui_test_serve("https://example.org/b/s.css", 200, "text/css", "body { background: #00ff00 }");
	ui_test_serve("https://example.org/c/", 200, HTML,
	              "<html><head><title>Missing</title><link rel=\"stylesheet\" href=\"gone.css\"></head>"
	              "<body style=\"background: #0000ff\"><link rel=\"stylesheet\" href=\"/late.css\"></body></html>");
	ui_test_serve("https://example.org/c/gone.css", 404, "text/html", "no");
	ui_test_serve("https://example.org/late.css", 200, "text/css", "body { background: #ff0000 !important }");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/a/"));
	CHECK(ui_test_shows("Base"));
	CHECK(ui_test_requested("https://example.org/a/s.css") == NULL);
	CHECK(is_color(page_pixel(), 0x00ff00));
	CHECK(go("https://example.org/c/"));
	CHECK(ui_test_shows("Missing"));
	CHECK(ui_test_requested("https://example.org/c/gone.css") != NULL);
	CHECK(is_color(page_pixel(), 0xff0000));
}

/* A long page (Wikipedia's article on Wikipedia: 2.4 MB): built on a thread of its own
 * (browser.c's BUILD_THREAD_BYTES), styled, the page before it shown meanwhile; laid out
 * again there once its picture arrives */
static char *long_page(const char *title)
{
	size_t n = 300 << 10;
	char *html = malloc(n + 512);
	char *s = html + sprintf(html, "<html><head><title>%s</title><link rel=\"stylesheet\" href=\"/long.css\">"
	                         "</head><body style=\"margin: 0\"><img src=\"/top.png\">", title);
	while ((size_t)(s - html) < n)
		s = stpcpy(s, "<p class=\"para\">Lorem ipsum dolor sit amet, <a href=\"/x\">consectetur</a></p>\n");
	strcpy(s, "</body></html>");
	return html;
}

static bool picture_shown(void)
{
	return is_color(page_pixel(), 0x0000ff);
}

TEST(long_page_built_on_a_thread)
{
	char *html = long_page("Long");
	ui_test_serve("https://example.org/long", 200, HTML, html);
	free(html);
	size_t n;
	void *png = ui_test_png(720, 600, 0x0000ff, &n); /* from y = 152 to 1052, narrowed to the page */
	ui_test_serve_data("https://example.org/top.png", 200, "image/png", png, n);
	free(png);
	ui_test_serve("https://example.org/long.css", 200, "text/css", "body { background: #00ff00 }");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/long"));
	CHECK(ui_test_wait_for("Long", 20000));
	CHECK(ui_test_requested("https://example.org/long.css") != NULL);
	CHECK(ui_test_wait_until(picture_shown, 20000));
	CHECK(is_color(ui_test_pixel(712, 1100), 0x00ff00)); /* beside the text under it */
}

/* Left while it is built: the thread destroys it, the page gone to shows */
TEST(long_page_left_while_built)
{
	char *html = long_page("Long");
	ui_test_serve("https://example.org/long", 200, HTML, html);
	free(html);
	ui_test_serve("https://example.org/long.css", 200, "text/css", "body { background: #00ff00 }");
	ui_test_serve("https://example.org/short", 200, HTML, "<html><head><title>Short</title></head></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/long"));
	CHECK(go("https://example.org/short"));
	CHECK(ui_test_wait_for("Short", 20000));
	ui_test_wait(2000);
	CHECK(ui_test_shows("Short"));
	CHECK(!ui_test_shows("Long"));
}

/* Wikipedia's buttons: icons sized by calc() (var(), rem and px, max()), their labels
 * hidden in a media query of calc(), quiet buttons in a rule that also lists :enabled */
TEST(calc_and_enabled)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><style>"
	              ".box { width: calc(50% + 2rem - 32px); height: calc(max(calc(var(--s, 1rem) + 4px), 10px) * 10);"
	              " background: #ff0000 }"
	              "@media all and (max-width: calc(1120px - 1px)) { .hide { display: none } }"
	              ".q:enabled, .q.on { background: #0000ff }"
	              "</style></head><body style=\"margin: 0\"><div class=\"box\"></div>"
	              "<div class=\"hide\" style=\"height: 200px; background: #00ff00\"></div>"
	              "<div class=\"q on\" style=\"height: 100px\"></div></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	/* The page from y = 152: the box 240 x 200 CSS pixels (360 x 300), then the blue one */
	CHECK(is_color(ui_test_pixel(300, 300), 0xff0000));
	CHECK(is_color(ui_test_pixel(420, 300), 0xffffff));
	CHECK(is_color(ui_test_pixel(300, 520), 0x0000ff));
}

/* Wikipedia's header: an icon button 44px by min-width and min-height, its 20px icon
 * in its middle, the logo after it rather than over it */
TEST(min_size_buttons_in_a_flex_row)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><body style=\"margin: 0\"><div style=\"display: flex; align-items: center; height: 54px\">"
	              "<span style=\"display: inline-flex; align-items: center; justify-content: center; min-width: 44px;"
	              " min-height: 44px\"><span style=\"display: inline-block; width: 20px; height: 20px;"
	              " background: #ff0000\"></span></span>"
	              "<span style=\"display: inline-block; width: 50px; height: 20px; background: #00ff00\"></span>"
	              "</div></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	/* From y = 152: the icon at 12 to 32 x 17 to 37 CSS pixels, the next one from x = 44 */
	CHECK(is_color(ui_test_pixel(33, 192), 0xff0000));
	CHECK(is_color(ui_test_pixel(100, 192), 0x00ff00));
	CHECK(is_color(ui_test_pixel(33, 165), 0xffffff));
}

/* SVG images (Wikipedia's logo): fetched, without a size of their own (the viewBox's),
 * and in a data: URI percent-encoded rather than base64 */
TEST(svg_images)
{
	ui_test_serve("https://example.org/logo.svg", 200, "image/svg+xml",
	              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
	              "fill=\"none\" viewBox=\"0 0 100 50\"><path fill=\"#0000ff\" d=\"M0 0h50v50H0z\"/></svg>");
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><body style=\"margin: 0\"><img src=\"/logo.svg\" style=\"display: block; "
	              "width: 400px; height: 200px\"><img src=\"data:image/svg+xml;utf8,<svg "
	              "xmlns='http://www.w3.org/2000/svg' width='20' height='20'><rect width='20' "
	              "height='20' fill='%23ff0000'/></svg>\" style=\"display: block; width: 200px; "
	              "height: 200px\"></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(1500);
	CHECK(ui_test_requested("https://example.org/logo.svg") != NULL);
	/* The page from y = 152: the logo to 452 (its left half drawn), then the red square */
	CHECK(is_color(ui_test_pixel(150, 300), 0x0000ff));
	CHECK(is_color(ui_test_pixel(450, 300), 0xffffff));
	CHECK(is_color(ui_test_pixel(150, 600), 0xff0000));
}

/* Icons drawn as a mask over the element's background color (Wikipedia's language
 * and edit icons: -webkit-mask-image and mask-image, an SVG), not as a square of it */
TEST(mask_images)
{
	ui_test_serve("https://example.org/icon.svg", 200, "image/svg+xml",
	              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
	              "<g fill=\"#54595d\"><path d=\"M0 0h10v20H0z\"/></g></svg>");
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><style>.icon { display: inline-block; width: 200px; height: 200px; "
	              "background-color: #ff0000; -webkit-mask-image: url(/icon.svg); mask-image: "
	              "url(/icon.svg); mask-size: 200px }</style></head><body style=\"margin: 0\">"
	              "<span class=\"icon\"></span></body></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_wait(1500);
	CHECK(ui_test_requested("https://example.org/icon.svg") != NULL);
	/* 300 x 300 from y = 152: the mask's left half in the background's red */
	CHECK(is_color(ui_test_pixel(100, 300), 0xff0000));
	CHECK(is_color(ui_test_pixel(250, 300), 0xffffff));
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

/* In the dark theme, pages are drawn dark as Books' are: the paper dark gray, a red
 * page lighter red, the view around it the paper's color */
TEST(dark_pages_in_the_dark_theme)
{
	ui_test_serve("https://example.org", 200, HTML,
	              "<html><head><title>Red</title></head><body style=\"background: #ff0000\">x</body></html>");
	ui_set_light(false, NULL);
	ui_test_wait(300);
	CHECK(!ui_light);
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("example.org"));
	CHECK(ui_test_shows("Red"));
	lv_color_t c = page_pixel();
	CHECK(c.red == 255 && c.green == c.blue && c.green > 60 && c.green < 200);
	ui_test_serve("https://example.org/plain", 200, HTML, "<html><head><title>Plain</title></head><body>x</body></html>");
	CHECK(go("example.org/plain"));
	CHECK(ui_test_shows("Plain"));
	CHECK(is_color(page_pixel(), PAGE_DARK_PAPER));
}

/*
 * Pages drawn as Web Lite draws them (page.cpp), 720 px wide from their top, into a
 * buffer of the tests': what CSS draws (markers, borders, corners, gradients, text
 * decorations), checked pixel by pixel
 */

#define CANVAS_H 600
static uint8_t canvas[720 * CANVAS_H * 4];

/* `body` in a page without margins, its images (`image`: a BGRA w x h of one color
 * 0xRRGGBB, for every one it asks for), laid out and drawn */
static struct page *page_drawn(const char *body, uint32_t image, int w, int h)
{
	char html[4096];
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\">%s</body></html>", body);
	struct page *p = page_create(html, strlen(html), "https://example.org/", 720, 1440);
	const char *url;
	while ((url = page_next_wanted(p, PAGE_IMAGE))) {
		uint8_t *px = malloc((size_t)w * h * 4);
		for (int i = 0; i < w * h; i++) {
			px[i * 4] = image & 0xff;
			px[i * 4 + 1] = image >> 8 & 0xff;
			px[i * 4 + 2] = image >> 16 & 0xff;
			px[i * 4 + 3] = 0xff;
		}
		page_set_image(p, url, px, w, h, w, h);
	}
	page_relayout(p);
	page_draw(p, canvas, 720, CANVAS_H, 0);
	return p;
}

/* The canvas at (x, y) as 0xRRGGBB */
static uint32_t at(int x, int y)
{
	const uint8_t *px = canvas + ((size_t)y * 720 + x) * 4;
	return (uint32_t)px[2] << 16 | px[1] << 8 | px[0];
}

static bool near(uint32_t c, uint32_t rgb, int tolerance)
{
	for (int s = 0; s < 24; s += 8)
		if (abs((int)(c >> s & 0xff) - (int)(rgb >> s & 0xff)) > tolerance)
			return false;
	return true;
}

/* The box of the pixels of about `rgb` within x0..x1 - 1, y0..y1 - 1; false if none */
static bool box_of(uint32_t rgb, int x0, int y0, int x1, int y1, lv_area_t *box)
{
	bool any = false;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			if (!near(at(x, y), rgb, 40))
				continue;
			if (!any)
				*box = (lv_area_t){ x, y, x, y };
			any = true;
			box->x1 = LV_MIN(box->x1, x);
			box->x2 = LV_MAX(box->x2, x);
			box->y1 = LV_MIN(box->y1, y);
			box->y2 = LV_MAX(box->y2, y);
		}
	return any;
}

/* List markers in the list's color: a disc, a circle (a ring), a square; an image */
TEST(list_markers)
{
	struct page *p = page_drawn(
		"<style>ul { margin: 0; padding-left: 80px; color: #ff0000; font-size: 32px } "
		"li { height: 60px }</style>"
		"<ul style=\"list-style-type: disc\"><li>x</li></ul>"
		"<ul style=\"list-style-type: circle\"><li>x</li></ul>"
		"<ul style=\"list-style-type: square\"><li>x</li></ul>"
		"<ul style=\"list-style-image: url(/marker.png)\"><li>x</li></ul>",
		0x00ff00, 12, 12);
	/* Each list 90 px high, its marker left of the text (from x = 120) */
	lv_area_t disc, circle, square, image;
	CHECK(box_of(0xff0000, 0, 0, 120, 90, &disc));
	CHECK(box_of(0xff0000, 0, 90, 120, 180, &circle));
	CHECK(box_of(0xff0000, 0, 180, 120, 270, &square));
	CHECK(box_of(0x00ff00, 0, 270, 120, 360, &image));
	CHECK(near(at((disc.x1 + disc.x2) / 2, (disc.y1 + disc.y2) / 2), 0xff0000, 40));
	CHECK(near(at(disc.x1, disc.y1), 0xffffff, 40)); /* round */
	CHECK(near(at((circle.x1 + circle.x2) / 2, (circle.y1 + circle.y2) / 2), 0xffffff, 40));
	CHECK(near(at(square.x1, square.y1), 0xff0000, 40)); /* square */
	CHECK(near(at((square.x1 + square.x2) / 2, (square.y1 + square.y2) / 2), 0xff0000, 0));
	page_destroy(p);
}

/* Borders each in their color, rounded corners, gradients: linear through its stops
 * (the first and last colors before and after them), radial and conic as their
 * middle color */
TEST(borders_corners_and_gradients)
{
	struct page *p = page_drawn(
		"<div style=\"width: 100px; height: 100px; border: 10px solid; "
		"border-color: #ff0000 #00ff00 #0000ff #ffff00\"></div>"
		"<div style=\"width: 200px; height: 100px; background: #0000ff; border-radius: 40px\"></div>"
		"<div style=\"width: 300px; height: 40px; "
		"background: linear-gradient(to right, #ff0000 20%, #00ff00 50%, #0000ff 80%)\"></div>"
		"<div style=\"width: 300px; height: 40px; background: radial-gradient(#ff0000, #0000ff)\"></div>"
		"<div style=\"width: 300px; height: 40px; background: conic-gradient(#ff0000, #0000ff)\"></div>",
		0, 0, 0);
	/* The bordered box: 180 px square */
	CHECK_INT(at(90, 5), 0xff0000);
	CHECK_INT(at(175, 90), 0x00ff00);
	CHECK_INT(at(90, 175), 0x0000ff);
	CHECK_INT(at(5, 90), 0xffff00);
	CHECK_INT(at(90, 90), 0xffffff);
	/* Rounded, from y = 180 to 330: corners cut, edges straight */
	CHECK_INT(at(2, 182), 0xffffff);
	CHECK_INT(at(297, 327), 0xffffff);
	CHECK_INT(at(150, 255), 0x0000ff);
	CHECK_INT(at(1, 255), 0x0000ff);
	CHECK_INT(at(150, 181), 0x0000ff);
	/* The linear gradient, 330 to 390: red, green in the middle, blue */
	CHECK_INT(at(10, 360), 0xff0000);
	CHECK(near(at(225, 360), 0x00ff00, 8));
	CHECK_INT(at(440, 360), 0x0000ff);
	CHECK(near(at(157, 360), 0x808000, 8));
	CHECK(near(at(292, 360), 0x008080, 8));
	/* Radial and conic: halfway */
	CHECK(near(at(225, 420), 0x800080, 4));
	CHECK(near(at(225, 480), 0x800080, 4));
	page_destroy(p);
}

/* Where the decoration's line is (the first row the text's whole width is in it)
 * relative to the top of dots in the text's color, in rows `y0` to `y1` - 1; INT_MIN if
 * none */
static int decoration_at(int y0, int y1)
{
	lv_area_t box;
	if (!box_of(0xff0000, 0, y0, 720, y1, &box))
		return INT32_MIN;
	int line = -1, dots = -1;
	for (int y = box.y1; y <= box.y2; y++) {
		bool full = true, any = false;
		for (int x = box.x1 + 1; x < box.x2; x++) {
			bool red = near(at(x, y), 0xff0000, 60);
			full &= red;
			any |= red;
		}
		if (full && line < 0)
			line = y;
		else if (any && !full && dots < 0)
			dots = y;
	}
	return line < 0 || dots < 0 ? INT32_MIN : line - dots;
}

/* Text decorations drawn across the text, spaces included: under it, through it,
 * over it; none without */
TEST(text_decorations)
{
	struct page *p = page_drawn(
		"<style>div { font-size: 40px; color: #ff0000; height: 100px; line-height: 100px }</style>"
		"<div>. . . .</div>"
		"<div style=\"text-decoration: underline\">. . . .</div>"
		"<div style=\"text-decoration: line-through\">. . . .</div>"
		"<div style=\"text-decoration: overline\">. . . .</div>",
		0, 0, 0);
	CHECK(decoration_at(0, 150) == INT32_MIN);
	int under = decoration_at(150, 300), through = decoration_at(300, 450), over = decoration_at(450, 600);
	CHECK(under != INT32_MIN && under > 0);
	CHECK(through != INT32_MIN && through < 0);
	CHECK(over != INT32_MIN && over < through);
	page_destroy(p);
}

/* How wide `body` draws (how far right its text goes) */
static int width_of(const char *body)
{
	char html[512];
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\"><span>%s</span></body></html>", body);
	struct page *p = page_create(html, strlen(html), "https://example.org/", 720, 1440);
	int w = page_content_width(p);
	page_destroy(p);
	return w;
}

/* A word longer than a line (a web address in a message, nothing to break it at)
 * breaks at its slashes, dashes and dots, or anywhere when it has none: no page
 * wider than its window, nor shrunk to it, for one; a word that fits stays whole */
TEST(long_words_break)
{
	const char *address = "https://bvb.ro/FinancialInstruments/SelectedData/NewsItem/TVBETETF-notificare-cu-privire-la-numarul-de-titluri-de-participare-aflate-in-circulatie/36F7D";
	CHECK(width_of(address) <= 720);
	char html[512];
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\"><a href=\"%s\">%s</a></body></html>", address, address);
	struct page *p = page_create(html, strlen(html), "https://example.org/", 720, 1440);
	int one_line = page_height(p);
	page_destroy(p);
	snprintf(html, sizeof(html), "<html><body style=\"margin: 0\">short</body></html>");
	p = page_create(html, strlen(html), "https://example.org/", 720, 1440);
	CHECK(one_line >= 3 * page_height(p)); /* over several lines */
	page_destroy(p);
	/* Letters alone, too long for a line: cut where the line ends */
	CHECK(width_of("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") <= 720);
	/* Pieces stay together while they fit */
	CHECK_INT(width_of("https://example.org/a-b"), width_of("https://example.org/a-b"));
	CHECK(width_of("https://example.org/a-b") > width_of("https://example.org/"));
}

/* text-transform: the letters drawn as they were typed so */
TEST(text_transformed)
{
	CHECK(width_of("ABCD") > width_of("abcd"));
	CHECK_INT(width_of("<span style=\"text-transform: uppercase\">abcd</span>"), width_of("ABCD"));
	CHECK_INT(width_of("<span style=\"text-transform: lowercase\">ABCD</span>"), width_of("abcd"));
	CHECK_INT(width_of("<span style=\"text-transform: capitalize\">ab cd</span>"), width_of("Ab Cd"));
	/* Letters beyond ASCII too, those of the fonts (Latin-1, Latin Extended-A, Romanian's
	 * comma-below ones): a heading in capitals keeps its Ș, É and Ă capitals */
	CHECK(width_of("&#x218;") != width_of("&#x219;"));
	CHECK_INT(width_of("<span style=\"text-transform: uppercase\">&#x219;a&#xe9;&#x103;&#xff;</span>"), width_of("&#x218;A&#xc9;&#x102;&#x178;"));
	CHECK_INT(width_of("<span style=\"text-transform: lowercase\">&#x21a;&#xc9;&#x141;&#xdf;</span>"), width_of("&#x21b;&#xe9;&#x142;&#xdf;"));
	CHECK_INT(width_of("<span style=\"text-transform: capitalize\">&#x219;coal&#x103; &#xe9;t&#xe9;</span>"), width_of("&#x218;coal&#x103; &#xc9;t&#xe9;"));
	/* A sign between the letters stays (× is not a case of ÷) */
	CHECK_INT(width_of("<span style=\"text-transform: lowercase\">&#xd7;</span>"), width_of("&#xd7;"));
	/* Sizes in points: 12pt is 16px */
	CHECK_INT(width_of("<span style=\"font-size: 12pt\">abcd</span>"),
	          width_of("<span style=\"font-size: 16px\">abcd</span>"));
}

/* Characters the fonts lack drawn as ones they have: spaces, dashes, bullets and primes
 * as theirs, invisible ones as nothing (not as missing glyphs) */
TEST(characters_the_fonts_lack)
{
	CHECK_INT(width_of("a&nbsp;b"), width_of("a b"));
	CHECK_INT(width_of("a&#x2003;b"), width_of("a b"));
	CHECK_INT(width_of("a&#x202f;b"), width_of("a b"));
	CHECK_INT(width_of("a&shy;b"), width_of("ab"));
	CHECK_INT(width_of("a&#x200b;b"), width_of("ab"));
	CHECK_INT(width_of("a&#xfeff;b"), width_of("ab"));
	CHECK_INT(width_of("a&#x2010;b"), width_of("a-b"));
	CHECK_INT(width_of("a&#x2212;b"), width_of("a-b"));
	CHECK_INT(width_of("a&#x2022;b"), width_of("a&#xb7;b"));
	CHECK_INT(width_of("a&#x2032;b"), width_of("a'b"));
	CHECK_INT(width_of("a&#x2033;b"), width_of("a\"b"));
	/* One no font has: a gap for it */
	CHECK(width_of("a&#x4e2d;b") > width_of("ab"));
	/* Drawn as measured: the soft hyphen's line as wide as without it */
	struct page *p = page_drawn("<p style=\"margin: 0; font-size: 40px; line-height: 100px\">a&shy;b&#x200b;c</p>"
	                            "<p style=\"margin: 0; font-size: 40px; line-height: 100px\">abc</p>", 0, 0, 0);
	lv_area_t with, without;
	CHECK(box_of(0x000000, 0, 0, 720, 150, &with));
	CHECK(box_of(0x000000, 0, 150, 720, 300, &without));
	CHECK_INT(with.x2, without.x2);
	page_destroy(p);
}

/* <link>ed style sheets asked for before the page is built whatever their attributes
 * look like: unquoted, after others ending in "href", their &amp; decoded (Wikipedia's) */
TEST(linked_style_sheet_attributes)
{
	ui_test_serve("https://example.org/", 200, HTML,
	              "<html><head><title>Linked</title><link data-href=\"/no.css\" rel=stylesheet "
	              "href=\"/load.php?a=1&amp;b=2\"><link rel='stylesheet' href=/b.css></head>"
	              "<body style=\"margin: 0\"><div class=\"a\" style=\"height: 300px\"></div>"
	              "<div class=\"b\" style=\"height: 300px\"></div></body></html>");
	ui_test_serve("https://example.org/load.php?a=1&b=2", 200, "text/css", ".a { background: #ff0000 }");
	ui_test_serve("https://example.org/b.css", 200, "text/css", ".b { background: #0000ff }");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Linked"));
	CHECK(ui_test_requested("https://example.org/no.css") == NULL);
	CHECK(is_color(ui_test_pixel(360, 300), 0xff0000));
	CHECK(is_color(ui_test_pixel(360, 700), 0x0000ff));
}

/* A mask taller than its box, fitted in it and centered; a background image repeated
 * along x only; both counted in how wide the page draws */
TEST(masks_and_background_images)
{
	struct page *p = page_drawn(
		"<div style=\"width: 100px; height: 100px; background-color: #ff0000; "
		"mask-image: url(/tall.png)\"></div>"
		"<div style=\"width: 400px; height: 100px; background: url(/tall.png) repeat-x\"></div>",
		0x00ff00, 10, 20);
	/* The mask 75 x 150 in the middle of the box's 150 x 150 */
	CHECK_INT(at(75, 75), 0xff0000);
	CHECK_INT(at(20, 75), 0xffffff);
	CHECK_INT(at(130, 75), 0xffffff);
	/* From y = 150: green along the top, 20 rows (the image's size in CSS pixels) */
	CHECK_INT(at(5, 160), 0x00ff00);
	CHECK_INT(at(590, 160), 0x00ff00);
	CHECK_INT(at(5, 250), 0xffffff);
	CHECK(page_content_width(p) >= 150);
	page_destroy(p);
}


/* Images that cannot be read (cut short, corrupt, too large) are not; wide ones are
 * scaled down to the screen's width, their natural size kept */
TEST(images_decoded_or_not)
{
	int w = 0, h = 0, nw = 0, nh = 0;
	uint8_t *px;
	/* A JPEG 1600 wide */
	uint8_t *rgb = calloc(1600 * 100, 4);
	char *jpeg = NULL;
	size_t jpeg_size = 0;
	FILE *f = open_memstream(&jpeg, &jpeg_size);
	CHECK(ui_jpeg_write(f, rgb, 1600, 100, 1600 * 4, 90, true));
	fclose(f);
	free(rgb);
	px = browser_image_decode(jpeg, jpeg_size, &w, &h, &nw, &nh);
	CHECK(px);
	CHECK_INT(w, 720);
	CHECK_INT(h, 45);
	CHECK_INT(nw, 1600);
	CHECK_INT(nh, 100);
	free(px);
	/* Its frame said to be 20000 x 20000 (SOF0: FF C0, length, precision, height, width) */
	char *sof = memmem(jpeg, jpeg_size, "\xff\xc0", 2);
	CHECK(sof);
	if (sof)
		memcpy(sof + 5, "\x4e\x20\x4e\x20", 4);
	CHECK(!browser_image_decode(jpeg, jpeg_size, &w, &h, &nw, &nh));
	free(jpeg);
	CHECK(!browser_image_decode("\xff\xd8\xff\xe0 not a JPEG", 16, &w, &h, &nw, &nh));

	/* A PNG 2000 x 1: 720 x 1 */
	size_t png_size;
	void *png = ui_test_png(2000, 1, 0xff0000, &png_size);
	px = browser_image_decode(png, png_size, &w, &h, &nw, &nh);
	CHECK(px);
	CHECK_INT(w, 720);
	CHECK_INT(h, 1);
	free(px);
	/* Cut short */
	CHECK(!browser_image_decode(png, png_size - 20, &w, &h, &nw, &nh));
	free(png);
	/* Too large: 8000 x 8000 said in its header (IHDR), then its data */
	static const unsigned char ihdr[] = { 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0x1f, 0x40, 0, 0, 0x1f, 0x40,
	                                      8, 6, 0, 0, 0 };
	unsigned char big[64];
	memcpy(big, "\x89PNG\r\n\x1a\n", 8);
	memcpy(big + 8, ihdr, sizeof(ihdr));
	uint32_t crc = crc32(0, ihdr + 4, sizeof(ihdr) - 4);
	big[29] = crc >> 24;
	big[30] = crc >> 16;
	big[31] = crc >> 8;
	big[32] = crc;
	memcpy(big + 33, "\0\0\0\0IDAT\x35\xaf\x06\x1e", 12);
	CHECK(!browser_image_decode((char *)big, 45, &w, &h, &nw, &nh));

	/* A WebP cut short */
	uint8_t *rgb3 = malloc(64 * 64 * 3);
	for (int i = 0; i < 64 * 64 * 3; i++)
		rgb3[i] = (uint8_t)(i * 37 ^ i >> 5); /* noise: not a few bytes */
	uint8_t *webp = NULL;
	size_t webp_size = WebPEncodeLosslessRGB(rgb3, 64, 64, 64 * 3, &webp);
	free(rgb3);
	CHECK(webp_size > 40);
	CHECK(!browser_image_decode((char *)webp, webp_size / 2, &w, &h, &nw, &nh));
	WebPFree(webp);

	/* An SVG after a byte order mark and blank lines, wider than the screen; one
	 * without a size */
	const char *svg = "\xef\xbb\xbf\n  <svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"100\">"
	                  "<rect width=\"1000\" height=\"100\" fill=\"#ff0000\"/></svg>";
	px = browser_image_decode(svg, strlen(svg), &w, &h, &nw, &nh);
	CHECK(px);
	CHECK_INT(w, 720);
	CHECK_INT(h, 72);
	CHECK_INT(nw, 1000);
	free(px);
	const char *empty = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"0\"></svg>";
	CHECK(!browser_image_decode(empty, strlen(empty), &w, &h, &nw, &nh));
}

/* A data: image's base64 broken into lines, as pages often have it */
TEST(data_image_with_line_breaks)
{
	size_t n;
	void *png = ui_test_png(60, 60, 0x00ff00, &n);
	char *b64 = base64(png, n);
	free(png);
	CHECK(ui_test_open("Web Lite"));
	/* A line break every 60 characters, then an encoded one */
	for (int k = 0; k < 2; k++) {
		char lines[8192], *o = lines;
		for (size_t i = 0; b64[i]; i++) {
			if (i && i % 60 == 0)
				o = stpcpy(o, k ? "%0A" : "\n");
			*o++ = b64[i];
		}
		*o = '\0';
		char html[9000], url[64];
		snprintf(html, sizeof(html), "<html><body style=\"margin: 0\"><img src=\"data:image/png;base64,%s\" "
		         "width=\"200\" height=\"200\"></body></html>", lines);
		snprintf(url, sizeof(url), "https://example.org/%d", k);
		ui_test_serve(url, 200, HTML, html);
		CHECK(go(url));
		ui_test_wait(1000);
		CHECK(is_color(ui_test_pixel(150, 300), 0x00ff00));
	}
	free(b64);
}

/* The bar's back button, below the page */
static bool back(void)
{
	lv_obj_t *buttons[4];
	unsigned count = ui_test_find_all(LV_SYMBOL_LEFT, buttons, 4);
	if (count < 2)
		return false;
	ui_test_tap(buttons[count - 1]);
	net_wait();
	return true;
}

/* Back goes through the last 32 pages, the earlier ones forgotten */
TEST(history_of_32_pages)
{
	ui_test_serve("https://example.org/*", 200, HTML, "<html><body>Page</body></html>");
	CHECK(ui_test_open("Web Lite"));
	for (int i = 1; i <= 33; i++) {
		char url[64];
		snprintf(url, sizeof(url), "https://example.org/%d", i);
		CHECK(go(url));
	}
	CHECK_STR(address(), "https://example.org/33");
	for (int i = 0; i < 31; i++)
		CHECK(back());
	CHECK_STR(address(), "https://example.org/2");
	CHECK(back());
	CHECK(ui_at_home());
}

/* A link to a place on the page itself scrolls there; one to a place on another page
 * opens it there */
TEST(links_to_places)
{
	/* With its fragment too: the fake asks for the URL as given */
	ui_test_serve("https://example.org/*", 200, HTML,
	              "<html><body style=\"margin: 0\"><a href=\"#end\" style=\"display: block; height: 3000px\">"
	              "Down</a><div id=\"end\" style=\"height: 2000px; background: #00ff00\"></div></body></html>");
	ui_test_serve("https://example.org/other", 200, HTML,
	              LINK_PAGE("Other", "/#end"));
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	unsigned n = ui_test_request_count();
	CHECK(is_color(page_pixel(), 0xffffff));
	ui_test_tap_at(360, 600);
	ui_test_wait(500);
	CHECK(is_color(page_pixel(), 0x00ff00));
	CHECK_INT(ui_test_request_count(), n);

	CHECK(go("https://example.org/other"));
	ui_test_tap_at(360, 600);
	net_wait();
	CHECK_STR(address(), "https://example.org/#end");
	CHECK(is_color(page_pixel(), 0x00ff00));
}

/* Spaces before an address are not part of it; only spaces: nothing */
TEST(spaces_typed)
{
	ui_test_serve("https://example.org*", 200, HTML, "<title>Example</title>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("   "));
	CHECK_INT(ui_test_request_count(), 0);
	CHECK(go("  example.org"));
	CHECK(ui_test_shows("Example"));
	CHECK_STR(address(), "https://example.org");
}

/* The keyboard hidden while typing: the address of the page shown again */
TEST(typing_given_up)
{
	ui_test_serve("https://example.org/", 200, HTML, "<title>Example</title>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/"));
	ui_test_tap(address_field());
	lv_textarea_set_text(address_field(), "");
	CHECK(ui_test_type("elsewhere"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK_STR(address(), "https://example.org/");
	CHECK_INT(ui_test_request_count(), 1);
}

/* Stopped while the next page waits for its style sheet: the page shown stays, with
 * its title (an untitled one: Web Lite's) */
TEST(stopped_while_styled)
{
	ui_test_serve("https://example.org/", 200, HTML, "<title>Example</title>");
	ui_test_serve("https://example.org/plain", 200, HTML, "<html><body>Plain</body></html>");
	ui_test_serve("https://example.org/styled", 200, HTML,
	              "<html><head><title>Styled</title><link rel=\"stylesheet\" href=\"/s.css\"></head></html>");
	ui_test_serve("https://example.org/s.css", 200, "text/css", "body { background: #00ff00 }");
	CHECK(ui_test_open("Web Lite"));
	for (int i = 0; i < 2; i++) {
		CHECK(go(i ? "https://example.org/plain" : "https://example.org/"));
		const char *title = i ? "Web Lite" : "Example";
		CHECK(ui_test_shows(title));
		/* The page arrived, its style sheet asked for and held */
		ui_test_network_hold(true);
		CHECK(go("https://example.org/styled"));
		ui_test_network_hold(false);
		for (int t = 0; t < 100 && !ui_test_requested("https://example.org/s.css"); t++)
			ui_test_wait(16);
		ui_test_network_hold(true);
		CHECK(ui_test_requested("https://example.org/s.css") != NULL);
		CHECK(ui_test_shows("Loading..."));
		CHECK(ui_test_tap_text(LV_SYMBOL_CLOSE));
		ui_test_network_hold(false);
		ui_test_wait(1000);
		CHECK(ui_test_shows(title));
		CHECK(!ui_test_shows("Styled"));
		CHECK(is_color(page_pixel(), 0xffffff));
	}
}

/* A long page left for another once shown: destroyed on a thread (it takes a while),
 * the other page shows; closed while its picture is laid out on a thread */
TEST(long_page_left)
{
	char *html = long_page("Long");
	ui_test_serve("https://example.org/long", 200, HTML, html);
	free(html);
	ui_test_serve("https://example.org/long.css", 200, "text/css", "body { background: #00ff00 }");
	size_t n;
	void *png = ui_test_png(720, 600, 0x0000ff, &n);
	ui_test_serve_data("https://example.org/top.png", 200, "image/png", png, n);
	free(png);
	ui_test_serve("https://example.org/short", 200, HTML, "<html><head><title>Short</title></head></html>");
	CHECK(ui_test_open("Web Lite"));
	CHECK(go("https://example.org/long"));
	CHECK(ui_test_wait_for("Long", 20000));
	CHECK(ui_test_wait_until(picture_shown, 20000));
	CHECK(go("https://example.org/short"));
	CHECK(ui_test_wait_for("Short", 20000));

	/* Again, closed as its picture arrives */
	CHECK(go("https://example.org/long"));
	CHECK(ui_test_wait_for("Long", 20000));
	ui_test_wait(600);
	CHECK(ui_test_back());
	ui_test_wait(2000);
	CHECK(ui_at_home());
}

/* HTML in an app's screen (ui_html_create(), html.c: GitHub's comments, mail): at most
 * 16 of its images fetched; drawn dark when the app asks, whatever the theme */
TEST(html_in_an_app_screen)
{
	size_t n;
	void *png = ui_test_png(10, 10, 0x0000ff, &n);
	ui_test_serve_data("https://example.org/*.png", 200, "image/png", png, n);
	free(png);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("HTML", NULL, &body);
	ui_push(scr);
	ui_test_settle();
	char html[2048] = "<div style=\"height: 200px\"></div>";
	for (int i = 0; i < 17; i++)
		snprintf(html + strlen(html), sizeof(html) - strlen(html), "<img src=\"i%d.png\" width=\"20\">", i);
	lv_obj_t *obj = ui_html_create(body, html, "https://example.org/", 600, 2000, NULL);
	for (int i = 0; i < 40; i++)
		ui_test_wait(100);
	CHECK_INT(ui_test_request_count(), 16);
	CHECK(ui_test_shows_color(obj, 0xffffff));
	CHECK(!ui_test_shows_color(obj, PAGE_DARK_PAPER));
	ui_html_set_dark(obj, true);
	ui_test_wait(100);
	CHECK(ui_test_shows_color(obj, PAGE_DARK_PAPER));
	CHECK(!ui_test_shows_color(obj, 0xffffff));
}

/* In the dark theme, its white the background it is on; wider than it is: scrolled
 * sideways, or scaled down to fit; left while an image is on its way: the fetch stopped */
TEST(html_fitted_in_the_dark_theme)
{
	ui_set_light(false, NULL);
	ui_test_settle();
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("HTML", NULL, &body);
	ui_push(scr);
	ui_test_settle();
	char css[1024];
	ui_html_css(css, sizeof(css), lv_color_black(), lv_color_white(), lv_color_hex(0x0000ff));
	lv_obj_t *obj = ui_html_create(body, "<div style=\"width: 800px; height: 100px\"></div>", NULL, 600, 2000,
	                               css);
	ui_test_settle();
	uint32_t bg = lv_color_to_u32(lv_obj_get_style_bg_color(body, 0)) & 0xffffff;
	CHECK(ui_test_shows_color(obj, bg));
	/* 1200 px wide: 150 high, scrolled; fitted, half as high */
	CHECK_INT(lv_obj_get_height(obj), 150);
	CHECK(lv_obj_is_scrollable(obj));
	ui_html_set_fit(obj, true);
	ui_test_settle();
	CHECK_INT(lv_obj_get_height(obj), 75);
	CHECK(!lv_obj_is_scrollable(obj));

	ui_test_network_hold(true);
	ui_html_create(body, "<img src=\"a.png\">", "https://example.org/", 600, 2000, NULL);
	ui_test_wait(300);
	CHECK(ui_test_requested("https://example.org/a.png") != NULL);
	ui_pop();
	ui_test_settle();
	ui_test_network_hold(false);
	ui_test_wait(500);
	CHECK_INT(ui_test_request_count(), 1);
}
