/* Notes (notes.c), through the UI: phone-ui on the host (ui_test.h) */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#define NOTES "/var/lib/notes"

TEST_SETUP
{
	ui_test_start();
}

/* A note saved `age` seconds ago */
static void note_write(const char *name, const char *text, time_t age)
{
	char path[256];
	mkdir(NOTES, 0755);
	snprintf(path, sizeof(path), NOTES "/%s", name);
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
	struct timeval tv[2] = { { time(NULL) - age, 0 }, { time(NULL) - age, 0 } };
	utimes(path, tv);
}

/* The only note's file name and text, NULL if there is not exactly one */
static const char *only_note(char *name, size_t size)
{
	static char text[4096];
	DIR *dir = opendir(NOTES);
	struct dirent *d;
	int count = 0;
	while (dir && (d = readdir(dir)))
		if (d->d_name[0] != '.' && count++ == 0)
			snprintf(name, size, "%s", d->d_name);
	if (dir)
		closedir(dir);
	if (count != 1)
		return NULL;

	char path[512];
	snprintf(path, sizeof(path), NOTES "/%s", name);
	FILE *f = fopen(path, "r");
	text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
	fclose(f);
	return text;
}

static int32_t y_of(const char *text)
{
	lv_obj_t *obj = ui_test_find(text);
	lv_area_t a = { 0 };
	if (obj)
		lv_obj_get_coords(obj, &a);
	return obj ? a.y1 : -1;
}

/* Typing at the end of the text, wherever the tap put the cursor */
static void cursor_to_end(void)
{
	lv_obj_t *ta = ui_test_textarea();
	CHECK(ta);
	if (ta)
		lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
}

TEST(empty)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("Notes"));
	CHECK(ui_test_shows("No notes"));
}

TEST(new_note_saved_on_back)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("New note"));
	/* Starts with the keyboard */
	CHECK(ui_test_type("Shopping\nmilk, eggs. Bread"));
	CHECK_STR(ui_test_typed(), "Shopping\nmilk, eggs. Bread");

	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Shopping\nmilk, eggs. Bread");
	CHECK(strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".txt") == 0);

	/* Listed: the title, the rest as a preview on one line */
	CHECK(ui_test_shows("Notes"));
	CHECK(!ui_test_shows("No notes"));
	CHECK(ui_test_shows("Shopping"));
	CHECK(ui_test_shows("milk, eggs. Bread"));
}

TEST(saved_when_the_keyboard_is_hidden)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Draft"));
	char name[64];
	CHECK(!only_note(name, sizeof(name)));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(!ui_test_textarea());
	CHECK_STR(only_note(name, sizeof(name)), "Draft");
}

TEST(nothing_typed_nothing_saved)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("  \n "));
	CHECK(ui_test_back());
	char name[64];
	CHECK(!only_note(name, sizeof(name)));
	CHECK(ui_test_shows("No notes"));
}

TEST(listed_newest_first)
{
	note_write("a.txt", "Oldest\nfirst one", 3 * 86400);
	note_write("b.txt", "Newest\n\n  last   one  ", 60);
	note_write("c.txt", "Middle", 86400);
	note_write("ignored.md", "Not a note", 0);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("last one")); /* whitespace runs as one space */
	CHECK(!ui_test_shows("Not a note"));
	int32_t newest = y_of("Newest"), middle = y_of("Middle"), oldest = y_of("Oldest");
	CHECK(newest >= 0 && middle > newest && oldest > middle);
}

TEST(title_is_the_first_line_with_text)
{
	note_write("a.txt", "\n\n   \n  second line  \nthird", 0);
	note_write("b.txt", " \n\t\n", 60); /* written elsewhere: Notes deletes blank notes */
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("second line"));
	CHECK(ui_test_shows("third"));
	CHECK(ui_test_shows("Untitled"));
}

TEST(edit)
{
	note_write("a.txt", "Ideas", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Ideas"));
	CHECK(ui_test_shows("Note"));
	/* Opens to read: no keyboard until the text is tapped */
	CHECK(!ui_test_typed());
	CHECK(ui_test_tap_text("Ideas"));
	CHECK(ui_test_textarea());
	cursor_to_end();
	CHECK(ui_test_type("\nA phone"));
	CHECK(ui_test_back());

	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Ideas\nA phone");
	CHECK_STR(name, "a.txt");
	CHECK(ui_test_shows("Ideas"));
	CHECK(ui_test_shows("A phone"));
}

TEST(opened_unchanged_not_written)
{
	note_write("a.txt", "Keep", 3600);
	struct stat before, after;
	stat(NOTES "/a.txt", &before);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Keep"));
	CHECK(ui_test_back());
	stat(NOTES "/a.txt", &after);
	CHECK_INT(after.st_mtime, before.st_mtime);
}

TEST(delete)
{
	note_write("a.txt", "Old", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Old"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH "  Delete note"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete note"));
	char name[64];
	CHECK(!only_note(name, sizeof(name)));
	CHECK(ui_test_shows("Notes"));
	CHECK(ui_test_shows("No notes"));
}

TEST(delete_cancelled)
{
	note_write("a.txt", "Old", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Old"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH "  Delete note"));
	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Old");
}

TEST(emptied_note_deleted)
{
	note_write("a.txt", "x", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("x"));
	CHECK(ui_test_tap_text("x"));
	cursor_to_end();
	CHECK(ui_test_type("\b"));
	CHECK_STR(ui_test_typed(), "");
	CHECK(ui_test_back());
	char name[64];
	CHECK(!only_note(name, sizeof(name)));
	CHECK(ui_test_shows("No notes"));
}
