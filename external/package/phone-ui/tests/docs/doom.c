/* docs/external/doom.md: Doom, the plugin app, its engine the tests' (fake_doom/doomengine.c,
 * frames of one color: only the controls are worth showing), a game to play */

struct fake_doom fake_doom;

/* An IWAD of one map, enough for Doom to list and start it */
static void doom_wad(const char *path)
{
	uint8_t wad[12 + 1 + 16] = "IWAD";
	wad[4] = 1;  /* lumps */
	wad[8] = 13; /* the directory's offset */
	wad[13] = 12; /* the lump's offset; its size */
	wad[17] = 1;
	memcpy(wad + 21, "MAP01", 5);
	ui_test_write(path, "");
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(wad, 1, sizeof(wad), f);
		fclose(f);
	}
}

static void doom_seed(void)
{
	plugin_install(DOOM_APP, "doom", "doom.so", "doomengine.so");
	doom_wad("/var/lib/doom/wads/doom2.wad");
	fake_doom.color = 0x3a2a20;
}

/* doom.md: playing upright, the controls under the game */
TEST(doom_playing)
{
	CHECK(ui_test_open("Doom"));
	CHECK(ui_test_tap_text("Doom II"));
	CHECK(ui_test_wait_for("FIRE", 2000));
	CHECK(ui_test_shows("MOVE"));
	ui_test_wait(500); /* its frames drawn */
	shot("doom-playing");
}
