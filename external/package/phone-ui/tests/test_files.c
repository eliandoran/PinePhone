/* Files (files.c), through the UI: phone-ui on the host (ui_test.h). It browses the
 * sandbox's file system: the host's, read-only, with the test's /var/lib. Its places are
 * the apps' folders (folders.c); an external app's: test_external.c. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "test.h"
#include "i18n.h"
#include "ui_test.h"

#define DOCS "/var/lib/docs"

TEST_SETUP
{
	ui_test_start();
}

/* The list dragged up until `name` is among its rows: only those near the ones in view exist,
 * and the root is the host's (30 folders before "var" on CI's Ubuntu) */
static bool find_entry(const char *name)
{
	for (int i = 0; i < 20 && !ui_test_shows(name); i++)
		ui_test_swipe(360, 1100, 360, 400, 300);
	return ui_test_shows(name);
}

static bool tap_entry(const char *name)
{
	return find_entry(name) && ui_test_tap_text(name);
}

/* Files, All files, then /var/lib and `folder` in it */
static bool open_folder(const char *folder)
{
	return ui_test_open("Files") && ui_test_tap_text("All files") && tap_entry("var") &&
	       ui_test_tap_text("lib") && ui_test_tap_text(folder);
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

/* The preview's text: the label in the card after the details (then Show the whole file) */
static const char *preview(void)
{
	lv_obj_t *folder = ui_test_find("Folder");
	if (!folder)
		return NULL;
	lv_obj_t *details = lv_obj_get_parent(lv_obj_get_parent(folder));
	lv_obj_t *card = lv_obj_get_child(lv_obj_get_parent(details), (int32_t)lv_obj_get_index(details) + 1);
	if (!card)
		return NULL;
	lv_obj_t *label = lv_obj_get_child(card, 0);
	return label && lv_obj_check_type(label, &lv_label_class) ? lv_label_get_text(label) : NULL;
}

/* It starts at the apps' folders, those a computer sees over MTP (made at startup), the
 * built-in apps' only: no external app installed */
TEST(places)
{
	ui_test_write("/var/lib/photos/IMG_1.jpg", "");
	CHECK(ui_test_open("Files"));
	CHECK(ui_test_shows("Files"));
	CHECK(y_of("Photos") < y_of("Music"));
	CHECK(y_of("Music") < y_of("Books"));
	CHECK(y_of("Books") < y_of("Notes"));
	CHECK(y_of("Notes") < y_of("Memos"));
	CHECK(y_of("Memos") < y_of("All files"));
	CHECK(!ui_test_shows("Genesis"));
	CHECK(!ui_test_shows("usr"));
	/* A place opens as a folder, titled by its name */
	CHECK(ui_test_tap_text("Photos"));
	CHECK(ui_test_shows("Photos"));
	CHECK(ui_test_tap_text("IMG_1.jpg"));
	CHECK(ui_test_shows("/var/lib/photos"));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Music"));
	/* Made, empty */
	CHECK(ui_test_tap_text("Memos"));
	CHECK(ui_test_shows("Empty folder"));
}

/* After the apps' folders, phone-ui's logs (S90phone-ui's, also shared over MTP): their own
 * place, no app's */
TEST(logs_place)
{
	ui_test_write("/var/lib/phone-ui/log/phone-ui.log.1", "phone-ui: log of 2026-11-11 20:56:04 UTC\n");
	CHECK(ui_test_open("Files"));
	CHECK(y_of("Memos") < y_of("Logs"));
	CHECK(y_of("Logs") < y_of("All files"));
	CHECK(ui_test_shows("/var/lib/phone-ui/log"));
	CHECK(ui_test_tap_text("Logs"));
	CHECK(ui_test_tap_text("phone-ui.log.1"));
	CHECK(ui_test_shows_part("phone-ui: log of 2026-11-11"));
}

/* The whole text of a file in a virtual list (its rows made for the lines in view), in
 * a monospace font, long lines wrapped. A log (in Logs) opens there at once, at its end,
 * and follows it as it grows while at its end; another text file from its details */

#define LOG "/var/lib/phone-ui/log/phone-ui.log"

/* `n` lines "line 0001"... into `path` (appended) */
static void lines_write(const char *path, unsigned from, unsigned n)
{
	if (from == 1)
		ui_test_write(path, ""); /* made, with its folders */
	FILE *f = fopen(path, from > 1 ? "a" : "w");
	for (unsigned i = from; i < from + n; i++)
		fprintf(f, "line %04u\n", i);
	fclose(f);
}

static bool open_log(void)
{
	return ui_test_open("Files") && ui_test_tap_text("Logs") && ui_test_tap_text("phone-ui.log");
}

TEST(log_opens_at_its_end)
{
	lines_write(LOG, 1, 500);
	CHECK(open_log());
	ui_test_wait(500);
	CHECK(ui_test_shows("line 0500"));
	CHECK(!ui_test_shows("line 0001")); /* not even made: far out of view */
	CHECK(ui_test_font("line 0500") == &ui_font_mono_20);
}

static bool shows_line_0520(void)
{
	ui_test_wait(100);
	return ui_test_shows("line 0520");
}

TEST(log_follows_at_its_end)
{
	lines_write(LOG, 1, 500);
	CHECK(open_log());
	ui_test_wait(500);
	lines_write(LOG, 501, 20);
	CHECK(ui_test_wait_until(shows_line_0520, 3000));
}

/* Scrolled up to read: left there, not taken to the end */
TEST(log_scrolled_up_stays)
{
	lines_write(LOG, 1, 500);
	CHECK(open_log());
	ui_test_wait(500);
	ui_test_swipe(360, 500, 360, 1200, 300);
	ui_test_wait(500);
	CHECK(!ui_test_shows("line 0500"));
	CHECK(ui_test_shows_part("line 04")); /* earlier ones in view */
	lines_write(LOG, 501, 20);
	ui_test_wait(2500);
	CHECK(!ui_test_shows("line 0520"));
	CHECK(!ui_test_shows("line 0500"));
}

static bool shows_restarted(void)
{
	ui_test_wait(100);
	return ui_test_shows("restarted");
}

/* Moved along while open (phone-ui restarted: a new log there): read again from its start */
TEST(log_replaced)
{
	lines_write(LOG, 1, 500);
	CHECK(open_log());
	ui_test_wait(500);
	ui_test_write(LOG, "phone-ui: log of a new start\nrestarted\n");
	CHECK(ui_test_wait_until(shows_restarted, 3000));
	CHECK(ui_test_shows("phone-ui: log of a new start"));
	CHECK(!ui_test_shows("line 0500"));
}

/* A line longer than a row: on the next rows, every character kept */
TEST(log_long_line_wrapped)
{
	char line[160];
	memset(line, 'x', 120);
	strcpy(line + 120, "TAIL");
	ui_test_write(LOG, "");
	FILE *f = fopen(LOG, "w");
	fprintf(f, "first\n%s\nlast\n", line);
	fclose(f);
	CHECK(open_log());
	ui_test_wait(500);
	CHECK(!ui_test_shows_part(line));
	CHECK(ui_test_shows_part("xTAIL"));
	CHECK(ui_test_shows("first"));
	CHECK(ui_test_shows("last"));
}

/* Another text file: its details and start first, the whole of it from there, at its top */
TEST(text_file_whole)
{
	lines_write(DOCS "/long.txt", 1, 300);
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("long.txt"));
	CHECK(ui_test_shows("Size"));
	CHECK(ui_test_tap_text("Show the whole file"));
	ui_test_wait(500);
	CHECK(ui_test_shows("line 0001"));
	CHECK(!ui_test_shows("line 0300"));
}

