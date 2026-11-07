/*
 * Doom (../../../../apps/doom), a plugin app: doom.so built for the host (DOOM_APP), without
 * its engine (lib/doomengine.so, loaded when a game starts), installed as `just app doom`
 * installs it. Its list of games.
 */

#include <stdio.h>

#include "test.h"
#include "ui_test.h"

#define APP "/var/lib/apps/doom"

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

TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "") || !copy(DOOM_APP "/app.json", APP "/app.json") ||
	    !copy(DOOM_APP "/doom.so", APP "/doom.so"))
		TEST_FAIL("cannot install %s", DOOM_APP);
}

/* Without IWADs: where to copy them, its text kept (a static label) */
TEST(no_games)
{
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_shows("No games"));
	CHECK(ui_test_shows("Copy IWADs (doom2.wad, freedoom1.wad...) to /var/lib/doom/wads"));
}
