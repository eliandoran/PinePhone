/* Tasks (tasks.c), through the UI: phone-ui on the host (ui_test.h) */

#include <stdlib.h>
#include <sys/stat.h>

#include "test.h"
#include "data/data.h"
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

/* The heading of the section a task is listed in, "" if none */
static const char *section_of(const char *title)
{
	lv_obj_t *label = ui_test_find(title);
	if (!label)
		return "";
	lv_obj_t *row = lv_obj_get_parent(lv_obj_get_parent(label));
	lv_obj_t *card = lv_obj_get_parent(row);
	int32_t i = lv_obj_get_index(card);
	lv_obj_t *heading = i > 0 ? lv_obj_get_child(lv_obj_get_parent(card), i - 1) : NULL;
	return heading && lv_obj_check_type(heading, &lv_label_class) ? lv_label_get_text(heading) : "";
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
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
	CHECK(!ui_test_textarea());
	CHECK(ui_test_shows("No tasks"));
	CHECK(ui_test_tap_field("Water"));
	CHECK_STR(ui_test_typed(), "Water");
}

/* The count on the Done row at the end of the list, -1 if there is no such row */
static int done_count(void)
{
	lv_obj_t *label = ui_test_find("Done");
	if (!label)
		return -1;
	lv_obj_t *count = lv_obj_get_child(lv_obj_get_parent(label), 1);
	return count ? atoi(lv_label_get_text(count)) : -1;
}

TEST(listed_from_the_file)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 300}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Pay rent"));
	/* The done one behind a row at the end */
	CHECK(!ui_test_shows("Book flights"));
	CHECK(done_count() == 1);
	CHECK(y_of("Pay rent") < y_of("Done"));
	CHECK(checked() == 0);
}

TEST(no_done_row_without_done_tasks)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(done_count() == -1);
}

TEST(nothing_to_do)
{
	tasks_write("{\"tasks\": [{\"title\": \"Book flights\", \"created\": 200, \"done\": 300}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Nothing to do"));
	CHECK(done_count() == 1);
}

TEST(ticked_off_leaves_after_a_moment)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(tick("Pay rent"));
	CHECK(undone_saved() == 1);
	/* Still there, ticked, for a mistake to be seen */
	CHECK(ui_test_shows("Pay rent"));
	CHECK(checked() == 1);
	CHECK(done_count() == 1); /* counted at once */
	ui_test_wait(3500);
	CHECK(!ui_test_shows("Pay rent"));
	CHECK(ui_test_shows("Book flights"));
	CHECK(done_count() == 1);
}

TEST(ticked_by_mistake_ticked_back)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(tick("Pay rent"));
	CHECK(done_count() == 1);
	ui_test_wait(1000);
	CHECK(tick("Pay rent"));
	CHECK(checked() == 0);
	CHECK(done_count() == -1); /* the row gone with it */
	ui_test_wait(3500);
	CHECK(ui_test_shows("Pay rent"));
	CHECK(y_of("Pay rent") < y_of("Book flights")); /* in its place */
	CHECK(done_count() == -1);
	CHECK(undone_saved() == 2);
}

TEST(done_screen_latest_first)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 1772000000},"
	            "{\"title\": \"Fix the bike\", \"created\": 300, \"done\": 1773475920}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(done_count() == 2);
	CHECK(ui_test_tap_text("Done"));
	CHECK(ui_test_shows("Fix the bike"));
	CHECK(ui_test_shows("Book flights"));
	CHECK(!ui_test_shows("Pay rent"));
	CHECK(!ui_test_shows("New task"));
	CHECK(checked() == 2);
	CHECK(y_of("Fix the bike") < y_of("Book flights"));
	/* When each was done */
	CHECK(due_shows("Fix the bike", "08:12"));
	CHECK(due_shows("Book flights", "25 Feb"));

	/* Ticked back: to do again, after a moment */
	CHECK(tick("Book flights"));
	CHECK(checked() == 1);
	ui_test_wait(3500);
	CHECK(!ui_test_shows("Book flights"));
	CHECK(undone_saved() == 2);
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Book flights"));
	CHECK(done_count() == 1);
}

