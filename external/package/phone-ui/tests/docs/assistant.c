/* docs/external/assistant.md's screenshots (included by test_docs.c): Assistant, the plugin
 * app, DeepSeek's API served by the tests, a question answered with a web search, earlier chats */

static void assistant_seed(void)
{
	plugin_install(ASSISTANT_APP, "assistant", "assistant.so", NULL);
}

#define ASSISTANT_CHAT "https://api.deepseek.com/chat/completions"
#define ASSISTANT_SEARCH "https://api.deepseek.com/v1/tools/search"
#define ASSISTANT_SSE "text/event-stream"
#define ASSISTANT_DELTA(json) "data: {\"choices\":[{\"index\":0,\"delta\":" json "}]}\n\n"
#define ASSISTANT_QUESTION "Will it rain in Edinburgh this afternoon?"
#define ASSISTANT_ANSWER "Probably, yes. Today's forecasts give a 60 to 70% chance of light rain " \
	"from about 13:00, easing off in the evening, with a high of around 10\xc2\xb0" "C. " \
	"Take an umbrella if you are out after lunch."

/* Two chats of the days before */
static void assistant_earlier_chats(void)
{
	ui_test_write("/var/lib/assistant/1773344400.json",
	              "{\"messages\":[\n"
	              "{\"role\":\"user\",\"time\":1773344400,\"content\":\"How do I say \\\"where is the station\\\" in French?\"},\n"
	              "{\"role\":\"assistant\",\"time\":1773344404,\"content\":\"O\xc3\xb9 est la gare ?\"}]}\n");
	ui_test_write("/var/lib/assistant/1773429600.json",
	              "{\"messages\":[\n"
	              "{\"role\":\"user\",\"time\":1773429600,\"content\":\"How long should I boil an egg for a soft yolk?\"},\n"
	              "{\"role\":\"assistant\",\"time\":1773429606,\"content\":\"About 6 minutes from boiling water "
	              "for a large egg, then into cold water for a minute.\"}]}\n");
}

/* The settings: a key typed in, web search on */
static void assistant_set_up(void)
{
	CHECK(ui_test_tap_text(LV_SYMBOL_SETTINGS));
	CHECK(ui_test_shows("Assistant settings"));
	CHECK(ui_test_tap_field("API key"));
	CHECK(ui_test_type("sk-0123456789abcdef"));
	lv_obj_t *switches[2];
	CHECK_INT(ui_test_find_widgets(&lv_switch_class, switches, 2), 2);
	ui_test_tap(switches[1]); /* Web search */
	CHECK(lv_obj_has_state(switches[1], LV_STATE_CHECKED));
	CHECK(ui_test_back());
}

/* assistant.md: setting it up */
TEST(assistant_settings)
{
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_shows("No chats"));
	CHECK(ui_test_tap_text(LV_SYMBOL_SETTINGS));
	CHECK(ui_test_shows("Assistant settings"));
	CHECK(ui_test_shows("DeepSeek"));
	CHECK(ui_test_shows("Create one on platform.deepseek.com, API keys."));
	CHECK(ui_test_shows("deepseek-flash"));
	CHECK(ui_test_shows("Fast, cheap"));
	CHECK(ui_test_shows("deepseek-v4-pro"));
	CHECK(ui_test_shows("Smarter, slower"));
	CHECK(ui_test_shows("Thinking"));
	CHECK(ui_test_shows("Better answers, takes much longer"));
	CHECK(ui_test_shows("Web search"));
	CHECK(ui_test_shows("Looks things up, $0.75 per 1000 searches"));
	shot("assistant-settings");
}

/* A question asked in a new chat, answered after a web search */
static void assistant_ask(void)
{
	assistant_earlier_chats();
	ui_test_serve(ASSISTANT_CHAT, 200, ASSISTANT_SSE,
	              ASSISTANT_DELTA("{\"content\":\"" ASSISTANT_ANSWER "\"}") "data: [DONE]\n\n");
	ui_test_serve_once(ASSISTANT_CHAT, 200, ASSISTANT_SSE,
	                   ASSISTANT_DELTA("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
	                                   "\"function\":{\"name\":\"web_search\",\"arguments\":"
	                                   "\"{\\\"query\\\":\\\"Edinburgh weather forecast 14 March 2026\\\"}\"}}]}")
	                   "data: [DONE]\n\n");
	ui_test_serve(ASSISTANT_SEARCH, 200, "application/json",
	              "{\"results\":[{\"title\":\"Edinburgh: hourly forecast\",\"url\":\"https://forecast.example/edinburgh\","
	              "\"summary\":\"Light rain from 13:00, 60-70%. High 10, low 3. Wind W 19 km/h.\","
	              "\"date\":\"2026-03-14T06:00:00Z\"}]}");
	CHECK(ui_test_open("Assistant"));
	assistant_set_up();
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows("DeepSeek"));
	CHECK(ui_test_shows("Ask a question below"));
	CHECK(ui_test_type(ASSISTANT_QUESTION "\n"));
	CHECK(ui_test_wait_for(ASSISTANT_ANSWER, 3000));
	ui_test_wait(300);
	ui_test_settle();
	const struct ui_test_request *s = ui_test_requested(ASSISTANT_SEARCH);
	CHECK(s && strstr(s->headers, "Authorization: Bearer sk-0123456789abcdef\n") != NULL);
}

/* assistant.md: a question and its answer */
TEST(assistant_chat)
{
	assistant_ask();
	CHECK(ui_test_shows(ASSISTANT_QUESTION));
	CHECK(ui_test_shows(LV_SYMBOL_OK " Searched the web: Edinburgh weather forecast 14 March 2026"));
	CHECK(ui_test_shows("09:41"));
	shot("assistant-chat");
}

/* assistant.md: the chats, newest first */
TEST(assistant_chats)
{
	assistant_ask();
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Assistant"));
	CHECK(ui_test_shows(ASSISTANT_QUESTION));
	CHECK(ui_test_shows_part("Probably, yes."));
	CHECK(ui_test_shows("How long should I boil an egg for a soft yolk?"));
	CHECK(ui_test_shows("Fri"));
	CHECK(ui_test_shows("Thu"));
	lv_obj_t *newest = ui_test_find(ASSISTANT_QUESTION), *older = ui_test_find("Fri");
	CHECK(newest && older);
	if (newest && older) {
		lv_area_t a, b;
		lv_obj_get_coords(newest, &a);
		lv_obj_get_coords(older, &b);
		CHECK(a.y1 < b.y1);
	}
	shot("assistant-chats");
}
