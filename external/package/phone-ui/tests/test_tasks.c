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

/* Taps the circle before a task's title (the title opens the task) */
static bool tick(const char *title)
{
	lv_obj_t *label = ui_test_find(title);
	if (!label)
		return false;
	lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(label));
	ui_test_tap(lv_obj_get_child(row, 0));
	return true;
}

/* The dim line under a task's title (its due date), NULL if none */
static lv_obj_t *due_label(const char *title)
{
	lv_obj_t *label = ui_test_find(title);
	if (!label)
		return NULL;
	lv_obj_t *text = lv_obj_get_parent(label);
	return lv_obj_get_child_count(text) > 1 ? lv_obj_get_child(text, 1) : NULL;
}

static bool due_shows(const char *title, const char *due)
{
	lv_obj_t *label = due_label(title);
	return label && !lv_obj_is_hidden(label) && strcmp(lv_label_get_text(label), due) == 0;
}

static bool due_red(const char *title)
{
	lv_obj_t *label = due_label(title);
	return label && lv_color_eq(lv_obj_get_style_text_color(label, 0), lv_color_hex(UI_COLOR_RED));
}

#define NOW 1773481260 /* Saturday 14 March 2026, 09:41:00 UTC */

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

	CHECK(tick("Pay rent"));
	CHECK(ui_test_shows("Done"));
	CHECK(checked() == 1);
	CHECK(y_of("Book flights") < y_of("Done"));
	CHECK(y_of("Done") < y_of("Pay rent"));
	CHECK(undone_saved() == 2);

	/* The latest done first */
	CHECK(tick("Fix the bike"));
	CHECK(checked() == 2);
	CHECK(y_of("Done") < y_of("Fix the bike"));
	CHECK(y_of("Fix the bike") < y_of("Pay rent"));

	/* Undone, back in its place among the others */
	CHECK(tick("Pay rent"));
	CHECK(checked() == 1);
	CHECK(y_of("Pay rent") < y_of("Book flights"));
	CHECK(y_of("Book flights") < y_of("Done"));

	CHECK(tick("Fix the bike"));
	CHECK(checked() == 0);
	CHECK(!ui_test_shows("Done"));
	CHECK(y_of("Book flights") < y_of("Fix the bike"));
	CHECK(undone_saved() == 3);
}

TEST(opened_and_renamed)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_shows("Task"));
	CHECK(ui_test_shows("Due"));
	CHECK(ui_test_shows("None"));
	CHECK(!ui_test_shows("Time")); /* only once it has a date */
	CHECK(ui_test_tap_field("Pay rent"));
	CHECK(ui_test_type(" today"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Pay rent today"));
	CHECK(strstr(tasks_saved(), "\"Pay rent today\""));
	CHECK(checked() == 0); /* opening it did not tick it */
}

TEST(emptied_title_kept)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay"));
	CHECK(ui_test_tap_field("Pay"));
	CHECK(ui_test_type("\b\b\b"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Pay"));
	CHECK(strstr(tasks_saved(), "\"Pay\""));
}

TEST(deleted)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("Task"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Delete task"));
	CHECK(ui_test_shows("Tasks"));
	CHECK(!ui_test_shows("Pay rent"));
	CHECK(ui_test_shows("Book flights"));
	CHECK(!strstr(tasks_saved(), "Pay rent"));
}

TEST(due_tomorrow_then_a_time)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Due"));
	CHECK(ui_test_shows("Due date"));
	CHECK(ui_test_tap_text("Tomorrow"));
	/* Back on the task */
	CHECK(ui_test_shows("Task"));
	CHECK(ui_test_shows("Tomorrow"));
	CHECK(ui_test_shows("Time"));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-15\""));

	CHECK(ui_test_tap_text("Time"));
	CHECK(ui_test_tap_text("18:00"));
	CHECK(ui_test_shows("Task"));
	CHECK(ui_test_shows("18:00"));
	CHECK(strstr(tasks_saved(), "\"time\": \"18:00\""));

	CHECK(ui_test_back());
	CHECK(due_shows("Pay rent", "Tomorrow 18:00"));
	CHECK(!due_red("Pay rent"));
}

TEST(due_picked_in_the_month)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Due"));
	CHECK(ui_test_shows("March 2026"));
	CHECK(ui_test_tap_text("22")); /* the 27th is there twice: February's too */
	CHECK(ui_test_shows("Task"));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-22\""));
	CHECK(ui_test_back());
	CHECK(due_shows("Pay rent", "Sun 22 Mar"));
}