TEST(done_tasks_deleted)
{
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 300},"
	            "{\"title\": \"Fix the bike\", \"created\": 300, \"done\": 400}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Done"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("Fix the bike"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Delete done tasks"));
	/* Nothing left there: back to the list */
	CHECK(ui_test_shows("Tasks"));
	CHECK(ui_test_shows("Pay rent"));
	CHECK(done_count() == -1);
	CHECK(!strstr(tasks_saved(), "Book flights"));
	CHECK(!strstr(tasks_saved(), "Fix the bike"));
	CHECK(strstr(tasks_saved(), "Pay rent"));
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
	CHECK_STR(section_of("Pay rent"), "Tomorrow");
	CHECK(due_shows("Pay rent", "18:00")); /* the day is the heading */
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
	CHECK_STR(section_of("Pay rent"), "Today");
	CHECK(!due_label("Pay rent"));
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
	CHECK(due_shows("Pay rent", "12:15"));
}

TEST(due_cleared_with_its_time)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-20\", \"time\": \"09:00\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK_STR(section_of("Pay rent"), "Later");
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
	CHECK_STR(section_of("Pay rent"), "Overdue");
	CHECK_STR(section_of("Call mum"), "Overdue"); /* its time passed */
	CHECK(y_of("Pay rent") < y_of("Call mum"));
	CHECK_STR(section_of("Lunch"), "Today");
	CHECK(due_shows("Lunch", "13:00"));
	CHECK(!due_red("Lunch"));
	CHECK_STR(section_of("Shop"), "Today"); /* no time: not late yet */
	CHECK(y_of("Shop") < y_of("Lunch"));
	CHECK(lv_color_eq(lv_obj_get_style_text_color(ui_test_find("Overdue"), 0), lv_color_hex(UI_COLOR_RED)));
	CHECK(!ui_test_shows("Old trip")); /* done */
}

TEST(grouped_by_when_due)
{
	ui_test_wall_clock(NOW); /* Saturday 09:41 */
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Undated\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Far\", \"created\": 150, \"done\": 0, \"due\": \"2026-04-02\"},"
	            "{\"title\": \"Saturday next\", \"created\": 200, \"done\": 0, \"due\": \"2026-03-21\"},"
	            "{\"title\": \"Tonight\", \"created\": 300, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"20:00\"},"
	            "{\"title\": \"Any time today\", \"created\": 400, \"done\": 0, \"due\": \"2026-03-14\"},"
	            "{\"title\": \"Morning\", \"created\": 500, \"done\": 0, \"due\": \"2026-03-14\", \"time\": \"10:00\"},"
	            "{\"title\": \"Sunday\", \"created\": 600, \"done\": 0, \"due\": \"2026-03-15\", \"time\": \"08:00\"},"
	            "{\"title\": \"Last week\", \"created\": 700, \"done\": 0, \"due\": \"2026-03-07\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK_STR(section_of("Last week"), "Overdue");
	CHECK_STR(section_of("Any time today"), "Today");
	CHECK_STR(section_of("Morning"), "Today");
	CHECK_STR(section_of("Tonight"), "Today");
	CHECK_STR(section_of("Sunday"), "Tomorrow");
	CHECK_STR(section_of("Saturday next"), "Later");
	CHECK_STR(section_of("Far"), "Later");
	CHECK_STR(section_of("Undated"), "No date");
	CHECK(y_of("Last week") < y_of("Any time today"));
	CHECK(y_of("Any time today") < y_of("Morning"));
	CHECK(y_of("Morning") < y_of("Tonight"));
	CHECK(y_of("Tonight") < y_of("Sunday"));
	CHECK(y_of("Sunday") < y_of("Saturday next"));
	CHECK(y_of("Saturday next") < y_of("Far"));
	CHECK(y_of("Far") < y_of("Undated"));
	CHECK(due_shows("Sunday", "08:00"));
	CHECK(due_shows("Far", "Thu 2 Apr"));
}

