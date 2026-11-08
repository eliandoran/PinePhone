/*
 * Browser (apps/web), the WebKit plugin app: web.so built for the host (WEB_APP, the
 * Makefile's), installed in /var/lib/apps/web and loaded by phone-ui (external.c) as on
 * the phone, with fake_webkit.c as the engine and pages served by the tests
 * (fake_curl.c). Frames are one color per page: the tests check the title, the address,
 * what was requested, pixels, and what the engine was told.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wpe/wpe.h>

#include "test.h"
#include "ui_test.h"

#define SEARCH "https://duckduckgo.com/?q="
#define HTML "text/html; charset=utf-8"
#define PAGE(title, color) "<html><head><title>" title "</title></head>" \
	"<body style=\"background: " color "\"><h1>" title "</h1></body></html>"

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
	if (out)
		ok &= fclose(out) == 0;
	return ok;
}

/* As `just app web` installs it, but its engine (root/), which is fake_webkit.c */
static void install(void)
{
	ui_test_reset();
	if (!ui_test_write("/var/lib/apps/web/app.json", "") ||
	    !copy(WEB_APP "/app.json", "/var/lib/apps/web/app.json") ||
	    !copy(WEB_APP "/web.so", "/var/lib/apps/web/web.so"))
		TEST_FAIL("cannot install %s", WEB_APP);
}

TEST_SETUP
{
	install();
}

static bool open_browser(void)
{
	ui_test_start();
	bool ok = ui_test_open("Browser");
	ui_test_wait(100);
	return ok;
}

static void net_wait(void)
{
	ui_test_wait(300);
	ui_test_settle();
}

/* Browser's screen: its address field */
static bool browser_shown(void)
{
	lv_obj_t *ta = ui_test_find_widget(&lv_textarea_class);
	return ta && strcmp(lv_textarea_get_placeholder_text(ta), "Address or search") == 0;
}

static const char *address(void)
{
	lv_obj_t *ta = ui_test_find_widget(&lv_textarea_class);
	return ta ? lv_textarea_get_text(ta) : "";
}

/* Typed in the address bar, then Enter */
static bool go(const char *text)
{
	lv_obj_t *ta = ui_test_find_widget(&lv_textarea_class);
	if (!ta)
		return false;
	ui_test_tap(ta);
	lv_textarea_set_text(ta, "");
	bool ok = ui_test_type(text) && ui_test_type("\n");
	net_wait();
	return ok;
}

/* The page's frame, NULL if none */
static lv_obj_t *frame(void)
{
	return ui_test_find_widget(&lv_3dtexture_class);
}

/* The page's view: the area the frame is drawn in, NULL if none */
static lv_obj_t *view(void)
{
	lv_obj_t *f = frame();
	return f ? lv_obj_get_parent(f) : NULL;
}

