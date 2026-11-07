/* docs/external/genesis.md: Genesis, the plugin app, its core the tests'
 * (fake_genesis/libretro.c, frames of one color: only the controls are worth showing), a
 * few games, one saved */

struct fake_genesis fake_genesis;

static void genesis_seed(void)
{
	plugin_install(GENESIS_APP, "genesis", "genesis.so", "genesis_plus_gx_libretro.so");
	static const char *const roms[] = { "Moonbase Racer.md", "Orbit Patrol.md", "Starfield Quest.md",
	                                    "Tiny Planets.gg" };
	char path[256];
	for (unsigned i = 0; i < sizeof(roms) / sizeof(roms[0]); i++) {
		snprintf(path, sizeof(path), "/var/lib/genesis/roms/%s", roms[i]);
		ui_test_write(path, "rom");
	}
	ui_test_write("/var/lib/genesis/saves/Orbit Patrol.state", "state");
	fake_genesis.color = 0x10204a;
}

/* genesis.md: the games, one saved */
TEST(genesis_games)
{
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_wait_for("Orbit Patrol", 2000));
	CHECK(ui_test_shows("Saved"));
	shot("genesis-games");
}

/* genesis.md: playing upright, the gamepad under the game */
TEST(genesis_playing)
{
	CHECK(ui_test_open("Genesis"));
	CHECK(ui_test_tap_text("Moonbase Racer"));
	CHECK(ui_test_wait_for("START", 2000));
	ui_test_wait(500); /* its frames drawn */
	shot("genesis-playing");
}