/* Without dates, no headings */
TEST(undated_without_headings)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK_STR(section_of("Pay rent"), "");
	CHECK(!ui_test_shows("No date"));
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
	CHECK(ui_test_key(UI_TEST_KEY_HIDE));
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
	CHECK_STR(section_of("Pay rent"), "Tomorrow");
	CHECK_STR(section_of("Call mum"), "Tomorrow");
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
	CHECK_STR(section_of("Trip"), "Later");
	CHECK(due_shows("Trip", "Sun 22 Mar"));

	/* Tomorrow picked there is the Tomorrow chip */
	CHECK(ui_test_tap_text("Sun 22 Mar"));
	CHECK(ui_test_tap_text("15"));
	CHECK(ui_test_shows("Pick\xe2\x80\xa6"));
	CHECK(ui_test_type("Gym\n"));
	CHECK_STR(section_of("Gym"), "Tomorrow");
}

/* Reminders: a task with a date and a time notifies then (alarms.c) */

#define DUE_SOON (NOW + 18 * 60 + 50) /* 09:59:50, ten seconds before a task due at 10:00 */
#define DUE_AT_TEN "{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, " \
                   "\"due\": \"2026-03-14\", \"time\": \"10:00\"}]}"

static unsigned notified(void)
{
	return notification_app_count(ui_app_find("Tasks"));
}

/* A banner (on the top layer, at the top) showing `text` */
static lv_obj_t *banner(const char *text)
{
	lv_obj_t *found[8];
	unsigned n = ui_test_find_all(text, found, 8);
	for (unsigned i = 0; i < n && i < 8; i++) {
		lv_area_t a;
		lv_obj_get_coords(found[i], &a);
		if (lv_obj_get_screen(found[i]) == lv_layer_top() && a.y1 >= 0 && a.y2 < 300)
			return found[i];
	}
	return NULL;
}

static bool rent_banner(void)
{
	return banner("Pay rent") != NULL;
}

TEST(reminded_at_its_time)
{
	ui_test_wall_clock(DUE_SOON);
	tasks_write(DUE_AT_TEN);
	ui_test_start();
	ui_test_wait(5000);
	CHECK(!rent_banner());
	CHECK(notified() == 0);
	CHECK(ui_test_wait_until(rent_banner, 10000));
	CHECK(banner("Today 10:00"));
	CHECK(notified() == 1);
	CHECK(strstr(tasks_saved(), "\"reminded\": true"));
}

TEST(banner_opens_the_task)
{
	ui_test_wall_clock(DUE_SOON);
	tasks_write(DUE_AT_TEN);
	ui_test_start();
	CHECK(ui_test_wait_until(rent_banner, 15000));
	ui_test_settle(); /* slid in */
	lv_obj_t *b = banner("Pay rent");
	CHECK(b);
	if (b)
		ui_test_tap(b);
	CHECK(ui_test_wait_for("Task", 1000));
	CHECK(ui_test_shows("Due"));
	CHECK(notified() == 0); /* seen */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Tasks"));
}

TEST(missed_while_off_reminded_at_start)
{
	ui_test_wall_clock(NOW); /* 09:41 */
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-14\", \"time\": \"09:00\"}]}");
	ui_test_start();
	CHECK(ui_test_wait_until(rent_banner, 3000));
	CHECK(notified() == 1);
}

TEST(not_reminded_twice)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-14\", \"time\": \"09:00\", \"reminded\": true}]}");
	ui_test_start();
	ui_test_wait(3000);
	CHECK(!rent_banner());
	CHECK(notified() == 0);
}

TEST(no_reminder_without_a_time_or_done)
{
	ui_test_wall_clock(DUE_SOON);
	tasks_write("{\"tasks\": ["
	            "{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, \"due\": \"2026-03-14\"},"
	            "{\"title\": \"Call mum\", \"created\": 200, \"done\": 300, \"due\": \"2026-03-14\", \"time\": \"10:00\"}]}");
	ui_test_start();
	ui_test_wait(15000);
	CHECK(notified() == 0);
}

