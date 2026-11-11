/*
 * External apps (external.c): programs in /var/lib/apps/<id>/, found at startup, listed in
 * the menu, run with the display and the input handed over until they exit. The apps are
 * shell scripts that note what they were given in their $APP_DATA.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "ui.h"
#include "ui_test.h"

#define APPS "/var/lib/apps"
#define LOG "/var/lib/app-data/spinner/log"
#define CPU_MIN "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"

TEST_SETUP
{
	ui_test_reset();
}

/* `id` installed as `just app` does: app.json, and `script` as its program (run.sh) */
static void install(const char *id, const char *manifest, const char *script)
{
	char path[128];
	snprintf(path, sizeof(path), APPS "/%s/app.json", id);
	CHECK(ui_test_write(path, manifest));
	if (!script)
		return;
	snprintf(path, sizeof(path), APPS "/%s/run.sh", id);
	CHECK(ui_test_write(path, script));
	chmod(path, 0755);
}

/* A file's text, "" if none (static) */
static const char *read_file(const char *path)
{
	static char buf[4096];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

/* Its index in the apps, -1 if not there */
static int app_index(const char *name)
{
	for (unsigned i = 0; i < ui_app_count; i++)
		if (strcmp(ui_apps[i].name, name) == 0)
			return (int)i;
	return -1;
}

/* A process gone, or ended and not reaped yet */
static bool ended(const char *pid)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", atoi(pid));
	const char *stat = read_file(path);
	const char *state = strrchr(stat, ')');
	return !*stat || (state && state[2] == 'Z');
}

TEST(listed_by_name_after_the_built_in_apps)
{
	install("zebra", "{\"name\": \"Zebra\", \"exec\": \"run.sh\"}", "#!/bin/sh\n");
	install("abacus", "{\"name\": \"abacus\", \"exec\": \"run.sh\"}", "#!/bin/sh\n");
	/* Its folder's name without a name */
	install("lantern", "{}", "#!/bin/sh\n");
	ui_test_start();
	int abacus = app_index("abacus"), lantern = app_index("lantern"), zebra = app_index("Zebra");
	CHECK(abacus > app_index("Notes"));
	CHECK(abacus >= 0 && abacus < lantern && lantern < zebra);
	/* Settings stays last */
	CHECK_INT(app_index("Settings"), (int)ui_app_count - 1);
	CHECK(ui_test_open("abacus"));
}

TEST(broken_ones_left_out)
{
	install("good", "{\"name\": \"Good\"}", NULL);
	install("list", "[\"name\", \"List\"]", NULL);
	install("cut", "{\"name\": \"Cut", NULL);
	CHECK(ui_test_write(APPS "/bare/run.sh", "#!/bin/sh\n"));
	ui_test_start();
	CHECK(app_index("Good") >= 0);
	CHECK_INT(app_index("List"), -1);
	CHECK_INT(app_index("Cut"), -1);
	CHECK_INT(app_index("bare"), -1);
}

/* Each app's tile in the menu: its icon, or its symbol on its color */
TEST(icon_shown_instead_of_its_symbol)
{
	install("ruby", "{\"name\": \"Ruby\", \"symbol\": \"Q\"}", NULL);
	size_t size;
	void *png = ui_test_png(128, 128, 0xd01020, &size);
	FILE *f = fopen(APPS "/ruby/icon.png", "wb");
	CHECK(f && fwrite(png, 1, size, f) == size);
	if (f)
		fclose(f);
	/* An icon cut short, one that is not a PNG: their symbols */
	install("cut", "{\"name\": \"Cut\", \"symbol\": \"K\"}", NULL);
	f = fopen(APPS "/cut/icon.png", "wb");
	CHECK(f && fwrite(png, 1, size / 2, f) == size / 2);
	if (f)
		fclose(f);
	free(png);
	install("text", "{\"name\": \"Text\", \"symbol\": \"W\"}", NULL);
	CHECK(ui_test_write(APPS "/text/icon.png", "not a picture\n"));
	ui_test_start();
	ui_open_menu();
	ui_test_settle();

	CHECK(!ui_test_shows("Q"));
	CHECK(ui_test_shows("K"));
	CHECK(ui_test_shows("W"));
	lv_obj_t *label = ui_test_find("Ruby");
	CHECK(label);
	if (label) {
		lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
		ui_test_wait(50);
		CHECK(ui_test_shows_color(lv_obj_get_parent(label), 0xd01020));
	}
}

TEST(runs_in_its_folder_with_its_arguments)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\", \"args\": [\"--fast\", \"two words\"]}",
	        "#!/bin/sh\n"
	        "{\n"
	        "\tfor a; do echo \"arg $a\"; done\n"
	        "\techo \"in $(pwd)\"\n"
	        "\techo \"lib $LD_LIBRARY_PATH\"\n"
	        "\techo \"data $APP_DATA\"\n"
	        "\techo \"cpu $(cat " CPU_MIN ")\"\n"
	        "} > \"$APP_DATA/log\"\n");
	/* phone-ui's own library path is not the app's */
	setenv("LD_LIBRARY_PATH", "/usr/lib/elsewhere", 1);
	CHECK(ui_test_open("Spinner"));
	CHECK_STR(read_file(LOG), "arg --fast\narg two words\n"
	                          "in " APPS "/spinner\n"
	                          "lib " APPS "/spinner/lib\n"
	                          "data /var/lib/app-data/spinner\n"
	                          "cpu 648000\n");
	CHECK_INT(ui_test_display_handed_over(), 1);
	/* phone-ui has its input again */
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("Notes"));
}

