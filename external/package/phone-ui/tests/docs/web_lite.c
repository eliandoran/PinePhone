/* docs/apps/web-lite.md's screenshots (included by test_docs.c): the start page, and a
 * made-up community garden's site served by the tests */

#define WEB_LITE_SITE "https://millbrook-garden.example"

static const char web_lite_html[] =
	"<!DOCTYPE html><html><head><title>Millbrook Community Garden</title><style>"
	"body { margin: 0; background: #fbfaf5; color: #2b2b2b; }"
	"header { background: #3f7d3a; color: #fff; padding: 20px 16px; }"
	"header h1 { margin: 0; font-size: 26px; }"
	"header p { margin: 4px 0 0; font-size: 14px; color: #dcebd8; }"
	"nav { background: #e5efdf; padding: 10px 16px; }"
	"nav a { color: #2d5e29; margin-right: 16px; font-weight: bold; }"
	".main { padding: 4px 16px 24px; }"
	"h2 { font-size: 20px; color: #3f7d3a; margin: 20px 0 8px; }"
	".note { background: #fff3cd; border-left: 4px solid #e0a800; padding: 10px 12px; }"
	"li { margin-bottom: 6px; }"
	"a { color: #2d5e29; }"
	"</style></head><body>"
	"<header><h1>Millbrook Community Garden</h1><p>Growing together since 1998</p></header>"
	"<nav><a href=\"/plots\">Plots</a><a href=\"/events\">Events</a><a href=\"/contact\">Contact</a></nav>"
	"<div class=\"main\">"
	"<p class=\"note\"><b>Next work day:</b> Saturday 21 March, 10:00. Bring gloves; tea and cake provided.</p>"
	"<h2>Spring planting</h2>"
	"<p>The beds by the gate are ready for potatoes and onions. Seed packets are in the shed, "
	"free for members: take what you will sow this month.</p>"
	"<h2>This month</h2>"
	"<ul><li>Sow broad beans and peas outside</li>"
	"<li>Start tomatoes on a sunny windowsill</li>"
	"<li>Mulch the fruit bushes</li></ul>"
	"<h2>Plots</h2>"
	"<p>Two half plots are free. <a href=\"/plots\">See the waiting list</a>.</p>"
	"</div></body></html>";

/* web-lite.md: the start page */
TEST(web_lite_start)
{
	CHECK(ui_test_open("Web Lite"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("Web Lite"));
	CHECK(ui_test_find_widget(&lv_textarea_class) != NULL);
	CHECK_STR(lv_textarea_get_text(ui_test_find_widget(&lv_textarea_class)), "");
	CHECK_INT(ui_test_request_count(), 0);
	shot("web-lite-start");
}

/* web-lite.md: an address typed in the bar, its page */
TEST(web_lite_page)
{
	ui_test_serve(WEB_LITE_SITE, 200, "text/html; charset=utf-8", web_lite_html);
	CHECK(ui_test_open("Web Lite"));
	CHECK(ui_test_tap_field("Address or search"));
	CHECK(ui_test_type("millbrook-garden.example\n"));
	ui_test_wait(500);
	ui_test_settle();
	CHECK(ui_test_requested(WEB_LITE_SITE) != NULL);
	CHECK(ui_test_shows("Millbrook Community Garden")); /* the title, in the header */
	lv_obj_t *ta = ui_test_find_widget(&lv_textarea_class);
	CHECK(ta != NULL);
	if (ta)
		CHECK_STR(lv_textarea_get_text(ta), WEB_LITE_SITE);
	lv_color_t c = ui_test_pixel(700, 160); /* the site's green banner */
	CHECK(c.red == 0x3f && c.green == 0x7d && c.blue == 0x3a);
	shot("web-lite-page");
}