/* The phone woken for it while asleep (alarms.c's wake timer) */
TEST(wakes_the_phone_for_it)
{
	ui_test_wall_clock(NOW);
	tasks_write(DUE_AT_TEN);
	ui_test_start();
	CHECK(ui_test_wake_at() == NOW + 19 * 60);
	CHECK(ui_test_open("Tasks"));
	CHECK(tick("Pay rent"));
	CHECK(ui_test_wake_at() == 0); /* done: no reason to */
}

TEST(ticked_off_withdraws_it)
{
	ui_test_wall_clock(DUE_SOON);
	tasks_write(DUE_AT_TEN);
	ui_test_start();
	CHECK(ui_test_wait_until(rent_banner, 15000));
	ui_test_wait(6000); /* the banner gone */
	CHECK(!rent_banner());
	CHECK(notified() == 1);
	CHECK(ui_test_open("Tasks"));
	CHECK(tick("Pay rent"));
	CHECK(notified() == 0);
}

TEST(time_set_in_the_past_not_reminded)
{
	ui_test_wall_clock(NOW); /* 09:41 */
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, \"due\": \"2026-03-14\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Time"));
	CHECK(ui_test_tap_text("09:00"));
	ui_test_wait(3000);
	CHECK(notified() == 0);
}

TEST(new_time_reminds_again)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-14\", \"time\": \"09:00\", \"reminded\": true}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Time"));
	CHECK(ui_test_tap_text("18:00"));
	CHECK(ui_test_back());
	CHECK(ui_test_back());
	/* The clock set forward (NTP): due at once */
	ui_test_wall_clock(NOW + 9 * 3600);
	CHECK(ui_test_wait_until(rent_banner, 3000));
	CHECK(notified() == 1);
}

TEST(due_in_another_year)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Renew passport\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2027-01-05\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK_STR(section_of("Renew passport"), "Later");
	CHECK(due_shows("Renew passport", "Tue 5 Jan 2027"));
}

/* Not a list of tasks, or one without a title: left out */
TEST(file_without_tasks)
{
	tasks_write("{\"version\": 1}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("No tasks"));
}

TEST(task_without_a_title_left_out)
{
	tasks_write("{\"tasks\": [{\"title\": \"\", \"created\": 100, \"done\": 0},"
	            "{\"created\": 150, \"done\": 0},"
	            "{\"title\": \"Pay rent\", \"created\": 200, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Pay rent"));
	CHECK(checked() == 0);
	lv_obj_t *label = ui_test_find("Pay rent");
	CHECK(label && lv_obj_get_child_count(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(label)))) == 1);
}

/* An earlier version's tasks without ids get theirs after the others', then new ones */
TEST(ids_after_the_saved_ones)
{
	tasks_write("{\"tasks\": [{\"id\": 7, \"title\": \"Pay rent\", \"created\": 100, \"done\": 0},"
	            "{\"title\": \"Book flights\", \"created\": 200, \"done\": 0},"
	            "{\"id\": 3, \"title\": \"Fix the bike\", \"created\": 300, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Call mum\n"));
	const char *saved = tasks_saved();
	CHECK(strstr(saved, "\"id\": 7, \"title\": \"Pay rent\""));
	CHECK(strstr(saved, "\"id\": 8, \"title\": \"Book flights\""));
	CHECK(strstr(saved, "\"id\": 3, \"title\": \"Fix the bike\""));
	CHECK(strstr(saved, "\"id\": 9, \"title\": \"Call mum\""));
}

TEST(added_with_the_button)
{
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("  Buy milk "));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("Buy milk"));
	CHECK(strstr(tasks_saved(), "\"title\": \"Buy milk\""));
}

/* Where they are kept is not writable: in the list all the same, until Tasks is left */
TEST(not_saved)
{
	ui_test_write("/var/lib/tasks", "not a folder");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Buy milk\n"));
	CHECK(ui_test_shows("Buy milk"));
}

TEST(not_saved_over_a_folder)
{
	mkdir("/var/lib/tasks", 0755);
	mkdir(TASKS, 0755);
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_type("Buy milk\n"));
	CHECK(ui_test_shows("Buy milk"));
	CHECK(access(TASKS ".new", F_OK) != 0); /* not left behind */
}