/* Its details as it is when opened, not when its folder was read */
TEST(file_details_now)
{
	ui_test_write(DOCS "/grows.txt", "a\n");
	CHECK(open_folder("docs"));
	lines_write(DOCS "/grows.txt", 1, 300); /* 3000 bytes */
	CHECK(ui_test_tap_text("grows.txt"));
	CHECK(ui_test_shows("3.0 kB"));
}

/* An app's icon (a texture) on the row of `text` */
static bool app_icon_beside(const char *text)
{
	lv_obj_t *icons[16];
	unsigned n = ui_test_find_widgets(&lv_3dtexture_class, icons, 16);
	lv_obj_t *label = ui_test_find(text);
	if (!label)
		return false;
	lv_area_t a, b;
	lv_obj_get_coords(label, &a);
	for (unsigned i = 0; i < n && i < 16; i++) {
		lv_obj_get_coords(icons[i], &b);
		if (b.y1 <= a.y1 && b.y2 >= a.y1)
			return true;
	}
	return false;
}

/* Each place shows the icon of the app whose folder it is */
TEST(places_icons)
{
	CHECK(ui_test_open("Files"));
	CHECK(app_icon_beside("Photos")); /* Gallery's */
	CHECK(app_icon_beside("Music"));
	CHECK(app_icon_beside("Memos"));  /* Recorder's */
	CHECK(!app_icon_beside("All files"));
	CHECK(ui_test_shows(LV_SYMBOL_DRIVE));
}

