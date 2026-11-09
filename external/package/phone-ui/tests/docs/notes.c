/* docs/apps/notes.md's screenshots (included by test_docs.c) */

#include <sys/stat.h>
#include <sys/time.h>

#include "ui/rich_edit.h"
#include "ui_symbols.h"

/* A note saved at `when` (wall clock) */
static void notes_seed(const char *name, const char *text, time_t when)
{
	char path[256];
	snprintf(path, sizeof(path), "/var/lib/notes/%s", name);
	CHECK(ui_test_write(path, text));
	struct timeval tv[2] = { { when, 0 }, { when, 0 } };
	CHECK(utimes(path, tv) == 0);
}

static void notes_seed_all(void)
{
	notes_seed("20260302-181204.txt",
	           "Books to read\nThe Left Hand of Darkness, Piranesi, A Gentleman in Moscow\n",
	           NOW - 12 * 86400 + 8 * 3600);
	notes_seed("20260313-193020.txt",
	           "Bike repair\nNew chain, front brake pads. Ask about the wheel truing.\n",
	           NOW - 14 * 3600 - 11 * 60);
	notes_seed("20260314-081500.txt",
	           "Shopping\nOat milk, eggs, sourdough bread, lemons, coffee beans\n",
	           NOW - 86 * 60);
}

/* notes.md: the list, newest first */
TEST(notes_list)
{
	notes_seed_all();
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("Shopping"));
	CHECK(ui_test_shows("08:15")); /* today: the time */
	CHECK(ui_test_shows("Bike repair"));
	CHECK(ui_test_shows("Fri"));   /* this week: the day */
	CHECK(ui_test_shows("Books to read"));
	CHECK(ui_test_shows("2 Mar")); /* older: the date */
	lv_obj_t *newest = ui_test_find("Shopping"), *oldest = ui_test_find("Books to read");
	CHECK(newest && oldest);
	if (newest && oldest) {
		lv_area_t a, b;
		lv_obj_get_coords(newest, &a);
		lv_obj_get_coords(oldest, &b);
		CHECK(a.y1 < b.y1);
	}
	shot("notes-list");
}

/* notes.md: a new note being typed */
TEST(notes_new)
{
	notes_seed_all();
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text(LV_SYMBOL_PLUS));
	CHECK(ui_test_shows("New note"));
	CHECK(ui_test_textarea() != NULL); /* the keyboard is up */
	CHECK(ui_test_type("Call the dentist\nMonday morning, ask for a cleaning"));
	shot("notes-new");
}

/* notes.md: a note with formatting and lists, the toolbar over the keyboard */
TEST(notes_formatted)
{
	notes_seed_all();
	notes_seed("20260314-090000.md",
	           "Weekend\n**Saturday**: the market, then *the lake*\n- [x] Bread\n- [x] Cheese\n- [ ] Strawberries\n"
	           "1. Pack the car\n2. Leave by nine",
	           NOW - 41 * 60);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_tap_text("Weekend"));
	lv_obj_t *edit = ui_test_find_widget(&ui_rich_edit_class);
	CHECK(edit);
	if (!edit)
		return;
	ui_test_tap(edit); /* below the text: the cursor at its end, the keyboard up */
	CHECK(ui_test_shows(UI_SYMBOL_LIST_CHECK));
	CHECK_STR(ui_rich_edit_text(edit), "Weekend\nSaturday: the market, then the lake\nBread\nCheese\nStrawberries\n"
	                                   "Pack the car\nLeave by nine");
	shot("notes-formatted");
}