static lv_color_t page_pixel(void)
{
	lv_area_t a = { 0 };
	lv_obj_t *v = view();
	if (v)
		lv_obj_get_coords(v, &a);
	return ui_test_pixel((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

static bool is_color(lv_color_t c, uint32_t rgb)
{
	return c.red == (rgb >> 16 & 0xff) && c.green == (rgb >> 8 & 0xff) && c.blue == (rgb & 0xff);
}

/* The bar's back arrow, below the header's */
static bool tap_bar_back(void)
{
	lv_obj_t *found[4];
	unsigned n = ui_test_find_all(LV_SYMBOL_LEFT, found, 4);
	lv_obj_t *lowest = NULL;
	for (unsigned i = 0; i < n && i < 4; i++) {
		lv_area_t a, b;
		lv_obj_get_coords(found[i], &a);
		if (lowest)
			lv_obj_get_coords(lowest, &b);
		if (!lowest || a.y1 > b.y1)
			lowest = found[i];
	}
	if (!lowest)
		return false;
	ui_test_tap(lowest);
	net_wait();
	return true;
}

static unsigned requests_of(const char *url)
{
	unsigned n = 0;
	for (unsigned i = 0; i < ui_test_request_count(); i++)
		n += strcmp(ui_test_request(i)->url, url) == 0;
	return n;
}

TEST(listed_and_opens_on_start_page)
{
	ui_test_start();
	ui_open_menu();
	ui_test_settle();
	CHECK(ui_test_shows("Browser"));
	CHECK(open_browser());
	CHECK(ui_test_shows("Browser"));
	CHECK(browser_shown());
	CHECK_STR(address(), "");
	CHECK_INT(ui_test_request_count(), 0);
	CHECK(is_color(page_pixel(), 0xf2f2f7)); /* the start page's background */
}

/* WebKit at the lowest frequency is sluggish: the CPU at its highest while Browser is open
 * (cpu.c) */
TEST(cpu_at_highest)
{
	long khz = 0;
	CHECK(open_browser());
	CHECK(ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", &khz));
	CHECK_INT(khz, 1152000);
	CHECK(ui_test_back());
	CHECK(ui_read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", &khz));
	CHECK_INT(khz, 648000);
}

TEST(told_how_to_draw)
{
	CHECK(open_browser());
	CHECK_DBL(ui_test_web_scale(), 1.5);
	CHECK_INT(ui_test_web_activity(), wpe_view_activity_state_visible |
	          wpe_view_activity_state_focused | wpe_view_activity_state_in_window);
	CHECK(strstr(ui_test_web_user_agent(), "iPhone") != NULL);
	CHECK(ui_test_web_feature("StorageAPI"));
	CHECK(ui_test_web_feature("FileSystem"));
	CHECK(!ui_test_web_feature("WebGPU"));
}

TEST(address_typed)
{
	ui_test_serve("https://example.org", 200, HTML, PAGE("Example Domain", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("example.org"));
	CHECK(ui_test_requested("https://example.org") != NULL);
	CHECK(ui_test_shows("Example Domain"));
	CHECK_STR(address(), "https://example.org");
	CHECK(is_color(page_pixel(), 0xff0000));
}

/* Spaces before an address left out; only spaces: nothing loaded */
TEST(address_after_spaces)
{
	ui_test_serve("https://example.org", 200, HTML, PAGE("Example Domain", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("   "));
	CHECK_INT(ui_test_request_count(), 0);
	CHECK(go("  example.org"));
	CHECK(ui_test_requested("https://example.org") != NULL);
	CHECK(ui_test_shows("Example Domain"));
}

TEST(full_address_kept)
{
	ui_test_serve("http://example.org/a b", 200, HTML, PAGE("Plain", "#00ff00"));
	CHECK(open_browser());
	CHECK(go("http://example.org/a b"));
	CHECK(ui_test_requested("http://example.org/a b") != NULL);
	CHECK(ui_test_shows("Plain"));
}

TEST(words_searched)
{
	ui_test_serve(SEARCH "*", 200, HTML, PAGE("DuckDuckGo", "#de5833"));
	CHECK(open_browser());
	CHECK(go("pine phone"));
	CHECK(ui_test_requested(SEARCH "pine%20phone") != NULL);
	CHECK(ui_test_shows("DuckDuckGo"));
}

TEST(redirect_shows_where_it_went)
{
	ui_test_serve_redirect("https://example.org", "https://www.example.org/");
	ui_test_serve("https://www.example.org/", 200, HTML, PAGE("Moved", "#0000ff"));
	CHECK(open_browser());
	CHECK(go("example.org"));
	CHECK(ui_test_shows("Moved"));
	CHECK_STR(address(), "https://www.example.org/");
}

TEST(loading_and_stop)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Loading..."));
	CHECK(ui_test_shows(LV_SYMBOL_CLOSE));
	CHECK(ui_test_tap_text(LV_SYMBOL_CLOSE)); /* stop */
	CHECK(ui_test_shows("Browser"));
	CHECK(ui_test_shows(LV_SYMBOL_REFRESH));
	CHECK_STR(address(), "");
	ui_test_network_hold(false);
	net_wait();
	CHECK(!ui_test_shows("Example"));
	CHECK(is_color(page_pixel(), 0xf2f2f7));
}

TEST(reload)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	CHECK_INT(requests_of("https://example.org/"), 1);
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	net_wait();
	CHECK_INT(requests_of("https://example.org/"), 2);
	CHECK(ui_test_shows("Example"));
}

TEST(back_through_pages_then_closes)
{
	ui_test_serve("https://a.example/", 200, HTML, PAGE("Page A", "#ff0000"));
	ui_test_serve("https://b.example/", 200, HTML, PAGE("Page B", "#00ff00"));
	CHECK(open_browser());
	CHECK(go("https://a.example/"));
	CHECK(go("https://b.example/"));
	CHECK(ui_test_shows("Page B"));
	CHECK(tap_bar_back());
	CHECK(ui_test_shows("Page A"));
	CHECK_STR(address(), "https://a.example/");
	CHECK(is_color(page_pixel(), 0xff0000));
	CHECK(tap_bar_back());
	CHECK(ui_test_shows("Browser"));
	CHECK_STR(address(), "");
	CHECK(tap_bar_back()); /* on the start page: closes */
	CHECK(!browser_shown());
	CHECK_INT(ui_test_web_views(), 0);
}

TEST(page_not_loaded)
{
	CHECK(open_browser());
	CHECK(go("https://offline.example/")); /* nothing serves it */
	CHECK(ui_test_requested("https://offline.example/") != NULL);
	CHECK(ui_test_shows(LV_SYMBOL_REFRESH)); /* done loading */
	CHECK(!ui_test_shows("Loading..."));
	CHECK_STR(address(), "https://offline.example/");
}

TEST(new_window_opens_here)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	ui_test_serve("https://other.example/", 200, HTML, PAGE("Other", "#00ff00"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	ui_test_web_new_window("https://other.example/");
	net_wait();
	CHECK(ui_test_shows("Other"));
	CHECK_STR(address(), "https://other.example/");
	CHECK_INT(ui_test_web_views(), 1);
}

TEST(crash_reloads)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	ui_test_web_crash();
	net_wait();
	CHECK_INT(requests_of("https://example.org/"), 2);
	CHECK(ui_test_shows("Example"));
}

/* WebKit gets the finger in device pixels from the view's corner, and scrolls itself */
TEST(touches_reach_the_page)
{
	CHECK(open_browser());
	lv_obj_t *v = view();
	CHECK(v != NULL);
	if (!v)
		return;
	lv_area_t a;
	lv_obj_get_coords(v, &a);
	ui_test_tap_at(a.x1 + 100, a.y1 + 200);
	CHECK(ui_test_web_touch_count() >= 2);
	const struct ui_test_touch *first = ui_test_web_touch(0);
	const struct ui_test_touch *last = ui_test_web_touch(ui_test_web_touch_count() - 1);
	if (!first || !last)
		return;
	CHECK_INT(first->type, wpe_input_touch_event_type_down);
	CHECK_INT(first->x, 100);
	CHECK_INT(first->y, 200);
	CHECK_INT(last->type, wpe_input_touch_event_type_up);

	unsigned before = ui_test_web_touch_count();
	ui_test_swipe(a.x1 + 300, a.y1 + 600, a.x1 + 300, a.y1 + 200, 300);
	unsigned moves = 0;
	for (unsigned i = before; i < ui_test_web_touch_count(); i++)
		moves += ui_test_web_touch(i)->type == wpe_input_touch_event_type_motion;
	CHECK(moves > 2);
	last = ui_test_web_touch(ui_test_web_touch_count() - 1);
	CHECK_INT(last->type, wpe_input_touch_event_type_up);
	CHECK(last->y < 300);
	CHECK(browser_shown());
}

/* Each frame is shown, then handed back: WebKit draws the next once told */
TEST(frames_handed_back)
{
	ui_test_serve("https://a.example/", 200, HTML, PAGE("Page A", "#ff0000"));
	ui_test_serve("https://b.example/", 200, HTML, PAGE("Page B", "#00ff00"));
	CHECK(open_browser());
	CHECK(go("https://a.example/"));
	CHECK(go("https://b.example/"));
	CHECK(ui_test_web_frames() >= 3);
	CHECK_INT(ui_test_web_images(), 1); /* the one shown */
	CHECK(is_color(page_pixel(), 0x00ff00));
}

TEST(closing_frees_the_engine)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	CHECK_INT(ui_test_web_views(), 1);
	CHECK(ui_test_back());
	CHECK_INT(ui_test_web_views(), 0);
	CHECK_INT(ui_test_web_images(), 0);
	/* And again */
	CHECK(ui_test_open("Browser"));
	ui_test_wait(100);
	CHECK_INT(ui_test_web_views(), 1);
	CHECK(is_color(page_pixel(), 0xf2f2f7));
}

/* Closed while a page loads: nothing arrives after */
TEST(closed_while_loading)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	net_wait();
	CHECK_INT(ui_test_web_views(), 0);
	CHECK(!ui_test_shows("Example"));
}

TEST(engine_does_not_start)
{
	ui_test_web_broken(true);
	CHECK(open_browser());
	CHECK(ui_test_shows("Browser"));
	CHECK_INT(ui_test_web_views(), 0);
	CHECK(go("example.org")); /* nothing to load it */
	CHECK_INT(ui_test_request_count(), 0);
	CHECK(tap_bar_back());
	CHECK(!browser_shown());
}

/* Development: `echo URL > /tmp/web-load` over SSH */
TEST(load_from_file)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(ui_test_write("/tmp/web-load", "https://example.org/\n"));
	ui_test_wait(600);
	net_wait();
	CHECK(ui_test_shows("Example"));
	CHECK(fopen("/tmp/web-load", "r") == NULL);
}

/* `echo "js:return ..." > /tmp/web-load`: run in the page (the fake has no JavaScript:
 * its error logged), not loaded as an address */
TEST(script_from_file)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	CHECK(ui_test_write("/tmp/web-load", "js:return document.title\n"));
	ui_test_wait(600);
	net_wait();
	CHECK(fopen("/tmp/web-load", "r") == NULL);
	CHECK_INT(ui_test_request_count(), 1);
	CHECK(ui_test_shows("Example"));
	CHECK_STR(address(), "https://example.org/");
}

/* WebKit lays the page out at the view's size and its frame fills the view, 1 px
 * rounding aside, drawn at its own size: not one of another size, stretched to fit */
static bool page_fits(void)
{
	lv_obj_t *v = view();
	if (!v)
		return false;
	lv_obj_update_layout(v);
	int32_t w = lv_obj_get_width(v), h = lv_obj_get_height(v), css_w, css_h, frame_w, frame_h;
	ui_test_web_size(&css_w, &css_h);
	ui_test_web_frame_size(&frame_w, &frame_h);
	bool fits = css_w == (int32_t)(w / 1.5f) && css_h == (int32_t)(h / 1.5f) &&
	            abs(frame_w - w) <= 1 && abs(frame_h - h) <= 1;
	fits &= lv_obj_get_width(frame()) == frame_w && lv_obj_get_height(frame()) == frame_h;
	if (!fits)
		fprintf(stderr, "  view %dx%d, WebKit lays out %dx%d (x 1.5), its frame %dx%d drawn %dx%d\n",
		        (int)w, (int)h, (int)css_w, (int)css_h, (int)frame_w, (int)frame_h,
		        (int)lv_obj_get_width(frame()), (int)lv_obj_get_height(frame()));
	return fits;
}

/* The keyboard takes the bottom of the view: the page is laid out in what is left,
 * then in all of it again once the keyboard goes */
TEST(keyboard_shrinks_the_page)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	CHECK(page_fits());
	int32_t full = lv_obj_get_height(view());

	CHECK(ui_test_tap_field("https://example.org/"));
	ui_test_wait(100);
	CHECK(ui_test_textarea() != NULL); /* the keyboard is up */
	CHECK(lv_obj_get_height(view()) < full);
	CHECK(page_fits());
	CHECK(is_color(page_pixel(), 0xff0000));

	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD)); /* hidden */
	ui_test_wait(100);
	CHECK_INT(lv_obj_get_height(view()), full);
	CHECK(page_fits());
}

