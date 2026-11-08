/* docs/apps/files.md's screenshots (included by test_docs.c) */

/* Files, then its place `place`, as a user taps them */
static bool files_open_place(const char *place)
{
	return ui_test_open("Files") && ui_test_shows("Files") && ui_test_tap_text(place);
}

/* files.md: where it starts, the folders a computer sees, then All files */
TEST(files_places)
{
	mkdir("/var/lib/photos", 0755);
	mkdir("/var/lib/music", 0755);
	mkdir("/var/lib/books", 0755);
	mkdir("/var/lib/notes", 0755);
	mkdir("/var/lib/memos", 0755);
	CHECK(ui_test_open("Files"));
	CHECK(ui_test_shows("Photos"));
	CHECK(ui_test_shows("Memos"));
	CHECK(ui_test_shows("All files"));
	shot("files-places");
}

/* files.md: a folder, its files with their size and date */
TEST(files_folder)
{
	/* Photos of this morning, of this week and older (gallery_photo(): docs/gallery.c) */
	gallery_photo("IMG_20260314_091204.jpg", 480, 640, &gallery_scenes[0], NOW - 29 * 60);
	gallery_photo("IMG_20260314_084710.jpg", 480, 640, &gallery_scenes[1], NOW - 54 * 60);
	gallery_photo("IMG_20260313_183002.jpg", 640, 480, &gallery_scenes[3], NOW - 911 * 60);
	gallery_photo("IMG_20260311_120116.jpg", 480, 640, &gallery_scenes[4], NOW - 3 * 86400);
	gallery_photo("IMG_20260301_164522.jpg", 480, 640, &gallery_scenes[5], NOW - 13 * 86400);
	gallery_photo("SCR_20260310_200514.jpg", 720, 1440, &gallery_scenes[2], NOW - 4 * 86400);
	mkdir(GALLERY_PHOTOS "/Holidays", 0755);
	CHECK(files_open_place("Photos"));
	CHECK(ui_test_shows("Photos")); /* the title */
	CHECK(!ui_test_shows("All files"));
	CHECK(ui_test_shows("Holidays"));
	CHECK(ui_test_shows_part(", 09:12"));
	CHECK(ui_test_shows_part(", Fri"));
	CHECK(ui_test_shows_part(", 1 Mar"));
	CHECK(ui_test_shows_part(" kB, "));
	/* Folders first */
	lv_obj_t *dir = ui_test_find("Holidays"), *file = ui_test_find("IMG_20260301_164522.jpg");
	CHECK(dir && file);
	if (dir && file) {
		lv_area_t a, b;
		lv_obj_get_coords(dir, &a);
		lv_obj_get_coords(file, &b);
		CHECK(a.y1 < b.y1);
	}
	CHECK(ui_test_shows(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_shows(LV_SYMBOL_IMAGE));
	shot("files-folder");
	/* Back goes to the places */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("All files"));
}

/* files.md: a text file's details and the start of it */
TEST(files_text)
{
	static const char note[] =
		"Shopping list\n"
		"- oat milk\n"
		"- coffee beans\n"
		"- 6 eggs\n"
		"- tomatoes, basil\n"
		"- bread from the bakery on Elm Street\n"
		"\n"
		"Call the plumber about the kitchen tap before Tuesday.\n";
	ui_test_write("/var/lib/notes/20260312-191503.txt", note);
	struct timeval tv[2] = { { NOW - 2 * 86400 + 600, 0 }, { NOW - 2 * 86400 + 600, 0 } };
	utimes("/var/lib/notes/20260312-191503.txt", tv);
	CHECK(files_open_place("Notes"));
	CHECK(ui_test_tap_text("20260312-191503.txt"));
	CHECK(ui_test_shows("Size"));
	char size[16];
	snprintf(size, sizeof(size), "%zu B", sizeof(note) - 1);
	CHECK(ui_test_shows(size));
	CHECK(ui_test_shows("Modified"));
	CHECK(ui_test_shows("Thu"));
	CHECK(ui_test_shows("Folder"));
	CHECK(ui_test_shows("/var/lib/notes"));
	CHECK(ui_test_shows_part("Shopping list\n- oat milk"));
	shot("files-text");
}
