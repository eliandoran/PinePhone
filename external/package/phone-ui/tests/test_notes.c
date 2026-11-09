/* Notes (notes.c), through the UI: phone-ui on the host (ui_test.h) */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "test.h"
#include "i18n.h"
#include "ui_test.h"
#include "ui_symbols.h"
#include "ui/rich_edit.h"

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
	CHECK(ui_test_font("Shopping\nmilk, eggs. Bread") == UI_FONT_BODY_LARGE); /* read at length */

	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Shopping\nmilk, eggs. Bread");
	CHECK(strlen(name) > 3 && strcmp(name + strlen(name) - 3, ".md") == 0); /* Markdown */

	/* Listed: the title, the rest as a preview on one line */
	CHECK(ui_test_shows("Notes"));
	CHECK(!ui_test_shows("No notes"));
	CHECK(ui_test_shows("Shopping"));
	CHECK(ui_test_shows("milk, eggs. Bread"));
	CHECK(ui_test_font("milk, eggs. Bread") == UI_FONT_BODY);
}

TEST(saved_when_the_keyboard_is_hidden)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Draft"));
	char name[64];
	CHECK(!only_note(name, sizeof(name)));
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
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
	note_write("ignored.json", "Not a note", 0);
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
	CHECK_STR(name, "a.md"); /* saved as Markdown, the .txt gone */
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

/* A new note from the round button at the bottom right, in reach of the thumb */
TEST(new_note_button)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_action_button(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("New note"));
}

/* A first line longer than a card's title is cut between characters, not inside one */
TEST(long_title_cut_between_characters)
{
	char text[256] = "", title[256] = "";
	for (int i = 0; i < 60; i++)
		strcat(text, "\xc4\x83"); /* ă: two bytes, the cut at 95 falls in one */
	for (int i = 0; i < 47; i++)
		strcat(title, "\xc4\x83");
	note_write("long.txt", text, 60);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows(title));
}

/* What cannot be read, or is no file, is not listed */
TEST(unreadable_left_out)
{
	note_write("kept.txt", "Kept", 60);
	note_write("locked.txt", "Locked", 60);
	chmod(NOTES "/locked.txt", 0);
	mkdir(NOTES "/folder.txt", 0755);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("Kept"));
	CHECK(!ui_test_shows("Locked"));
	CHECK(!ui_test_shows("folder"));
	chmod(NOTES "/locked.txt", 0644);
}

/* Another note made in the same second: a name of its own, both kept */
TEST(same_second_names)
{
	time_t t = 1773481260; /* 09:41:00 */
	ui_test_wall_clock(t);
	for (int i = 0; i < 60; i++) {
		char name[64], stamp[32];
		time_t s = t + i;
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", gmtime(&s));
		snprintf(name, sizeof(name), "%s.txt", stamp); /* a .txt's name taken for a .md too */
		note_write(name, "Earlier", 3600);
	}
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Later"));
	CHECK(ui_test_back());
	DIR *dir = opendir(NOTES);
	struct dirent *d;
	int numbered = 0, count = 0;
	while (dir && (d = readdir(dir))) {
		if (d->d_name[0] == '.')
			continue;
		count++;
		numbered += strstr(d->d_name, "-2.md") != NULL;
	}
	if (dir)
		closedir(dir);
	CHECK_INT(count, 61);
	CHECK_INT(numbered, 1);
}

/* Cannot be saved (read-only): the text stays in the editor, nothing written */
TEST(not_saved)
{
	mkdir(NOTES, 0555);
	chmod(NOTES, 0555);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Lost"));
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	CHECK(ui_test_shows("Lost"));
	char name[64];
	CHECK(only_note(name, sizeof(name)) == NULL);
	chmod(NOTES, 0755);
}

/* Scrolled to the end, the last card is not under the button */
TEST(new_note_button_clear_of_the_last)
{
	for (int i = 0; i < 20; i++) {
		char name[32], text[32];
		snprintf(name, sizeof(name), "note%02d.txt", i);
		snprintf(text, sizeof(text), "Note %d", i);
		note_write(name, text, 3600 * (i + 1));
	}
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_clear_of_action_button("Note 19", LV_SYMBOL_PLUS));
}

/* The editor's text with its marks: a marked run as [biu:text], paragraphs on lines, list
 * items after "* ", "# " (numbered), "[ ] " or "[x] " (to do, done), as test_rich_text.c's */
