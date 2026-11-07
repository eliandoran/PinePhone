/* docs/external/github.md: GitHub, the plugin app, signed in, GitHub's API answered from
 * here (fake_curl.c): the notifications, a pull request with its conversation and files */

#define GITHUB_API "https://api.github.com"
#define GITHUB_PR GITHUB_API "/repos/lena/moonbase/pulls/12"

static void github_seed(void)
{
	plugin_install(GITHUB_APP, "github", "github.so", NULL);
	ui_test_write("/var/lib/app-data/github/token", "ghp_docs\n");
}

time_t __real_time(time_t *t);

/* `ago` hours ago, GitHub's ISO time: by the real clock, which the plugin reads (only phone-ui's
 * time() is the tests') */
static const char *github_time(char *buf, size_t size, int ago)
{
	time_t t = __real_time(NULL) - ago * 3600;
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

static void github_serve(void)
{
	char t[5][32], buf[4096];
	snprintf(buf, sizeof(buf),
	         "[{\"id\": \"1\", \"unread\": true, \"reason\": \"review_requested\", \"updated_at\": \"%s\","
	         "  \"subject\": {\"title\": \"Show the battery's temperature\", \"type\": \"PullRequest\","
	         "    \"url\": \"" GITHUB_PR "\"}, \"repository\": {\"full_name\": \"lena/moonbase\"}},"
	         " {\"id\": \"2\", \"unread\": true, \"reason\": \"mention\", \"updated_at\": \"%s\","
	         "  \"subject\": {\"title\": \"Wi-Fi drops after waking up\", \"type\": \"Issue\","
	         "    \"url\": \"" GITHUB_API "/repos/lena/moonbase/issues/9\"},"
	         "  \"repository\": {\"full_name\": \"lena/moonbase\"}},"
	         " {\"id\": \"3\", \"unread\": false, \"reason\": \"subscribed\", \"updated_at\": \"%s\","
	         "  \"subject\": {\"title\": \"v2.4\", \"type\": \"Release\", \"url\": null},"
	         "  \"repository\": {\"full_name\": \"orbit-tools/telemetry\"}},"
	         " {\"id\": \"4\", \"unread\": false, \"reason\": \"comment\", \"updated_at\": \"%s\","
	         "  \"subject\": {\"title\": \"Which charger do you use?\", \"type\": \"Discussion\", \"url\": null},"
	         "  \"repository\": {\"full_name\": \"lena/moonbase\"}}]",
	         github_time(t[0], 32, 1), github_time(t[1], 32, 3), github_time(t[2], 32, 30),
	         github_time(t[3], 32, 75));
	ui_test_serve(GITHUB_API "/notifications*", 200, "application/json", buf);
	ui_test_serve(GITHUB_API "/notifications/threads/*", 205, "text/plain", "");
	ui_test_serve(GITHUB_API "/user", 200, "application/json", "{\"login\": \"sam\"}");
	snprintf(buf, sizeof(buf),
	         "{\"number\": 12, \"title\": \"Show the battery's temperature\", \"state\": \"open\","
	         " \"merged\": false, \"draft\": false, \"user\": {\"login\": \"lena\"}, \"created_at\": \"%s\","
	         " \"body\": \"Reads it from the PMIC and shows it in **Settings**.\","
	         " \"base\": {\"ref\": \"main\"}, \"head\": {\"ref\": \"battery-temp\", \"sha\": \"abc123\"},"
	         " \"changed_files\": 1}", github_time(t[0], 32, 20));
	ui_test_serve(GITHUB_PR, 200, "application/json", buf);
	snprintf(buf, sizeof(buf),
	         "[{\"id\": 1, \"user\": {\"login\": \"lena\"}, \"created_at\": \"%s\","
	         "  \"body\": \"Tested on my phone: it shows 31 \\u00b0C while charging.\"},"
	         " {\"id\": 2, \"user\": {\"login\": \"sam\"}, \"created_at\": \"%s\","
	         "  \"body\": \"Nice! Could it also warn above 45 \\u00b0C?\"}]",
	         github_time(t[1], 32, 6), github_time(t[2], 32, 2));
	ui_test_serve(GITHUB_API "/repos/lena/moonbase/issues/12/comments?*", 200, "application/json", buf);
	snprintf(buf, sizeof(buf),
	         "[{\"id\": 5, \"user\": {\"login\": \"marco\"}, \"state\": \"APPROVED\","
	         "  \"submitted_at\": \"%s\", \"body\": \"\"}]", github_time(t[3], 32, 4));
	ui_test_serve(GITHUB_PR "/reviews*", 200, "application/json", buf);
	ui_test_serve(GITHUB_PR "/files*", 200, "application/json",
	              "[{\"filename\": \"src/battery.c\", \"status\": \"modified\", \"additions\": 6, \"deletions\": 1,"
	              "  \"patch\": \"@@ -40,6 +40,11 @@ static void battery_update(void)\\n"
	              " \\tint level = read_level();\\n"
	              "-\\tlabel_set(level_label, level);\\n"
	              "+\\tlabel_set(level_label, level);\\n"
	              "+\\t/* The PMIC's sensor, in tenths of a degree */\\n"
	              "+\\tint temp = read_long(BATTERY \\\"/temp\\\");\\n"
	              "+\\tif (temp > 0)\\n"
	              "+\\t\\tlabel_set_fmt(temp_label, \\\"%d \\u00b0C\\\", temp / 10);\\n"
	              "+\\n"
	              " \\tif (level < 15)\\n"
	              " \\t\\twarn_low();\\n"
	              " }\"}]");
}

static bool github_open_pr(void)
{
	github_serve();
	return ui_test_open("GitHub") && ui_test_tap_text("Notifications") &&
	       ui_test_wait_for("Show the battery's temperature", 2000) &&
	       ui_test_tap_text("Show the battery's temperature") && ui_test_wait_for("Overview", 2000);
}

/* github.md: the notifications */
TEST(github_notifications)
{
	github_serve();
	CHECK(ui_test_open("GitHub"));
	CHECK(ui_test_tap_text("Notifications"));
	CHECK(ui_test_wait_for("Wi-Fi drops after waking up", 2000));
	CHECK(ui_test_shows("Which charger do you use?"));
	shot("github-notifications");
}

/* github.md: a pull request's conversation, a review among the comments */
TEST(github_conversation)
{
	CHECK(github_open_pr());
	CHECK(ui_test_tap_text("Conversation"));
	CHECK(ui_test_wait_for("marco approved", 2000));
	CHECK(ui_test_shows("Nice! Could it also warn above 45 \xc2\xb0" "C?"));
	shot("github-conversation");
}

/* github.md: a pull request's changes, colored */
TEST(github_files)
{
	CHECK(github_open_pr());
	CHECK(ui_test_tap_text("Files"));
	CHECK(ui_test_wait_for("src/battery.c", 2000));
	shot("github-files");
}