TEST(root)
{
	CHECK(ui_test_open("Files"));
	CHECK(ui_test_tap_text("All files"));
	CHECK(ui_test_shows("All files")); /* the title */
	CHECK(find_entry("var"));
	CHECK(ui_test_shows("usr"));
	CHECK(ui_test_tap_text("var"));
	CHECK(ui_test_shows("var"));  /* the title */
	CHECK(ui_test_shows("lib"));
	/* Back goes up, then to the places */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("usr"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("All files"));
	CHECK(!ui_test_shows("usr"));
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

/* Three-byte characters, the last one cut after two of them */
TEST(long_text_cut_inside_a_character)
{
	char text[3001] = "";
	for (int i = 0; i + 3 < 3000; i += 3)
		memcpy(text + i, "\xe2\x82\xac", 3); /* € */
	text[2997] = '\0';
	ui_test_write(DOCS "/euros", text);
	CHECK(open_folder("docs"));
	CHECK(ui_test_tap_text("euros"));
	const char *p = preview();
	CHECK(p != NULL);
	if (p) {
		CHECK_INT(strlen(p), 2046);
		CHECK(!memcmp(p + 2043, "\xe2\x82\xac", 3));
	}
}

TEST(empty_folder)
{
	mkdir(DOCS, 0755);
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("Empty folder"));
}

/* Pulled down, a folder is read again: what came meanwhile (over USB) shows */
TEST(pull_refresh)
{
	mkdir(DOCS, 0755);
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("Empty folder"));
	CHECK(ui_test_write(DOCS "/copied.txt", "Arrived.\n"));
	mkdir(DOCS "/sub", 0755);
	ui_test_pull_down();
	CHECK(ui_test_shows("copied.txt"));
	CHECK(ui_test_shows("sub"));
	CHECK(!ui_test_shows("Empty folder"));
	/* Its rows open as before */
	CHECK(ui_test_tap_text("copied.txt"));
	CHECK(ui_test_shows_part("Arrived."));
}

TEST(folder_not_readable)
{
	mkdir(DOCS, 0);
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("Cannot open this folder"));
	chmod(DOCS, 0755);
}

/* All of a long folder, its rows made for those in view only (a virtual list): the last
 * one there once scrolled to the end, and opened */
TEST(many_entries)
{
	char path[64];
	mkdir(DOCS, 0755);
	for (int i = 0; i < 2000; i++) {
		snprintf(path, sizeof(path), DOCS "/f%04d", i);
		ui_test_write(path, "");
	}
	CHECK(open_folder("docs"));
	CHECK(ui_test_shows("f0000"));
	CHECK(!ui_test_shows("f1999"));
	lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(ui_test_find("f0000")));
	CHECK(lv_obj_get_child_count(lv_obj_get_parent(row)) < 100);
	lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(row)), LV_COORD_MAX, LV_ANIM_OFF);
	ui_test_wait(100);
	CHECK(ui_test_shows("f1999"));
	CHECK(!ui_test_shows("f0000"));
	CHECK(ui_test_tap_text("f1999"));
	CHECK(ui_test_shows("Modified"));
}

