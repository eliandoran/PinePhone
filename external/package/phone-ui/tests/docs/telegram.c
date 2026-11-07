/* docs/external/telegram.md: Telegram, the plugin app, installed as `just app telegram` does
 * (test_telegram.c's TEST_SETUP), TDLib fake_tdjson.c: signed in, a few chats, one with a
 * link to an article Telegram has an instant view of, and a reply */

#define TELEGRAM_DIR "/var/lib/apps/telegram"
#define TELEGRAM_ANA 4242
#define TELEGRAM_CLUB 5151
#define TELEGRAM_RADU 303
#define TELEGRAM_GONE 6060
#define TELEGRAM_URL "https://spacenotes.example.org/earthrise-the-photo-that-changed-how-we-see-earth"
#define TELEGRAM_TITLE "Earthrise: the photo that changed how we see Earth"

static bool telegram_copy(const char *from, const char *to)
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

/* The app with its API credentials; libtdjson loaded first, as test_telegram.c explains */
static void telegram_install(void)
{
	if (!ui_test_write(TELEGRAM_DIR "/lib/libtdjson.so", "") ||
	    !telegram_copy(TELEGRAM_APP "/app.json", TELEGRAM_DIR "/app.json") ||
	    !telegram_copy(TELEGRAM_APP "/telegram.so", TELEGRAM_DIR "/telegram.so") ||
	    !telegram_copy(TELEGRAM_APP "/lib/libtdjson.so", TELEGRAM_DIR "/lib/libtdjson.so"))
		TEST_FAIL("cannot install %s", TELEGRAM_APP);
	ui_test_write("/var/lib/telegram/api", "12345 0123456789abcdef0123456789abcdef\n");
	if (!dlopen(TELEGRAM_DIR "/lib/libtdjson.so", RTLD_NOW | RTLD_GLOBAL))
		TEST_FAIL("%s", dlerror());
	ui_test_td_account("Sam", "Carter", "samcarter", "447700900123");
}

/* A text message's JSON: `extra` adds fields (a reply, a link preview) */
static void telegram_message(char *buf, size_t size, long long chat, long long id, long long sender,
                             long long ago, const char *text, const char *extra)
{
	snprintf(buf, size,
	         "{\"@type\":\"message\",\"id\":%lld,\"chat_id\":%lld,\"sender_id\":{\"@type\":\"messageSenderUser\","
	         "\"user_id\":%lld},\"is_outgoing\":%s,\"date\":%lld,%s\"content\":{\"@type\":\"messageText\","
	         "\"text\":{\"@type\":\"formattedText\",\"text\":\"%s\",\"entities\":[]}%s}}",
	         id, chat, sender, sender == FAKE_TD_ME ? "true" : "false", (long long)NOW - ago,
	         extra && strstr(extra, "reply_to") ? extra : "", text,
	         extra && strstr(extra, "link_preview") ? extra : "");
}

/* A chat in the main list, its last message (JSON), unread count, order */
static void telegram_chat(long long id, const char *title, const char *type, const char *last, int unread,
                          int order)
{
	char buf[4096];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateNewChat\",\"chat\":{\"@type\":\"chat\",\"id\":%lld,\"title\":\"%s\","
	         "\"type\":%s,\"photo\":null,\"unread_count\":%d,\"last_message\":%s,\"positions\":"
	         "[{\"@type\":\"chatPosition\",\"list\":{\"@type\":\"chatListMain\"},\"order\":\"%d\","
	         "\"is_pinned\":false}]}}", id, title, type, unread, last, order);
	ui_test_td_update(buf);
}

static void telegram_user(long long id, const char *first, const char *last, bool deleted)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
	         "{\"@type\":\"updateUser\",\"user\":{\"@type\":\"user\",\"id\":%lld,\"first_name\":\"%s\","
	         "\"last_name\":\"%s\",\"status\":{\"@type\":\"userStatusRecently\"},\"type\":{\"@type\":\"%s\"}}}",
	         id, first, last, deleted ? "userTypeDeleted" : "userTypeRegular");
	ui_test_td_update(buf);
}