static const char *marked(void)
{
	static char buf[1024];
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	if (!edit)
		return NULL;
	const struct rich_doc *doc = ui_rich_edit_doc(edit);
	size_t n = 0;
	buf[0] = '\0';
	for (uint32_t i = 0; i < doc->count; i++) {
		const struct rich_block *b = &doc->blocks[i];
		if (i)
			n += snprintf(buf + n, sizeof(buf) - n, "\n");
		if (b->type != RICH_PARAGRAPH)
			n += snprintf(buf + n, sizeof(buf) - n, "%s",
			              b->type == RICH_BULLET     ? "* "
			              : b->type == RICH_NUMBERED ? "# "
			              : b->checked               ? "[x] "
			                                         : "[ ] ");
		uint32_t at = 0;
		for (uint32_t r = 0; r < b->run_count; r++) {
			const struct rich_run *run = &b->runs[r];
			if (!run->marks)
				n += snprintf(buf + n, sizeof(buf) - n, "%.*s", (int)run->len, b->text + at);
			else
				n += snprintf(buf + n, sizeof(buf) - n, "[%s%s%s:%.*s]", run->marks & RICH_BOLD ? "b" : "",
				              run->marks & RICH_ITALIC ? "i" : "", run->marks & RICH_UNDERLINE ? "u" : "",
				              (int)run->len, b->text + at);
			at += run->len;
		}
	}
	return buf;
}

/* Whether the toolbar's button with that symbol is on */
static bool checked(const char *symbol)
{
	lv_obj_t *label = ui_test_find(symbol);
	CHECK(label);
	return label && lv_obj_has_state(lv_obj_get_parent(label), LV_STATE_CHECKED);
}

/* The toolbar's marks apply to what is typed next, until turned off */
TEST(bold_italic_underline_typed)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Plain "));
	CHECK(!checked(UI_SYMBOL_BOLD));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(checked(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("strong"));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(!checked(UI_SYMBOL_BOLD));
	CHECK(ui_test_tap_text(UI_SYMBOL_ITALIC));
	CHECK(ui_test_tap_text(UI_SYMBOL_UNDERLINE));
	CHECK(checked(UI_SYMBOL_ITALIC) && checked(UI_SYMBOL_UNDERLINE));
	CHECK(ui_test_type(" both"));
	CHECK_STR(marked(), "Plain [b:strong][iu: both]");
	CHECK_STR(ui_test_typed(), "Plain strong both");
	CHECK(ui_test_shows("Plain strong both"));
	CHECK(ui_test_font("Plain strong both") == UI_FONT_BODY_LARGE);
}

/* A new line goes on in the same marks; deleting takes the marks away with the text */
TEST(marks_go_on_after_enter)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("Title\nbody"));
	CHECK_STR(marked(), "[b:Title]\n[b:body]");
	CHECK(ui_test_type("\b\b\b\b\b"));
	CHECK_STR(marked(), "[b:Title]");
}

/* The cursor moved: the toolbar shows the marks there, and typing takes them */
TEST(cursor_moved_takes_the_marks_there)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("ab"));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("cd"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), 1);
	ui_test_wait(200);
	CHECK(checked(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("x"));
	CHECK_STR(marked(), "[b:axb]cd");
	cursor_to_end();
	ui_test_wait(200);
	CHECK(!checked(UI_SYMBOL_BOLD));
}