TEST(a_game_runs_at_the_highest_frequency)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\", \"cpu\": \"full\"}",
	        "#!/bin/sh\necho \"cpu $(cat " CPU_MIN ")\" > \"$APP_DATA/log\"\n");
	CHECK(ui_test_open("Spinner"));
	CHECK_STR(read_file(LOG), "cpu 1152000\n");
	/* The interface's again after it */
	CHECK_STR(read_file(CPU_MIN), "648000\n");
}

TEST(what_it_started_ends_with_it)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\"}",
	        "#!/bin/sh\nsleep 60 &\necho $! > \"$APP_DATA/log\"\n");
	CHECK(ui_test_open("Spinner"));
	char pid[32];
	snprintf(pid, sizeof(pid), "%s", read_file(LOG));
	CHECK(atoi(pid) > 0);
	CHECK(ended(pid));
}

TEST(one_that_cannot_run_gives_the_display_back)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"missing\"}", NULL);
	CHECK(ui_test_open("Spinner"));
	CHECK_INT(ui_test_display_handed_over(), 1);
	CHECK(ui_test_open("Notes"));
	CHECK(ui_test_shows("Notes"));
}

/* Folders an app declares ("folders", under /var/lib): made, shared over MTP (the list the
 * board's USB mode script reads) and in Files, after the built-in apps'; the name the
 * app's unless given. Paths out of /var/lib, or that umtprd.conf could not quote, refused.
 * Last over MTP, read-only: phone-ui's logs (S90phone-ui's), not in Files */
TEST(folders)
{
	install("emu", "{\"name\": \"Emu\", \"folders\": [{\"path\": \"emu/roms\", \"name\": \"Emu ROMs\"},"
	        " {\"path\": \"emu-saves\"}, {\"path\": \"../etc\"}, {\"path\": \"/etc\"},"
	        " {\"path\": \"emu/../../etc\"}, {\"path\": \"a\\\"b\"}, {\"path\": \"\"}, {}]}", NULL);
	ui_test_start();
	struct stat st;
	CHECK(stat("/var/lib/emu/roms", &st) == 0 && S_ISDIR(st.st_mode));
	CHECK(stat("/var/lib/emu-saves", &st) == 0 && S_ISDIR(st.st_mode));
	CHECK_STR(read_file("/tmp/folders"),
	          "storage \"/var/lib/photos\" \"Photos\" \"rw\"\n"
	          "storage \"/var/lib/music\" \"Music\" \"rw\"\n"
	          "storage \"/var/lib/videos\" \"Videos\" \"rw\"\n"
	          "storage \"/var/lib/books\" \"Books\" \"rw\"\n"
	          "storage \"/var/lib/notes\" \"Notes\" \"rw\"\n"
	          "storage \"/var/lib/memos\" \"Memos\" \"rw\"\n"
	          "storage \"/var/lib/emu/roms\" \"Emu ROMs\" \"rw\"\n"
	          "storage \"/var/lib/emu-saves\" \"Emu\" \"rw\"\n"
	          "storage \"/var/lib/phone-ui/log\" \"Logs\" \"ro\"\n");
	CHECK(stat("/var/lib/phone-ui/log", &st) == 0 && S_ISDIR(st.st_mode));
	CHECK(ui_test_open("Files"));
	CHECK(!ui_test_shows("Logs"));
	CHECK(ui_test_shows("Emu ROMs"));
	CHECK(ui_test_shows("/var/lib/emu-saves"));
	CHECK(ui_test_tap_text("Emu ROMs"));
	CHECK(ui_test_shows("Empty folder"));
}

/* Its manifest gone since it was found: nothing runs */
TEST(removed_since_found)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\"}", "#!/bin/sh\n");
	ui_test_start();
	CHECK(remove(APPS "/spinner/app.json") == 0);
	CHECK(ui_test_open("Spinner"));
	CHECK_INT(ui_test_display_handed_over(), 0);
}

/* The app notes it started, then that it was asked to end (SIGTERM, to the whole group) */
#define ENDS_ON_TERM \
	"#!/bin/sh\n" \
	"trap 'echo ended >> \"$APP_DATA/log\"; exit 0' TERM\n" \
	"echo started > \"$APP_DATA/log\"\n" \
	"while :; do sleep 0.01; done\n"

TEST(power_held_ends_it)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\"}", ENDS_ON_TERM);
	ui_test_start();
	ui_test_app_power_hold(LOG, 2500);
	ui_test_touchscreen(); /* another input device, without Power */
	CHECK(ui_test_open("Spinner"));
	CHECK_STR(read_file(LOG), "started\nended\n");
	CHECK_INT(ui_test_display_handed_over(), 1);
}

TEST(power_pressed_briefly_leaves_it)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\"}",
	        "#!/bin/sh\n"
	        "trap 'echo ended >> \"$APP_DATA/log\"; exit 0' TERM\n"
	        "echo started > \"$APP_DATA/log\"\n"
	        "sleep 0.5\n");
	ui_test_start();
	ui_test_app_power_hold(LOG, 1000);
	CHECK(ui_test_open("Spinner"));
	CHECK_STR(read_file(LOG), "started\n");
}

/* One that ignores SIGTERM is killed 3 s later */
TEST(power_held_on_kills_it)
{
	install("spinner", "{\"name\": \"Spinner\", \"exec\": \"run.sh\"}",
	        "#!/bin/sh\n"
	        "trap '' TERM\n"
	        "echo $$ > \"$APP_DATA/log\"\n"
	        "while :; do sleep 0.01; done\n");
	ui_test_start();
	ui_test_app_power_hold(LOG, 5500);
	CHECK(ui_test_open("Spinner"));
	char pid[32];
	snprintf(pid, sizeof(pid), "%s", read_file(LOG));
	CHECK(atoi(pid) > 0);
	CHECK(ended(pid));
	CHECK(ui_test_open("Notes"));
}