#define TELEGRAM_PLAIN(t) "{\"@type\":\"richTextPlain\",\"text\":\"" t "\"}"
#define TELEGRAM_PARAGRAPH(t) "{\"@type\":\"pageBlockParagraph\",\"text\":" TELEGRAM_PLAIN(t) "}"

/* Ana's chat: her link with its preview, Sam's reply to it, her question; the article */
static void telegram_ana(void)
{
	char link[2048], reply[1024], ask[1024];
	telegram_message(link, sizeof(link), TELEGRAM_ANA, 1048576, TELEGRAM_ANA, 40 * 60,
	                 "This made my morning: " TELEGRAM_URL,
	                 ",\"link_preview\":{\"@type\":\"linkPreview\",\"url\":\"" TELEGRAM_URL "\","
	                 "\"display_url\":\"spacenotes.example.org/earthrise\",\"site_name\":\"Space Notes\","
	                 "\"title\":\"" TELEGRAM_TITLE "\",\"description\":{\"@type\":\"formattedText\","
	                 "\"text\":\"On Christmas Eve 1968, Apollo 8 saw the Earth rise over the Moon.\","
	                 "\"entities\":[]},\"author\":\"\",\"type\":{\"@type\":\"linkPreviewTypeArticle\","
	                 "\"photo\":null},\"instant_view_version\":2}");
	telegram_message(reply, sizeof(reply), TELEGRAM_ANA, 2097152, FAKE_TD_ME, 32 * 60,
	                 "Beautiful. Printing it for the fridge",
	                 "\"reply_to\":{\"@type\":\"messageReplyToMessage\",\"chat_id\":4242,\"message_id\":1048576},");
	telegram_message(ask, sizeof(ask), TELEGRAM_ANA, 3145728, TELEGRAM_ANA, 3 * 60,
	                 "Are you coming to the planetarium tonight?", NULL);
	ui_test_td_message(link);
	ui_test_td_message(reply);
	ui_test_td_message(ask);
	telegram_chat(TELEGRAM_ANA, "Ana Popescu", "{\"@type\":\"chatTypePrivate\",\"user_id\":4242}", ask, 1, 400);

	ui_test_td_file(900, DEMO_DIR "/photos/earthrise.jpg");
	ui_test_td_instant_view(TELEGRAM_URL,
		"{\"@type\":\"webPageInstantView\",\"version\":2,\"view_count\":0,\"is_rtl\":false,\"is_full\":true,"
		"\"blocks\":["
		"{\"@type\":\"pageBlockTitle\",\"title\":" TELEGRAM_PLAIN(TELEGRAM_TITLE) "},"
		"{\"@type\":\"pageBlockAuthorDate\",\"author\":" TELEGRAM_PLAIN("Lena Weiss") ",\"publish_date\":1773100800},"
		TELEGRAM_PARAGRAPH("On Christmas Eve 1968, the three astronauts of Apollo 8 were the first people "
		                   "to see the Earth rise over the Moon's horizon.") ","
		"{\"@type\":\"pageBlockPhoto\",\"photo\":{\"@type\":\"photo\",\"has_stickers\":false,\"sizes\":["
		"{\"@type\":\"photoSize\",\"type\":\"x\",\"width\":1440,\"height\":1440,\"photo\":{\"@type\":\"file\","
		"\"id\":900,\"local\":{\"@type\":\"localFile\",\"path\":\"\",\"is_downloading_completed\":false}}}]},"
		"\"caption\":{\"@type\":\"pageBlockCaption\",\"text\":" TELEGRAM_PLAIN("Earthrise, 24 December 1968") ","
		"\"credit\":" TELEGRAM_PLAIN("NASA") "},\"url\":\"\",\"has_spoiler\":false},"
		"{\"@type\":\"pageBlockHeader\",\"header\":" TELEGRAM_PLAIN("An unplanned photo") "},"
		"{\"@type\":\"pageBlockParagraph\",\"text\":{\"@type\":\"richTexts\",\"texts\":["
		TELEGRAM_PLAIN("Bill Anders took it while the crew was busy photographing the surface: ") ","
		"{\"@type\":\"richTextUrl\",\"text\":" TELEGRAM_PLAIN("the recording") ",\"url\":\"https://example.org\","
		"\"is_cached\":false}," TELEGRAM_PLAIN(" keeps their surprise.") "]}}"
		"]}");
}

