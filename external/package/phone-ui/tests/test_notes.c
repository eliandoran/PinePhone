/* Notes (notes.c), through the UI: phone-ui on the host (ui_test.h) */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "test.h"
#include "i18n.h"
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

/* Where a text is on the screen, x1 -1 if it is not */
static lv_area_t area_of(const char *text)
{
	lv_obj_t *obj = ui_test_find(text);
	lv_area_t a = { -1, -1, -1, -1 };
	if (obj)
		lv_obj_get_coords(obj, &a);
	return a;
}

/* Whether a comes before b in reading order: above it, or left of it on its line */
static bool before(lv_area_t a, lv_area_t b)
{
	return a.x1 >= 0 && b.x1 >= 0 && (a.y1 < b.y1 || (a.y1 == b.y1 && a.x1 < b.x1));
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
	CHECK(before(area_of("Newest"), area_of("Middle")));
	CHECK(before(area_of("Middle"), area_of("Oldest")));
	/* When each was changed, under its text */
	char when[32];
	ui_when_format(when, sizeof(when), time(NULL) - 60);
	CHECK(ui_test_font(when) == UI_FONT_CAPTION);
}

TEST(grid_of_two_columns)
{
	note_write("a.txt", "First", 60);
	note_write("b.txt", "Second", 120);
	note_write("c.txt", "Third", 180);
	CHECK(ui_test_open("Notes"));
	lv_area_t first = area_of("First"), second = area_of("Second"), third = area_of("Third");
	/* Side by side on the 720 px display, the third below the first */
	CHECK_INT(second.y1, first.y1);
	CHECK(first.x1 >= 0 && first.x1 < 360 && second.x1 > 360);
	CHECK_INT(third.x1, first.x1);
	CHECK(third.y1 > first.y1);
	CHECK(ui_test_tap_text("Second"));
	CHECK(ui_test_shows("Note"));
	CHECK(ui_test_shows("Second"));
}

TEST(card_shows_several_lines)
{
	const char *rest = "milk eggs bread butter cheese apples pears flour sugar coffee tea";
	char text[128];
	snprintf(text, sizeof(text), "Shopping\n%s", rest);
	note_write("a.txt", text, 60);
	CHECK(ui_test_open("Notes"));
	lv_obj_t *preview = ui_test_find(rest);
	CHECK(preview);
	if (!preview)
		return;
	/* Wrapped in its card, not cut after one line */
	lv_area_t a;
	lv_obj_get_coords(preview, &a);
	CHECK(lv_area_get_width(&a) < 360);
	CHECK(lv_area_get_height(&a) > 2 * lv_font_get_line_height(lv_obj_get_style_text_font(preview, 0)));
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

/* In Romanian, switched to meanwhile: the empty list, a new note, one without a title */
TEST(romanian)
{
	ui_language_set("ro", NULL);
	ui_test_settle();
	CHECK(ui_test_open("Notițe"));
	CHECK(ui_test_shows("Nicio notiță"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("Notiță nouă"));
	ui_test_back();
	ui_test_back();
	note_write("1.txt", "\n\n", 60);
	CHECK(ui_test_open("Notițe"));
	CHECK(ui_test_tap_text("Fără titlu"));
	CHECK(ui_test_shows("Notiță"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_shows(LV_SYMBOL_TRASH "  Șterge notița"));
}