/* The frame's marker band is where a frame drawn 1:1 has it, from the view's top */
static bool marker_in_place(void)
{
	lv_obj_t *v = view();
	if (!v)
		return false;
	lv_area_t a;
	lv_obj_get_coords(v, &a);
	int32_t y = a.y1 + UI_TEST_WEB_MARKER_Y + UI_TEST_WEB_MARKER_H / 2;
	lv_color_t c = ui_test_pixel((a.x1 + a.x2) / 2, y);
	if (!is_color(c, UI_TEST_WEB_MARKER))
		fprintf(stderr, "  at %d, %d px down the view: #%02x%02x%02x, not the marker's band\n",
		        (int)y, (int)(y - a.y1), c.red, c.green, c.blue);
	return is_color(c, UI_TEST_WEB_MARKER);
}

/* Until WebKit has a frame of the view's new size, the last one is not stretched into
 * it, as the keyboard comes up and as it goes */
TEST(keyboard_no_stretched_frame)
{
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	CHECK(go("https://example.org/"));
	CHECK(marker_in_place());
	int32_t full = lv_obj_get_height(view());

	ui_test_web_hold_frames(true);
	CHECK(ui_test_tap_field("https://example.org/"));
	ui_test_wait(100);
	CHECK(lv_obj_get_height(view()) < full);
	CHECK(marker_in_place());
	CHECK(is_color(page_pixel(), 0xff0000));
	ui_test_web_hold_frames(false);
	ui_test_wait(100);
	CHECK(page_fits());
	CHECK(marker_in_place());

	ui_test_web_hold_frames(true);
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	ui_test_wait(100);
	CHECK_INT(lv_obj_get_height(view()), full);
	CHECK(marker_in_place());
	ui_test_web_hold_frames(false);
	ui_test_wait(100);
	CHECK(page_fits());
	CHECK(marker_in_place());
}

/* In Romanian: the start page's texts, the address field, loading (its own catalog, apps/web/po) */
TEST(romanian)
{
	ui_test_write("/var/lib/phone-ui/settings", "language=ro\n");
	ui_test_serve("https://example.org/", 200, HTML, PAGE("Example", "#ff0000"));
	CHECK(open_browser());
	lv_obj_t *ta = ui_test_find_widget(&lv_textarea_class);
	CHECK(ta && strcmp(lv_textarea_get_placeholder_text(ta), "Adresă sau căutare") == 0);
	const char *home = ui_test_web_html();
	CHECK(strstr(home, "<h1>Început</h1>") != NULL);
	CHECK(strstr(home, "o hartă în JavaScript") != NULL);
	CHECK(strstr(home, "JavaScript este oprit.") != NULL);
	CHECK(strstr(home, "JavaScript funcționează: ") != NULL);
	ui_test_network_hold(true);
	CHECK(go("https://example.org/"));
	CHECK(ui_test_shows("Se încarcă..."));
	ui_test_network_hold(false);
	net_wait();
}