TEST(time_set_with_the_buttons)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-14\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(due_shows("Pay rent", "Today"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Time"));
	CHECK(ui_test_shows("12:00"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("12:30"));
	CHECK(ui_test_tap_text(LV_SYMBOL_MINUS));
	CHECK(ui_test_shows("12:15"));
	CHECK(ui_test_tap_text("Set"));
	CHECK(ui_test_shows("Task"));
	CHECK(strstr(tasks_saved(), "\"time\": \"12:15\""));
	CHECK(ui_test_back());
	CHECK(due_shows("Pay rent", "Today 12:15"));
}

TEST(due_cleared_with_its_time)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-20\", \"time\": \"09:00\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(due_shows("Pay rent", "Friday 09:00"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Due"));
	CHECK(ui_test_tap_text("None"));
	CHECK(ui_test_shows("Task"));
	CHECK(!ui_test_shows("Time"));
	CHECK(!strstr(tasks_saved(), "\"due\""));
	CHECK(!strstr(tasks_saved(), "\"time\""));
	CHECK(ui_test_back());
	CHECK(!due_label("Pay rent") || lv_obj_is_hidden(due_label("Pay rent")));
}

TEST(overdue_in_red)
{
	ui_test_wall_clock(NOW); /* 09:41 */
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, \"due\": \"2026-03-13\"},"
	            "{\"title\": \"Call mum\", \"created\": 200, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"09:00\"},"
	            "{\"title\": \"Lunch\", \"created\": 300, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"13:00\"},"
	            "{\"title\": \"Shop\", \"created\": 400, \"done\": 0, \"due\": \"2026-03-14\"},"
	            "{\"title\": \"Old trip\", \"created\": 500, \"done\": 600, \"due\": \"2026-03-01\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(due_shows("Pay rent", "Yesterday"));
	CHECK(due_red("Pay rent"));
	CHECK(due_shows("Call mum", "Today 09:00"));
	CHECK(due_red("Call mum"));
	CHECK(!due_red("Lunch"));
	CHECK(!due_red("Shop")); /* today, no time: not late yet */
	CHECK(!due_red("Old trip")); /* done */
	CHECK(due_shows("Old trip", "Sun 1 Mar"));
}

TEST(sorted_by_due_then_undated)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Undated\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Next week\", \"created\": 200, \"done\": 0, \"due\": \"2026-03-21\"},"
	            "{\"title\": \"Today late\", \"created\": 300, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"20:00\"},"
	            "{\"title\": \"Today\", \"created\": 400, \"done\": 0, \"due\": \"2026-03-14\"},"
	            "{\"title\": \"Today early\", \"created\": 500, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"10:00\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(y_of("Today") < y_of("Today early"));
	CHECK(y_of("Today early") < y_of("Today late"));
	CHECK(y_of("Today late") < y_of("Next week"));
	CHECK(y_of("Next week") < y_of("Undated"));
}

TEST(next_week_is_monday)
{
	ui_test_wall_clock(NOW); /* a Saturday */
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Due"));
	CHECK(ui_test_tap_text("Next week"));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-16\""));
}

/* The chips under the list choose the due date of the tasks added */
TEST(chips_only_while_typing)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(!ui_test_shows("No date"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_shows("No date"));
	CHECK(ui_test_shows("Today"));
	CHECK(ui_test_shows("Tomorrow"));
	CHECK(ui_test_shows("Pick\xe2\x80\xa6"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(!ui_test_shows("No date"));
}

TEST(added_for_tomorrow_until_changed)
{
	ui_test_wall_clock(NOW);
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_tap_text("Tomorrow"));
	CHECK(ui_test_textarea() != NULL); /* still typing */
	CHECK(ui_test_type("Pay rent\n"));
	CHECK(ui_test_type("Call mum\n"));
	CHECK(due_shows("Pay rent", "Tomorrow"));
	CHECK(due_shows("Call mum", "Tomorrow"));
	CHECK(ui_test_tap_text("No date"));
	CHECK(ui_test_type("Read\n"));
	CHECK(ui_test_shows("Read"));
	CHECK(!due_label("Read"));
	CHECK(y_of("Call mum") < y_of("Read"));

	/* Not kept for another time */
	CHECK(ui_test_tap_text("Today")); /* the chip, not a task's date */
	CHECK(ui_test_back());
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Later\n"));
	CHECK(!due_label("Later"));
}

TEST(added_for_a_day_picked)
{
	ui_test_wall_clock(NOW);
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_tap_text("Pick\xe2\x80\xa6"));
	CHECK(ui_test_shows("Due date"));
	CHECK(ui_test_tap_text("22"));
	CHECK(ui_test_shows("Tasks"));
	CHECK(ui_test_shows("Sun 22 Mar")); /* the chip */
	CHECK(!ui_test_shows("Pick\xe2\x80\xa6"));
	CHECK(ui_test_textarea() != NULL);
	CHECK(ui_test_type("Trip\n"));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-22\""));
	CHECK(due_shows("Trip", "Sun 22 Mar"));

	/* Tomorrow picked there is the Tomorrow chip */
	CHECK(ui_test_tap_text("Sun 22 Mar"));
	CHECK(ui_test_tap_text("15"));
	CHECK(ui_test_shows("Pick\xe2\x80\xa6"));
	CHECK(ui_test_type("Gym\n"));
	CHECK(due_shows("Gym", "Tomorrow"));
}