TEST(due_today_picked)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Due"));
	CHECK(ui_test_tap_text("Today"));
	CHECK(ui_test_shows("Task"));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-14\""));
	CHECK(ui_test_back());
	CHECK_STR(section_of("Pay rent"), "Today");
}

TEST(time_cleared)
{
	ui_test_wall_clock(NOW);
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-20\", \"time\": \"09:00\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_text("Time"));
	CHECK(ui_test_tap_text("None"));
	CHECK(ui_test_shows("Task"));
	CHECK(!strstr(tasks_saved(), "\"time\""));
	CHECK(strstr(tasks_saved(), "\"due\": \"2026-03-20\""));
	CHECK(ui_test_back());
	CHECK(due_shows("Pay rent", "Friday"));
}

/* Enter in the title: done typing, kept at once */
TEST(renamed_with_enter)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_field("Pay rent"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), LV_TEXTAREA_CURSOR_LAST);
	CHECK(ui_test_type(" today\n"));
	CHECK(!ui_test_textarea());
	CHECK(ui_test_shows("Task"));
	CHECK(strstr(tasks_saved(), "\"Pay rent today\""));
}

/* Its time passes while the list is shown: overdue, without leaving it */
TEST(becomes_overdue_while_shown)
{
	ui_test_wall_clock(NOW + 50); /* 09:41:50 */
	tasks_write("{\"tasks\": [{\"title\": \"Call mum\", \"created\": 100, \"done\": 0, "
	            "\"due\": \"2026-03-14\", \"time\": \"09:42\"}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK_STR(section_of("Call mum"), "Today");
	ui_test_wait(15000);
	CHECK_STR(section_of("Call mum"), "Overdue");
	CHECK(due_red("Call mum"));
}

/* The last done task ticked back: none left there */
TEST(last_done_ticked_back)
{
	tasks_write("{\"tasks\": [{\"title\": \"Book flights\", \"created\": 200, \"done\": 300}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Done"));
	CHECK(tick("Book flights"));
	ui_test_wait(3500);
	CHECK(ui_test_shows("No done tasks"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Book flights"));
}

/* Left right after ticking back: to do all the same */
TEST(done_left_while_ticked_back)
{
	tasks_write("{\"tasks\": [{\"title\": \"Book flights\", \"created\": 200, \"done\": 300},"
	            "{\"title\": \"Fix the bike\", \"created\": 300, \"done\": 400}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Done"));
	CHECK(tick("Book flights"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Book flights"));
	CHECK(done_count() == 1);
	ui_test_wait(3500);
	CHECK(ui_test_shows("Book flights"));
}

/* Ticked off and left at once: done all the same */
TEST(left_right_after_ticking)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(tick("Pay rent"));
	CHECK(ui_test_back());
	ui_test_wait(3500);
	CHECK(undone_saved() == 0);
	CHECK(ui_test_open("Tasks"));
	CHECK(!ui_test_shows("Pay rent"));
	CHECK(done_count() == 1);
}

/* Spaces typed before the title are not kept */
TEST(renamed_trimmed)
{
	tasks_write("{\"tasks\": [{\"title\": \"Pay rent\", \"created\": 100, \"done\": 0}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Pay rent"));
	CHECK(ui_test_tap_field("Pay rent"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), 0);
	CHECK(ui_test_type("  "));
	CHECK(ui_test_back());
	CHECK(strstr(tasks_saved(), "\"title\": \"Pay rent\""));
}

/* Renamed from the Done screen: shown so there */
TEST(done_task_renamed)
{
	tasks_write("{\"tasks\": [{\"title\": \"Book flights\", \"created\": 200, \"done\": 300}]}");
	ui_test_start();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_text("Done"));
	CHECK(ui_test_tap_text("Book flights"));
	CHECK(ui_test_tap_field("Book flights"));
	lv_textarea_set_cursor_pos(ui_test_textarea(), LV_TEXTAREA_CURSOR_LAST);
	CHECK(ui_test_type(" home"));
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Book flights home"));
	CHECK(checked() == 1);
}