/* Tapping the text puts the cursor where the finger is */
TEST(tap_puts_the_cursor_there)
{
	note_write("a.txt", "Hello world\nsecond line", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Hello world"));
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	CHECK(edit);
	if (!edit)
		return;
	lv_area_t a;
	lv_obj_get_content_coords(edit, &a);
	int32_t line = lv_font_get_line_height(UI_FONT_BODY_LARGE);
	/* The first line's start, its end, the second line's end */
	ui_test_tap_at(a.x1 + 2, a.y1 + line / 2);
	CHECK(ui_test_textarea());
	CHECK_INT(lv_textarea_get_cursor_pos(ui_test_textarea()), 0);
	ui_test_tap_at(a.x2 - 10, a.y1 + line / 2);
	CHECK_INT(lv_textarea_get_cursor_pos(ui_test_textarea()), 11);
	ui_test_tap_at(a.x1 + 60, a.y1 + line * 3 / 2); /* in "second" */
	uint32_t pos = lv_textarea_get_cursor_pos(ui_test_textarea());
	CHECK(pos > 13 && pos < 18);
	/* Under the text: its end */
	ui_test_tap_at(a.x1 + 200, a.y1 + line * 5);
	CHECK_INT(lv_textarea_get_cursor_pos(ui_test_textarea()), 23);
	CHECK(ui_test_type("!"));
	CHECK_STR(ui_test_typed(), "Hello world\nsecond line!");
}

/* The toolbar comes with the keyboard */
TEST(toolbar_while_typing)
{
	note_write("a.txt", "Read me", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Read me"));
	CHECK(!ui_test_shows(UI_SYMBOL_BOLD));
	CHECK(ui_test_tap_text("Read me"));
	CHECK(ui_test_shows(UI_SYMBOL_BOLD));
	CHECK(ui_test_shows(UI_SYMBOL_ITALIC));
	CHECK(ui_test_shows(UI_SYMBOL_UNDERLINE));
	/* Right over the keys */
	lv_area_t bar, hide;
	lv_obj_get_coords(lv_obj_get_parent(lv_obj_get_parent(ui_test_find(UI_SYMBOL_BOLD))), &bar);
	lv_obj_get_coords(ui_test_find_widget(&lv_keyboard_class), &hide);
	CHECK(bar.y2 < hide.y1 + 2 && bar.y2 > hide.y1 - 10);
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	CHECK(!ui_test_shows(UI_SYMBOL_BOLD));
}

/* The list button makes the line a bullet list's item; Enter goes on with another */
TEST(bullet_list_typed)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Shopping\n"));
	CHECK(!checked(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK(checked(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("milk\neggs"));
	CHECK_STR(marked(), "Shopping\n* milk\n* eggs");
	CHECK_STR(ui_test_typed(), "Shopping\nmilk\neggs");
	/* Back on the title: no list there */
	lv_textarea_set_cursor_pos(ui_test_textarea(), 3);
	ui_test_wait(200);
	CHECK(!checked(UI_SYMBOL_LIST_UL));
	/* Tapped again on an item: a paragraph again */
	cursor_to_end();
	ui_test_wait(200);
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK_STR(marked(), "Shopping\n* milk\neggs");
	CHECK(!checked(UI_SYMBOL_LIST_UL));
}

/* Enter on an empty item ends the list, the line left as a paragraph */
TEST(enter_on_an_empty_item_ends_the_list)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("milk\n\n"));
	CHECK_STR(marked(), "* milk\n");
	CHECK_STR(ui_test_typed(), "milk\n");
	CHECK(!checked(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("Done"));
	CHECK_STR(marked(), "* milk\nDone");
}

/* Backspace at an item's start takes its bullet away, then joins it to the line before */
TEST(backspace_at_an_item_start)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("a\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("b"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), 2);
	ui_test_wait(200);
	CHECK(ui_test_type("\b"));
	CHECK_STR(marked(), "a\nb");
	CHECK_STR(ui_test_typed(), "a\nb");
	CHECK(ui_test_type("\b"));
	CHECK_STR(marked(), "ab");
}

/* An item's text starts right of its bullet, wrapped lines too; a tap on the bullet puts
 * the cursor at the text's start */
TEST(bullet_indents_the_item)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Title\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("one two three four five six seven eight nine ten eleven twelve"));
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	CHECK(edit);
	if (!edit)
		return;
	lv_area_t a;
	lv_obj_get_content_coords(edit, &a);
	int32_t line = lv_font_get_line_height(UI_FONT_BODY_LARGE);
	/* The bullet's place, left of the item */
	ui_test_tap_at(a.x1 + 4, a.y1 + line * 3 / 2);
	CHECK_INT(lv_textarea_get_cursor_pos(ui_test_textarea()), 6);
	/* Wrapped: the third line's start is the second's indent, not the edge */
	ui_test_tap_at(a.x1 + 4, a.y1 + line * 5 / 2);
	uint32_t wrapped = lv_textarea_get_cursor_pos(ui_test_textarea());
	CHECK(wrapped > 20);
	const char *t = ui_test_typed();
	CHECK(t && t[wrapped - 1] == ' '); /* the start of a word */
}

/* The numbered list button: items numbered from 1, Enter going on with the next */
TEST(numbered_list_typed)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Steps\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_OL));
	CHECK(checked(UI_SYMBOL_LIST_OL));
	CHECK(!checked(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("one\ntwo\nthree"));
	CHECK_STR(marked(), "Steps\n# one\n# two\n# three");
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	CHECK(edit);
	if (edit)
		CHECK_INT(rich_number(ui_rich_edit_doc(edit), 3), 3);
	/* Enter twice: the list ended, the next one starting at 1 again */
	CHECK(ui_test_type("\n\nNote\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_OL));
	CHECK(ui_test_type("again"));
	CHECK_STR(marked(), "Steps\n# one\n# two\n# three\nNote\n# again");
	if (edit)
		CHECK_INT(rich_number(ui_rich_edit_doc(edit), 5), 1);
}

/* A bullet list's item made a numbered one's, and back */
TEST(bullet_item_numbered)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_type("item"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_OL));
	CHECK_STR(marked(), "# item");
	CHECK(checked(UI_SYMBOL_LIST_OL) && !checked(UI_SYMBOL_LIST_UL));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_UL));
	CHECK_STR(marked(), "* item");
	/* Backspace at the start: a paragraph, as for bullets */
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_OL));
	lv_textarea_set_cursor_pos(ui_test_textarea(), 0);
	CHECK(ui_test_type("\n"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), 1);
	ui_test_wait(200);
	CHECK_STR(marked(), "# \n# item");
	CHECK(ui_test_type("\b"));
	CHECK_STR(marked(), "# \nitem");
}