/* A screen per folder: deeper than the screens the UI keeps, a folder does not open,
 * the one shown stays and goes back up */
TEST(folders_deeper_than_the_screens)
{
	char path[256] = DOCS, name[8];
	for (int i = 1; i <= 40; i++)
		snprintf(path + strlen(path), sizeof(path) - strlen(path), "/f%d", i);
	strcat(path, "/end");
	ui_test_write(path, ""); /* and the folders */
	CHECK(open_folder("docs"));
	int opened = 0;
	for (int i = 1; i <= 40; i++) {
		snprintf(name, sizeof(name), "f%d", i);
		lv_obj_t *before = ui_current();
		CHECK(ui_test_tap_text(name));
		if (ui_current() == before)
			break;
		opened = i;
	}
	CHECK(opened > 20 && opened < 40);
	snprintf(name, sizeof(name), "f%d", opened + 1);
	CHECK(ui_test_shows(name)); /* the folder it would not open, still listed */
	CHECK(ui_test_back());
	snprintf(name, sizeof(name), "f%d", opened);
	CHECK(ui_test_shows(name));
}

static int alert_ended = -1;

static void alert_done(enum ui_alert_end how, void *data)
{
	(void)data;
	alert_ended = how;
}

/* As deep as folders go, an alarm still shows (as a call would), then back to them */
TEST(alert_over_the_deepest_folder)
{
	char path[256] = DOCS, name[8];
	for (int i = 1; i <= 40; i++)
		snprintf(path + strlen(path), sizeof(path) - strlen(path), "/f%d", i);
	ui_test_write(path, "");
	CHECK(open_folder("docs"));
	for (int i = 1; i <= 40; i++) {
		snprintf(name, sizeof(name), "f%d", i);
		lv_obj_t *before = ui_current();
		if (!ui_test_tap_text(name) || ui_current() == before)
			break;
	}
	lv_obj_t *deepest = ui_current();
	ui_alert(LV_SYMBOL_BELL, "Clock", "Wake up", NULL, alert_done, NULL);
	ui_test_settle();
	CHECK(ui_test_shows("Wake up"));
	CHECK_INT(alert_ended, -1);
	CHECK(ui_test_tap_text("Stop"));
	CHECK_INT(alert_ended, UI_ALERT_STOP);
	CHECK(!ui_test_shows("Wake up"));
	CHECK(ui_current() == deepest);
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

/* In Romanian, switched to meanwhile: a file's details, an empty folder, a long one */
TEST(romanian)
{
	ui_language_set("ro", NULL);
	ui_test_settle();
	ui_test_write(DOCS "/a.txt", "Hello\n");
	struct timeval tv[2] = { { 1700000000, 0 }, { 1700000000, 0 } }; /* 14 November 2023 */
	utimes(DOCS "/a.txt", tv);
	mkdir(DOCS "/empty", 0755);
	CHECK(ui_test_open("Fișiere"));
	CHECK(ui_test_shows("Muzică")); /* the places' names */
	CHECK(ui_test_shows("Fotografii"));
	CHECK(ui_test_tap_text("Toate fișierele"));
	CHECK(tap_entry("var") && ui_test_tap_text("lib") && ui_test_tap_text("docs"));
	CHECK(ui_test_shows("6 B, 14 nov. 2023"));
	CHECK(ui_test_tap_text("a.txt"));
	CHECK(ui_test_shows("Dimensiune"));
	CHECK(ui_test_shows("Modificat"));
	CHECK(ui_test_shows("14 nov. 2023"));
	CHECK(ui_test_shows("Dosar"));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("empty"));
	CHECK(ui_test_shows("Dosar gol"));
}
