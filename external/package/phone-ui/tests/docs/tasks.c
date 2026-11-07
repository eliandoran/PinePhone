/* docs/apps/tasks.md's screenshots (included by test_docs.c) */

static void tasks_seed(void)
{
	CHECK(ui_test_write("/var/lib/tasks/tasks.json",
	                    "{\"tasks\": ["
	                    "{\"title\": \"Renew the bike insurance\", \"created\": 1773300000, \"done\": 0},"
	                    "{\"title\": \"Pick up the parcel from the post office\", \"created\": 1773310000, \"done\": 0},"
	                    "{\"title\": \"Buy a birthday present for Ana\", \"created\": 1773320000, \"done\": 0},"
	                    "{\"title\": \"Book the dentist\", \"created\": 1773200000, \"done\": 1773400000},"
	                    "{\"title\": \"Water the plants\", \"created\": 1773250000, \"done\": 1773470000}]}"));
}

/* tasks.md: the list, those to do then those done */
TEST(tasks_list)
{
	tasks_seed();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_shows("Renew the bike insurance"));
	CHECK(ui_test_shows("Done"));
	CHECK(ui_test_shows("Water the plants"));
	shot("tasks-list");
}

/* tasks.md: a task being typed */
TEST(tasks_new)
{
	tasks_seed();
	CHECK(ui_test_open("Tasks"));
	CHECK(ui_test_tap_field("New task"));
	CHECK(ui_test_textarea() != NULL); /* the keyboard is up */
	CHECK(ui_test_type("Call the garage"));
	shot("tasks-new");
}
