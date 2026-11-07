/* Tasks (tasks.c), through the UI: phone-ui on the host (ui_test.h) */

#include <sys/stat.h>

#include "test.h"
#include "ui_test.h"

#define TASKS "/var/lib/tasks/tasks.json"

TEST_SETUP
{
	ui_test_reset();
}

static void tasks_write(const char *json)
{
	mkdir("/var/lib/tasks", 0755);
	FILE *f = fopen(TASKS, "w");
	fputs(json, f);
	fclose(f);
}

/* The saved file, "" if none */
static const char *tasks_saved(void)
{
	static char text[4096];
	FILE *f = fopen(TASKS, "r");
	text[0] = '\0';
	if (f) {
		text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
		fclose(f);
	}
	return text;
}

/* Where a text is on the screen, y1 -1 if it is not */
static int32_t y_of(const char *text)
{
	lv_obj_t *obj = ui_test_find(text);
	lv_area_t a = { -1, -1, -1, -1 };
	if (obj)
		lv_obj_get_coords(obj, &a);
	return a.y1;
}

/* How many tasks show as done: their check marks */
static unsigned checked(void)
{
	lv_obj_t *found[16];
	return ui_test_find_all(LV_SYMBOL_OK, found, 16);
}

/* How many saved tasks are not done */
static unsigned undone_saved(void)
{
	unsigned n = 0;
	for (const char *s = tasks_saved(); (s = strstr(s, "\"done\": 0")); s++)
		n++;
	return n;
}

TEST(empty)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Tasks"));
	CHECK(ui_test_shows("No tasks"));
}

TEST(added_with_enter_and_kept)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Buy milk\n"));
	CHECK(ui_test_shows("Buy milk"));
	CHECK(!ui_test_shows("No tasks"));
	CHECK(ui_test_font("Buy milk") == UI_FONT_TITLE);
	/* The field empty again, the keyboard still up for the next one */
	CHECK_STR(ui_test_typed(), "");
	CHECK(ui_test_type("Call the bank\n"));
	CHECK(ui_test_shows("Call the bank"));
	/* In the order added */
	CHECK(y_of("Buy milk") < y_of("Call the bank"));
	CHECK(checked() == 0);
	CHECK(strstr(tasks_saved(), "\"Buy milk\""));
	CHECK(strstr(tasks_saved(), "\"Call the bank\""));

	/* Opened again, they are there */
	CHECK(ui_test_back());
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Buy milk"));
	CHECK(ui_test_shows("Call the bank"));
}

TEST(blank_adds_nothing)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("   \n"));
	CHECK(ui_test_shows("No tasks"));
	CHECK_STR(tasks_saved(), "");
}

TEST(hiding_the_keyboard_keeps_the_text)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Water"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(!ui_test_textarea());
	CHECK(ui_test_shows("No tasks"));
	CHECK(ui_test_tap_field("Water"));
	CHECK_STR(ui_test_typed(), "Water");
}

TEST(listed_from_the_file)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 300}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Pay rent"));
	CHECK(ui_test_shows("Book flights"));
	CHECK(ui_test_shows("Done"));
	CHECK(checked() == 1);
	/* The done one below, under its heading */
	CHECK(y_of("Pay rent") < y_of("Done"));
	CHECK(y_of("Done") < y_of("Book flights"));
}

TEST(marked_done_and_undone)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 0},"
	            "{\"title\": \"Fix the bike\", \"created\": 300, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(!ui_test_shows("Done"));

	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_shows("Done"));
	CHECK(checked() == 1);
	CHECK(y_of("Book flights") < y_of("Done"));
	CHECK(y_of("Done") < y_of("Pay rent"));
	CHECK(undone_saved() == 2);

	/* The latest done first */
	CHECK(ui_test_tap_text("Fix the bike"));
	CHECK(checked() == 2);
	CHECK(y_of("Done") < y_of("Fix the bike"));
	CHECK(y_of("Fix the bike") < y_of("Pay rent"));

	/* Undone, back in its place among the others */
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(checked() == 1);
	CHECK(y_of("Pay rent") < y_of("Book flights"));
	CHECK(y_of("Book flights") < y_of("Done"));

	CHECK(ui_test_tap_text("Fix the bike"));
	CHECK(checked() == 0);
	CHECK(!ui_test_shows("Done"));
	CHECK(y_of("Book flights") < y_of("Fix the bike"));
	CHECK(undone_saved() == 3);
}
