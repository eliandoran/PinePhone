/*
 * Plugin apps and the headers they were built against (external.c): fake.so
 * (fake_plugin.c, FAKE_APP, the Makefile's) installed in /var/lib/apps/fake with the
 * app_abi a test gives it (FAKE_PLUGIN_ABI in the environment), UI_ABI (abi.h, this
 * phone-ui's) doctored: a declaration added since the plugin was built keeps it, one
 * changed or removed, or other LVGL headers, retire it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "test.h"
#include "ui.h"
#include "ui_test.h"

#define APP "/var/lib/apps/fake"

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

/* As `just app` would install it */
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "{\"name\": \"Fake\", \"plugin\": \"fake.so\"}\n") ||
	    !ui_test_write(APP "/fake.so", "") || !copy(FAKE_APP "/fake.so", APP "/fake.so"))
		TEST_FAIL("cannot install the plugin");
}

/* phone-ui started with the plugin's app_abi `abi`, whether it is in the menu */
static bool loaded_with(const char *abi)
{
	setenv("FAKE_PLUGIN_ABI", abi, 1);
	ui_test_start();
	for (unsigned i = 0; i < ui_app_count; i++)
		if (strcmp(ui_apps[i].name, "Fake") == 0)
			return true;
	return false;
}

/* What its app_start() noted (fake_plugin.c), "" if nothing */
#define STARTED "/tmp/fake-plugin-started"
static const char *started(void)
{
	static char buf[256];
	FILE *f = fopen(STARTED, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

/* UI_ABI's entry (`name:hash`) with that name, "lvgl" or the first other one, as a
 * static copy */
static const char *entry(const char *name)
{
	static char buf[256];
	const char *abi = UI_ABI;
	for (const char *t = abi; *t; t = strchr(t, ' ') ? strchr(t, ' ') + 1 : t + strlen(t)) {
		size_t n = strcspn(t, " ");
		bool lvgl = strncmp(t, "lvgl:", 5) == 0;
		if (name ? lvgl : !lvgl) {
			snprintf(buf, sizeof(buf), "%.*s", (int)n, t);
			return buf;
		}
	}
	return NULL;
}

/* UI_ABI with `entry` replaced by `by` (NULL: left out), malloc'd */
static char *abi_with(const char *entry, const char *by)
{
	const char *at = strstr(UI_ABI, entry);
	char *abi = malloc(strlen(UI_ABI) + (by ? strlen(by) : 0) + 1);
	if (!at) {
		strcpy(abi, UI_ABI);
		return abi;
	}
	const char *rest = at + strlen(entry);
	if (!by && *rest == ' ')
		rest++;
	sprintf(abi, "%.*s%s%s", (int)(at - UI_ABI), UI_ABI, by ? by : "", rest);
	return abi;
}

TEST(built_for_these_headers)
{
	remove(STARTED);
	CHECK(loaded_with(UI_ABI));
	/* Started once, before it is opened */
	CHECK_STR(started(), "started\n");
	CHECK(ui_test_open("Fake"));
	CHECK(ui_test_shows("The fake plugin runs"));
	CHECK_STR(started(), "started\n");
}

TEST(a_declaration_added_since_keeps_it)
{
	char *abi = abi_with(entry(NULL), NULL);
	CHECK(strlen(abi) < strlen(UI_ABI));
	CHECK(loaded_with(abi));
	CHECK(ui_test_open("Fake"));
	CHECK(ui_test_shows("The fake plugin runs"));
	free(abi);
}

TEST(a_declaration_changed_since_retires_it)
{
	ui_test_plugin_may_not_load();
	char changed[256];
	snprintf(changed, sizeof(changed), "%s", entry(NULL));
	changed[strlen(changed) - 1] ^= 1;
	char *abi = abi_with(entry(NULL), changed);
	CHECK(strlen(abi) == strlen(UI_ABI));
	CHECK(!loaded_with(abi));
	free(abi);
}

TEST(a_declaration_removed_since_retires_it)
{
	ui_test_plugin_may_not_load();
	char abi[sizeof(UI_ABI) + 32];
	snprintf(abi, sizeof(abi), "%s ui_gone:deadbeef", UI_ABI);
	CHECK(!loaded_with(abi));
}

TEST(other_lvgl_headers_retire_it)
{
	ui_test_plugin_may_not_load();
	char changed[256];
	snprintf(changed, sizeof(changed), "%s", entry("lvgl"));
	changed[strlen(changed) - 1] ^= 1;
	char *abi = abi_with(entry("lvgl"), changed);
	CHECK(!loaded_with(abi));
	free(abi);
}

TEST(an_earlier_phone_ui_retires_it)
{
	ui_test_plugin_may_not_load();
	/* Before, UI_ABI was one hash of the headers together */
	CHECK(!loaded_with("0123456789abcdef"));
}

TEST(no_abi_retires_it)
{
	ui_test_plugin_may_not_load();
	CHECK(!loaded_with(""));
}

/* A copy cut short, or not a library at all */
TEST(not_a_library_left_out)
{
	ui_test_plugin_may_not_load();
	CHECK(ui_test_write(APP "/fake.so", "not a shared library\n"));
	CHECK(!loaded_with(UI_ABI));
}