/* Before phone-ui starts: what its tests need at hand once Telegram opens */
static void telegram_seed(void)
{
	telegram_install();
}

/* The chats once phone-ui runs: Ana, a group, Mara, someone who deleted their account */
static void telegram_chats(void)
{
	telegram_user(TELEGRAM_ANA, "Ana", "Popescu", false);
	telegram_user(TELEGRAM_RADU, "Radu", "", false);
	telegram_user(TELEGRAM_GONE, "", "", true);
	telegram_ana();
	char last[1024];
	telegram_message(last, sizeof(last), TELEGRAM_CLUB, 1048576, TELEGRAM_RADU, 25 * 60,
	                 "Saturday at 10 at the wall?", NULL);
	telegram_chat(TELEGRAM_CLUB, "Climbing club", "{\"@type\":\"chatTypeBasicGroup\",\"basic_group_id\":51}",
	              last, 4, 300);
	telegram_message(last, sizeof(last), 101, 1048576, FAKE_TD_ME, 3 * 3600, "See you then!", NULL);
	telegram_chat(101, "Mara Ionescu", "{\"@type\":\"chatTypePrivate\",\"user_id\":101}", last, 0, 200);
	telegram_message(last, sizeof(last), TELEGRAM_GONE, 1048576, TELEGRAM_GONE, 40 * 86400,
	                 "Thanks for the tickets", NULL);
	telegram_chat(TELEGRAM_GONE, "", "{\"@type\":\"chatTypePrivate\",\"user_id\":6060}", last, 0, 100);
}

static bool telegram_open_ana(void)
{
	telegram_chats();
	return ui_test_open("Telegram") && ui_test_wait_for("Ana Popescu", 2000) &&
	       ui_test_tap_text("Ana Popescu") && ui_test_wait_for("Are you coming to the planetarium tonight?", 2000);
}

/* telegram.md: the chat list, a deleted account last */
TEST(telegram_chats)
{
	telegram_chats();
	CHECK(ui_test_open("Telegram"));
	CHECK(ui_test_wait_for("Ana Popescu", 2000));
	CHECK(ui_test_shows("Climbing club"));
	CHECK(ui_test_shows("Deleted Account"));
	CHECK(ui_test_shows(UI_SYMBOL_GHOST));
	shot("telegram-chats");
}

/* telegram.md: a chat with a link's preview and Instant View, and a reply */
TEST(telegram_chat)
{
	CHECK(telegram_open_ana());
	CHECK(ui_test_shows("Space Notes"));
	CHECK(ui_test_shows("Instant View"));
	CHECK(ui_test_shows("Beautiful. Printing it for the fridge"));
	shot("telegram-chat");
}

/* telegram.md: a message swiped left, the reply bar over the field */
TEST(telegram_reply)
{
	CHECK(telegram_open_ana());
	lv_obj_t *label = ui_test_find("Are you coming to the planetarium tonight?");
	CHECK(label != NULL);
	if (!label)
		return;
	lv_area_t a;
	lv_obj_get_coords(lv_obj_get_parent(label), &a);
	ui_test_swipe((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, (a.x1 + a.x2) / 2 - 200, (a.y1 + a.y2) / 2, 300);
	CHECK(ui_test_textarea() != NULL);
	CHECK(ui_test_type("Yes, see you there"));
	shot("telegram-reply");
}

/* telegram.md: the article's Instant View, its photo downloaded */
TEST(telegram_instant_view)
{
	CHECK(telegram_open_ana());
	CHECK(ui_test_tap_text("Instant View"));
	CHECK(ui_test_wait_for("Lena Weiss \xc2\xb7 10 March 2026", 2000));
	CHECK(ui_test_shows(TELEGRAM_TITLE));
	ui_test_wait(500); /* the photo decoded */
	CHECK(ui_test_find_widget(&lv_3dtexture_class) != NULL);
	shot("telegram-instant-view");
}
