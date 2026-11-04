/* Files (files.c), through the UI: phone-ui on the host (ui_test.h). It browses the
 * sandbox's file system: the host's, read-only, with the test's /var/lib. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define DOCS "/var/lib/docs"

TEST_SETUP
{
	ui_test_start();
}

/* Files, then /var/lib and `folder` in it */
static bool open_folder(const char *folder)
{
	return ui_test_open("Files") && ui_test_tap_text("var") && ui_test_tap_text("lib") &&
	       ui_test_tap_text(folder);
}

static int32_t y_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	if (!label)
		return -1;
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	return a.y1;
}

/* The preview's text: the label in the card after the details */
static const char *preview(void)
{
	lv_obj_t *folder = ui_test_find("Folder");
	if (!folder)
		return NULL;
	lv_obj_t *body = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(folder)));
	lv_obj_t *card = lv_obj_get_child(body, -1);
	lv_obj_t *label = lv_obj_get_child(card, 0);
	return label && lv_obj_check_type(label, &lv_label_class) ? lv_label_get_text(label) : NULL;
}

TEST(root)
{
	CHECK(ui_test_open("Files"));
	CHECK(ui_test_shows("Files"));
	CHECK(ui_test_shows("usr"));
	CHECK(ui_test_shows("var"));
	CHECK(ui_test_tap_text("var"));
	CHECK(ui_test_shows("var"));  /* the title */
	CHECK(ui_test_shows("lib"));
	/* Back goes up */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("usr"));
}

TEST(folders_first_by_name)
{
	ui_test_write(DOCS "/b-folder/x", "");
	ui_test_write(DOCS "/a.txt", "Hello\nworld");
	ui_test_write(DOCS "/C.png", "\x89PNG\r\n\x1a\n");
	ui_test_write(DOCS "/song.mp3", "");
	ui_test_write(DOCS "/movie.mkv", "");
	CHECK(open_folder("docs"));
	CHECK(y_of("b-folder") < y_of("a.txt"));
	CHECK(y_of("a.txt") < y_of("C.png"));
	CHECK(y_of("C.png") < y_of("movie.mkv"));
	CHECK(y_of("movie.mkv") < y_of("song.mp3"));
	/* Files have their size and date, folders an arrow */
	CHECK(ui_test_shows_part("11 B, "));
	CHECK(ui_test_shows_part("0 B, "));
	CHECK(ui_test_shows(LV_SYMBOL_RIGHT));
	CHECK(ui_test_shows(LV_SYMBOL_DIRECTORY));
	CHECK(ui_test_shows(LV_SYMBOL_IMAGE));
	CHECK(ui_test_shows(LV_SYMBOL_AUDIO));
	CHECK(ui_test_shows(LV_SYMBOL_VIDEO));
	CHECK(ui_test_shows(LV_SYMBOL_EDIT));

	CHECK(ui_test_tap_text("b-folder"));
	CHECK(ui_test_shows("x"));
}

TEST(text_file)
{
	ui_test_write(DOCS "/a.txt", "Hello\nworld\n\n");
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("a.txt"));
	CHECK(ui_test_shows("a.txt"));
	CHECK(ui_test_shows("Size"));
	CHECK(ui_test_shows("13 B"));
	CHECK(ui_test_shows("Modified"));
	CHECK(ui_test_shows("/var/lib/docs"));
	CHECK_STR(preview(), "Hello\nworld"); /* without the last newlines */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("docs"));
}

TEST(binary_and_empty_files_not_shown)
{
	ui_test_write(DOCS "/data.bin", "");
	FILE *f = fopen(DOCS "/data.bin", "w");
	fwrite("ab\0cd", 1, 5, f);
	fclose(f);
	ui_test_write(DOCS "/empty", "");
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("data.bin"));
	CHECK(ui_test_shows("5 B"));
	CHECK_STR(preview(), NULL);
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("empty"));
	CHECK(ui_test_shows("0 B"));
	CHECK_STR(preview(), NULL);
}

TEST(control_characters_as_dots)
{
	ui_test_write(DOCS "/log", "a\x01" "b\tc\r\n");
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("log"));
	CHECK_STR(preview(), "a.b\tc.");
}

TEST(long_text_cut)
{
	/* 50 lines: 40 shown */
	char text[1024] = "";
	for (int i = 1; i <= 50; i++)
		snprintf(text + strlen(text), sizeof(text) - strlen(text), "line %d\n", i);
	ui_test_write(DOCS "/lines", text);
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("lines"));
	const char *p = preview();
	CHECK(p && strstr(p, "line 1\n") && !strchr(p + strlen(p) - 1, '\n'));
	CHECK(p && strlen(p) > 7 && !strcmp(p + strlen(p) - 7, "line 40"));
}

TEST(long_text_cut_between_characters)
{
	/* 2048 bytes read: "x" then two-byte characters, the last one cut in half */
	char text[3001] = "x";
	for (int i = 1; i + 1 < 3000; i += 2)
		memcpy(text + i, "\xc3\xa9", 2);
	text[2999] = '\0';
	ui_test_write(DOCS "/accents", text);
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("accents"));
	const char *p = preview();
	CHECK(p != NULL);
	if (p) {
		CHECK_INT(strlen(p), 2047);
		CHECK(!memcmp(p + 2045, "\xc3\xa9", 2));
	}
}

TEST(empty_folder)
{
	mkdir(DOCS, 0755);
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("Empty folder"));
}

TEST(folder_not_readable)
{
	mkdir(DOCS, 0);
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("Cannot open this folder"));
	chmod(DOCS, 0755);
}

TEST(many_entries)
{
	char path[64];
	mkdir(DOCS, 0755);
	for (int i = 0; i < 503; i++) {
		snprintf(path, sizeof(path), DOCS "/f%03d", i);
		ui_test_write(path, "");
	}
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("3 more not shown"));
}

TEST(links)
{
	ui_test_write(DOCS "/target.txt", "linked");
	mkdir(DOCS "/dir", 0755);
	CHECK(symlink("target.txt", DOCS "/link") == 0);
	CHECK(symlink("dir", DOCS "/dirlink") == 0);
	CHECK(symlink("nowhere", DOCS "/broken") == 0);
	CHECK(open_folder("docs"));
	/* As what they point to */
	CHECK(y_of("dirlink") < y_of("broken"));
	CHECK(ui_test_tap_text("link"));
	CHECK_STR(preview(), "linked");
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("broken"));
	CHECK(ui_test_shows("7 B")); /* the link itself */
	CHECK_STR(preview(), NULL);
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("dirlink"));
	CHECK(ui_test_shows("Empty folder"));
}
