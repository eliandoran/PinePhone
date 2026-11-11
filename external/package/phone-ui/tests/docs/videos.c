/* docs/apps/videos.md's screenshots (included by test_docs.c). The fake engine
 * (fake_video.c) draws a video as one color: the player's shot shows its controls. */

#define VIDEOS_DIR "/var/lib/videos"

/* A video of a size as a camera's would be (sparse: the fake reads its first line) */
static void videos_file(const char *name, int w, int h, uint32_t ms, uint32_t rgb, off_t size)
{
	char path[128];
	snprintf(path, sizeof(path), VIDEOS_DIR "/%s", name);
	ui_test_video(path, w, h, ms, rgb);
	CHECK(truncate(path, size) == 0);
}

static void videos_files(void)
{
	videos_file("Birthday party.mp4", 1920, 1080, 754000, 0x3a6ea5, 1288490188);
	videos_file("Hiking in the mountains.mp4", 1280, 720, 312000, 0x4e8a3c, 248512000);
	videos_file("Snow day.mkv", 720, 1280, 48000, 0xb8c4d0, 31457280);
}

/* videos.md: the list, by name */
TEST(videos_list)
{
	videos_files();
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_shows("Birthday party"));
	CHECK(ui_test_shows("Hiking in the mountains"));
	CHECK(ui_test_shows("Snow day"));
	shot("videos-list");
}

/* videos.md: a video playing, upright */
TEST(videos_player)
{
	videos_files();
	CHECK(ui_test_open("Videos"));
	CHECK(ui_test_tap_text("Hiking in the mountains"));
	ui_test_wait(83000);
	CHECK(ui_test_wait_for("1:24", 2000));
	CHECK(ui_test_shows("5:12"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	shot("videos-player");
}