/* Where a line's start is on the screen: the middle of its marker's place (a bullet, a
 * number, a checkbox) */
static lv_point_t marker_of_line(int line)
{
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	lv_area_t a = { 0 };
	CHECK(edit);
	if (edit)
		lv_obj_get_content_coords(edit, &a);
	int32_t h = lv_font_get_line_height(UI_FONT_BODY_LARGE);
	return (lv_point_t){ a.x1 + 24, a.y1 + line * h + h / 2 };
}

/* The to-do list button: items with a checkbox, Enter going on with another not done */
TEST(todo_list_typed)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Shopping\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_CHECK));
	CHECK(checked(UI_SYMBOL_LIST_CHECK));
	CHECK(ui_test_type("milk\neggs"));
	CHECK_STR(marked(), "Shopping\n[ ] milk\n[ ] eggs");
	/* Enter twice ends it, as other lists */
	CHECK(ui_test_type("\n\nDone"));
	CHECK_STR(marked(), "Shopping\n[ ] milk\n[ ] eggs\nDone");
}

/* Tapping a checkbox ticks the item, reading (no keyboard comes) or typing (the cursor
 * stays) */
TEST(todo_checkbox_tapped)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_CHECK));
	CHECK(ui_test_type("milk\neggs"));
	uint32_t cursor = lv_textarea_get_cursor_pos(ui_test_textarea());
	lv_point_t box = marker_of_line(0);
	ui_test_tap_at(box.x, box.y);
	CHECK_STR(marked(), "[x] milk\n[ ] eggs");
	CHECK(ui_test_textarea());
	CHECK_INT(lv_textarea_get_cursor_pos(ui_test_textarea()), cursor);

	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	box = marker_of_line(1);
	ui_test_tap_at(box.x, box.y);
	CHECK_STR(marked(), "[x] milk\n[x] eggs");
	CHECK(!ui_test_textarea());
	ui_test_tap_at(box.x, box.y);
	CHECK_STR(marked(), "[x] milk\n[ ] eggs");
	CHECK(!ui_test_textarea());
	/* The text past the checkbox is still a place for the cursor */
	ui_test_tap_at(box.x + 60, box.y);
	CHECK(ui_test_textarea());
}

/* After a done item, Enter makes one not done */
TEST(todo_new_item_not_done)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_CHECK));
	CHECK(ui_test_type("milk"));
	lv_point_t box = marker_of_line(0);
	ui_test_tap_at(box.x, box.y);
	CHECK(ui_test_type("\nbread"));
	CHECK_STR(marked(), "[x] milk\n[ ] bread");
}

/* The pixels left of a line's text, where its list marker is (not the cursor's place) */
static void marker_pixels(int line, uint32_t *px, int n)
{
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	lv_area_t a = { 0 };
	CHECK(edit);
	if (edit)
		lv_obj_get_content_coords(edit, &a);
	int32_t h = lv_font_get_line_height(UI_FONT_BODY_LARGE);
	for (int i = 0; i < n; i++)
		px[i] = lv_color_to_u32(ui_test_pixel(a.x1 + i % 44, a.y1 + line * h + i / 44));
}

/* An empty note's placeholder starts where the first line's text would: right of a list
 * item's marker, not over it */
TEST(placeholder_after_the_marker)
{
	static const char *const lists[] = { UI_SYMBOL_LIST_UL, UI_SYMBOL_LIST_OL, UI_SYMBOL_LIST_CHECK };
	for (size_t k = 0; k < sizeof(lists) / sizeof(lists[0]); k++) {
		CHECK(ui_test_open("Notes"));
		CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
		CHECK(ui_test_tap_text(lists[k]));
		enum { N = 44 * 29 };
		static uint32_t with[N], without[N];
		ui_test_wait(16);
		marker_pixels(0, with, N);
		CHECK(ui_test_type("x"));
		marker_pixels(0, without, N);
		CHECK(memcmp(with, without, sizeof(with)) == 0);
		CHECK(ui_test_type("\b"));
		ui_test_back();
		ui_test_back();
	}
}

/* Formatting is saved as Markdown, and listed without its markup */
TEST(formatting_saved_as_markdown)
{
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_type("Shopping\nfor "));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("Sunday"));
	CHECK(ui_test_tap_text(UI_SYMBOL_BOLD));
	CHECK(ui_test_type("\n"));
	CHECK(ui_test_tap_text(UI_SYMBOL_LIST_CHECK));
	CHECK(ui_test_type("milk\neggs"));
	lv_point_t box = marker_of_line(2);
	ui_test_tap_at(box.x, box.y);
	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Shopping\nfor **Sunday**\n- [x] milk\n- [ ] eggs");
	CHECK(ui_test_shows("Shopping"));
	CHECK(ui_test_shows("for Sunday milk eggs"));
}

/* A note in Markdown opens with its formatting */
TEST(markdown_note_opened)
{
	note_write("a.md", "Plan\n**bold** and *it* <u>under</u>\n- [x] done\n- [ ] open\n1. one\n2. two\n- dot", 60);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("bold and it under done open one two dot"));
	CHECK(ui_test_tap_text("Plan"));
	CHECK_STR(marked(), "Plan\n[b:bold] and [i:it] [u:under]\n[x] done\n[ ] open\n# one\n# two\n* dot");
	CHECK_STR(ui_rich_edit_text(ui_test_find_widget(&ui_rich_edit_class)),
	          "Plan\nbold and it under\ndone\nopen\none\ntwo\ndot");
}

/* Ticking a checkbox while reading is a change: saved */
TEST(checkbox_ticked_saved)
{
	note_write("a.md", "Shopping\n- [ ] milk", 60);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Shopping"));
	lv_point_t box = marker_of_line(1);
	ui_test_tap_at(box.x, box.y);
	CHECK(!ui_test_textarea());
	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Shopping\n- [x] milk");
}

/* Opened and left unchanged, a note is not written again: in Markdown, or in plain text
 * that would be written escaped */
TEST(unchanged_not_rewritten)
{
	note_write("a.md", "**Keep**\n- [x] this", 3600);
	note_write("b.txt", "5 * 3 = 15", 7200);
	struct stat before_a, before_b, after;
	stat(NOTES "/a.md", &before_a);
	stat(NOTES "/b.txt", &before_b);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Keep"));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("5 * 3 = 15"));
	CHECK(ui_test_back());
	CHECK(stat(NOTES "/a.md", &after) == 0);
	CHECK_INT(after.st_mtime, before_a.st_mtime);
	CHECK(stat(NOTES "/b.txt", &after) == 0);
	CHECK_INT(after.st_mtime, before_b.st_mtime);
}

/* Plain text from before Markdown, changed: saved as Markdown, what would read as it
 * escaped */
TEST(plain_note_saved_escaped)
{
	note_write("a.txt", "Maths\n5 * 3 = 15", 3600);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Maths"));
	CHECK(ui_test_tap_text("Maths\n5 * 3 = 15")); /* the editor: the keyboard up */
	cursor_to_end();
	CHECK(ui_test_type("!"));
	CHECK(ui_test_back());
	char name[64];
	CHECK_STR(only_note(name, sizeof(name)), "Maths\n5 \\* 3 = 15!");
	CHECK_STR(name, "a.md");
}
