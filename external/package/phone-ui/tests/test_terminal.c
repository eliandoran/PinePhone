/*
 * Terminal (apps/terminal), the plugin app: terminal.so built for the host
 * (TERMINAL_APP, the Makefile's), installed in /var/lib/apps/terminal and loaded by
 * phone-ui (external.c) as on the phone. Its shell is the script each test writes
 * (TERMINAL_SHELL): it prints what the test checks, escape sequences included, and
 * waits with `read` for the test to type Enter before its next step. Its output comes
 * in real time: the tests wait for texts (ui_test_wait_for()).
 */

#define _GNU_SOURCE /* RTLD_NEXT */
#include <dlfcn.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "ui_test.h"

#define SHELL "/tmp/terminal-shell" /* TERMINAL_SHELL of terminal.so */
#define APP "/var/lib/apps/terminal"
#define WAIT 5000
#define SETTINGS "/var/lib/phone-ui/settings"

/* terminal.so's forkpty(): this program's (linked -rdynamic, it comes first), failing when
 * a test says so, libc's otherwise */
static bool forkpty_fails;

pid_t forkpty(int *amaster, char *name, const struct termios *termp, const struct winsize *winp)
{
	if (forkpty_fails)
		return -1;
	pid_t (*real)(int *, char *, const struct termios *, const struct winsize *) =
		(pid_t (*)(int *, char *, const struct termios *, const struct winsize *))dlsym(RTLD_NEXT, "forkpty");
	return real ? real(amaster, name, termp, winp) : -1;
}

static bool copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
	char buf[65536];
	size_t n;
	bool ok = in && out;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
		ok = fwrite(buf, 1, n, out) == n;
	if (in)
		fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	return ok;
}

/* As `just app terminal` installs it */
TEST_SETUP
{
	ui_test_reset();
	if (!ui_test_write(APP "/app.json", "") || !copy(TERMINAL_APP "/app.json", APP "/app.json") ||
	    !copy(TERMINAL_APP "/terminal.so", APP "/terminal.so"))
		TEST_FAIL("cannot install %s", TERMINAL_APP);
}

/* The shell: `body` run by /bin/sh with the tests' PATH (the terminal's has the
 * phone's directories, empty on the host) */
static void shell(const char *body)
{
	const char *path = getenv("PATH");
	size_t size = strlen(body) + strlen(path) + 64;
	char *script = malloc(size);
	snprintf(script, size, "#!/bin/sh\nPATH='%s'\n%s\n", path, body);
	ui_test_write(SHELL, script);
	chmod(SHELL, 0755);
	free(script);
}

/* The terminal's view: the parent of a row showing `text` */
static lv_obj_t *view_of(const char *text)
{
	lv_obj_t *label = ui_test_find(text);
	return label ? lv_obj_get_parent(label) : NULL;
}

/* Row y of the grid when nothing went into the history yet (the cursor is child 0) */
static const char *row(lv_obj_t *view, int y)
{
	lv_obj_t *label = view ? lv_obj_get_child(view, 1 + y) : NULL;
	return label ? lv_label_get_text(label) : "(none)";
}

static lv_obj_t *cursor(lv_obj_t *view)
{
	return lv_obj_get_child(view, 0);
}

/* Enter, for the script's next `read` */
static bool enter(void)
{
	return ui_test_type("\n");
}

/* The cursor, the erasing, inserting and deleting, characters and character sets */
TEST(escape_sequences)
{
	shell("printf '\\033[2J\\033[H'\n"
	      "printf 'abcdef\\033[1;3HX'\n"
	      "printf '\\033[2;1Hhello world\\033[2;6H\\033[K'\n"
	      "printf '\\033[3;1H123456\\033[3;2H\\033[2P'\n"
	      "printf '\\033[4;1H123456\\033[4;2H\\033[2@'\n"
	      "printf '\\033[5;1Habc\\033[5;10Hz\\033[5;5H\\033[1K'\n"
	      "printf '\\033[6;1Hx\\033[3b'\n"
	      "printf '\\033[7;1Ha\\tb'\n"
	      "printf '\\033[8;1Hfoo\\rb'\n"
	      "printf '\\033[9;1Habc\\bX'\n"
	      "printf '\\033[10;1Hxyz\\033[10;2H\\033[1X'\n"
	      "printf '\\033[11;5Hgone\\033[2K'\n"
	      "printf '\\033[12;1H\\033(0lqk\\033(B lqk'\n"
	      "printf '\\033[13;1H\\303\\251\\342\\202\\254\\360\\237\\230\\200\\377'\n"
	      "printf '\\033[14;1Hab\\033[C\\033[CX\\033[D\\033[DY'\n"
	      "printf '\\033[15;1H\\033]0;a title\\007shown\\a'\n"
	      "printf '\\033[16;1H\\033)0\\016lqk\\017lqk'\n"
	      "printf '\\033[17;20H\\033[GA\\033[3`B\\033[18dC'\n"
	      "printf '\\033[19;1H\\033[?7lxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxyz\\033[?7h'\n"
	      "printf '\\033[20;1H01234567890123456789012345678901234567890123456789012345678'\n"
	      "printf '9\\033[22;1Hdone'\n"
	      "exec sleep 60");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("done", WAIT));
	lv_obj_t *v = view_of("done");
	CHECK_STR(row(v, 0), "abXdef");
	CHECK_STR(row(v, 1), "hello");
	CHECK_STR(row(v, 2), "1456");
	CHECK_STR(row(v, 3), "1  23456");
	CHECK_STR(row(v, 4), "         z");
	CHECK_STR(row(v, 5), "xxxx");
	CHECK_STR(row(v, 6), "a       b");
	CHECK_STR(row(v, 7), "boo");
	CHECK_STR(row(v, 8), "abX");
	CHECK_STR(row(v, 9), "x z");
	CHECK_STR(row(v, 10), "");
	CHECK_STR(row(v, 11), "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 lqk"); /* ┌─┐ */
	CHECK_STR(row(v, 12), "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\xef\xbf\xbd"); /* é€😀, then U+FFFD */
	CHECK_STR(row(v, 13), "ab YX");
	CHECK_STR(row(v, 14), "shown"); /* the title is not shown */
	CHECK_STR(row(v, 15), "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90lqk"); /* G1 shifted in, then G0 */
	CHECK_STR(row(v, 16), "A B");
	CHECK_STR(row(v, 17), "   C");
	CHECK_STR(row(v, 18), "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxz"); /* no wrap */
	CHECK_STR(row(v, 19), "01234567890123456789012345678901234567890123456789012345678");
	CHECK_STR(row(v, 20), "9");
	CHECK_STR(row(v, 21), "done");
	CHECK(!lv_obj_is_hidden(cursor(v)));
}

/* Scroll regions, lines inserted and deleted, the alternate screen, the cursor saved */
TEST(screens_and_regions)
{
	shell("printf '\\033[2J\\033[H1\\033[2;1H2\\033[3;1H3\\033[4;1H4\\033[5;1H5'\n"
	      "printf '\\033[2;4r\\033[2;1H\\033[L'\n" /* region rows 2 to 4: a blank line at 2 */
	      "printf '\\033[8;1Hstep 1'\n"
	      "read x\n"
	      "printf '\\033[2;4r\\033[3;1H\\033[M'\n" /* line 3 out of the region */
	      "printf '\\033[8;1H\\033[2Kstep 2'\n"
	      "read x\n"
	      "printf '\\033[2;4r\\033[S\\033[8;1H\\033[2Kstep 3'\n" /* the region up */
	      "read x\n"
	      "printf '\\033[2;4r\\033[T\\033[2;1H\\033M\\033[8;1H\\033[2Kstep 4'\n" /* down, then reverse index at its top */
	      "read x\n"
	      "printf '\\033[r\\033[2J\\033[10;1Hmain\\0337\\033[?1049h\\033[1;1Halt\\033[?25l'\n"
	      "read x\n"
	      "printf '\\033[?1049l\\0338X\\033[?25h\\033[s\\033[1;1H\\033[uY'\n"
	      "read x\n"
	      "printf '\\033[?47h\\033[1;1Halt 47\\033[?47l\\033[1;1H\\033E\\033D\\033Z'\n"
	      "printf '\\033[12;1Hend'\n"
	      "exec sleep 60");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("step 1", WAIT));
	lv_obj_t *v = view_of("step 1");
	CHECK_STR(row(v, 0), "1");
	CHECK_STR(row(v, 1), "");
	CHECK_STR(row(v, 2), "2");
	CHECK_STR(row(v, 3), "3"); /* 4 went out of the region */
	CHECK_STR(row(v, 4), "5");
	CHECK(enter());
	CHECK(ui_test_wait_for("step 2", WAIT));
	CHECK_STR(row(v, 1), "");
	CHECK_STR(row(v, 2), "3");
	CHECK_STR(row(v, 3), "");
	CHECK_STR(row(v, 4), "5");
	CHECK(enter());
	CHECK(ui_test_wait_for("step 3", WAIT));
	CHECK_STR(row(v, 1), "3");
	CHECK_STR(row(v, 2), "");
	CHECK_STR(row(v, 3), "");
	CHECK(enter());
	CHECK(ui_test_wait_for("step 4", WAIT));
	CHECK_STR(row(v, 1), ""); /* down twice: the region's top line two rows lower */
	CHECK_STR(row(v, 2), "");
	CHECK_STR(row(v, 3), "3");
	CHECK_STR(row(v, 4), "5"); /* outside the region */
	CHECK(enter());
	CHECK(ui_test_wait_for("alt", WAIT));
	CHECK(!ui_test_shows("main"));
	CHECK(lv_obj_is_hidden(cursor(v)));
	CHECK(enter());
	/* The main screen, the cursor where it was (X), then saved after it (CSI s) and back (CSI u) */
	CHECK(ui_test_wait_for("mainXY", WAIT));
	CHECK(!ui_test_shows("alt"));
	CHECK(!lv_obj_is_hidden(cursor(v)));
	CHECK(enter());
	CHECK(ui_test_wait_for("end", WAIT));
	CHECK(!ui_test_shows("alt 47"));
	CHECK(ui_test_shows("mainXY"));
}

/* What the terminal answers when asked: the cursor's position, its status, what it is */
TEST(replies)
{
	shell("stty raw -echo\n"
	      "printf '\\033[5;7H\\033[6n\\033[5n\\033[c\\033[>c\\033[=c'\n"
	      "r=$(dd bs=1 count=26 2>/dev/null)\n"
	      "stty sane\n"
	      "printf '\\033[2J\\033[H'\n"
	      "printf '%s\\n' \"$r\" | tr '\\033' E\n"
	      "exec sleep 60");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("E[5;7RE[0nE[?1;2cE[>0;0;0c", WAIT));
}

/* The keys above the keyboard and Ctrl: what the shell receives (bytes in hex) */
TEST(keys)
{
	shell("stty raw -echo\n"
	      "printf 'ready\\r\\n'\n"
	      "while :; do\n"
	      "  c=$(dd bs=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' \\n')\n"
	      "  printf '[%s]' \"$c\"\n"
	      "  [ \"$c\" = 61 ] && printf '\\033[?1h\\r\\napplication\\r\\n'\n" /* a: the arrows in application mode */
      "  [ \"$c\" = 0d ] && printf '\\r\\n'\n" /* Enter: on the next line */
	      "done");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("ready", WAIT));
	CHECK(ui_test_tap_button("Esc"));
	CHECK(ui_test_tap_button("Tab"));
	CHECK(ui_test_tap_button("|"));
	CHECK(ui_test_tap_button("/"));
	CHECK(ui_test_tap_button("-"));
	CHECK(ui_test_tap_button(LV_SYMBOL_LEFT));
	CHECK(ui_test_wait_for("[1b][09][7c][2f][2d][1b][5b][44]", WAIT));
	/* Ctrl, then a key: its control character */
	CHECK(ui_test_tap_button("Ctrl"));
	CHECK(ui_test_type("c"));
	CHECK(ui_test_tap_button("Ctrl"));
	CHECK(ui_test_type("?"));
	CHECK(ui_test_type("\n"));
	CHECK(ui_test_wait_for("[1b][09][7c][2f][2d][1b][5b][44][03][7f][0d]", WAIT));
	CHECK(ui_test_type("a"));
	CHECK(ui_test_wait_for("application", WAIT));
	CHECK(ui_test_tap_button(LV_SYMBOL_UP));
	CHECK(ui_test_wait_for("[1b][4f][41]", WAIT));
}

/* Lines scrolled off the top stay above, up to 1000; erasing them, resetting */
TEST(history)
{
	shell("i=1; while [ $i -le 1100 ]; do echo \"line $i\"; i=$((i+1)); done; echo end\n"
	      "read x\n"
	      "printf '\\033[3J'; echo cleared\n"
	      "read x\n"
	      "printf '\\033c'; echo reset\n"
	      "exec sleep 60");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("end", 10000));
	CHECK(ui_test_shows("line 1100"));
	CHECK(ui_test_shows("line 200"));
	CHECK(!ui_test_shows("line 1"));
	CHECK(!ui_test_shows("line 50"));
	/* Scrolled to the end: what was printed last is on the screen */
	lv_obj_t *end = ui_test_find("end");
	CHECK(end);
	if (end) {
		lv_area_t a, view;
		lv_obj_get_coords(end, &a);
		lv_obj_get_coords(lv_obj_get_parent(end), &view);
		CHECK(a.y1 >= view.y1 && a.y2 <= view.y2);
	}
	CHECK(enter());
	CHECK(ui_test_wait_for("cleared", WAIT));
	CHECK(!ui_test_shows("line 200"));
	CHECK(ui_test_shows("line 1100"));
	CHECK(enter());
	CHECK(ui_test_wait_for("reset", WAIT));
	CHECK(!ui_test_shows("line 1100"));
}

/* The size the shell is told, as the keyboard hides and shows */
static int rows_told(const char *text)
{
	int rows = 0, cols = 0;
	return sscanf(text, "%d %d", &rows, &cols) == 2 && cols == 59 ? rows : 0;
}

/* The last size the shell printed, 0 if none */
static int last_size(lv_obj_t *v)
{
	int rows = 0;
	for (uint32_t i = 1; v && i < lv_obj_get_child_count(v); i++) {
		int r = rows_told(lv_label_get_text(lv_obj_get_child(v, (int32_t)i)));
		if (r)
			rows = r;
	}
	return rows;
}

static lv_obj_t *size_view;
static int size_before;

static bool size_changed(void)
{
	int r = last_size(size_view);
	return r && r != size_before;
}

TEST(keyboard_resizes)
{
	shell("i=1; while [ $i -le 40 ]; do echo \"l$i\"; i=$((i+1)); done\n"
	      "trap 'stty size' WINCH\n"
	      "stty size\n"
	      "while :; do sleep 0.05; done");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("l40", WAIT));
	size_view = view_of("l40");
	CHECK(ui_test_wait_until(size_changed, WAIT));
	int with_keyboard = last_size(size_view);
	lv_obj_t *keys = ui_test_find_widget(&lv_buttonmatrix_class);
	CHECK(keys && !lv_obj_is_hidden(keys));
	/* Hidden: more rows, given back from the history */
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	size_before = with_keyboard;
	CHECK(ui_test_wait_until(size_changed, WAIT));
	int without = last_size(size_view);
	CHECK(without > with_keyboard + 10);
	CHECK(lv_obj_is_hidden(keys));
	CHECK(ui_test_shows("l1"));
	/* A tap on the terminal shows it again: fewer */
	lv_area_t a;
	lv_obj_get_coords(size_view, &a);
	ui_test_tap_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
	size_before = without;
	CHECK(ui_test_wait_until(size_changed, WAIT));
	CHECK_INT(last_size(size_view), with_keyboard);
	CHECK(!lv_obj_is_hidden(keys));
}

static lv_obj_t *terminal_screen;

static bool closed(void)
{
	return lv_screen_active() != terminal_screen;
}

/* The shell exiting closes the app */
TEST(shell_exits)
{
	shell("echo bye; read x; exit 0");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("bye", WAIT));
	terminal_screen = lv_screen_active();
	CHECK(enter());
	CHECK(ui_test_wait_until(closed, WAIT));
}

static pid_t shell_pid;

/* The shell's state in /proc ('S', 'R', 'Z'), 0 if it is gone */
static char state(pid_t pid)
{
	char path[64], buf[256];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char s = 0;
	if (fgets(buf, sizeof(buf), f)) {
		char *p = strrchr(buf, ')');
		s = p && p[1] ? p[2] : 0;
	}
	fclose(f);
	return s;
}

static bool shell_gone(void)
{
	return state(shell_pid) == 0;
}

/* Leaving ends the shell: SIGHUP, and SIGKILL 2 s later for one that ignores it */
TEST(leaving_ends_the_shell)
{
	shell("trap '' HUP\necho $$ > /tmp/shell.pid\necho ready\nwhile :; do sleep 0.05; done");
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("ready", WAIT));
	int pid = 0;
	FILE *f = fopen("/tmp/shell.pid", "r");
	if (f) {
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	CHECK(pid > 0);
	shell_pid = pid;
	CHECK(ui_test_back());
	ui_test_wait(1000);
	char s = state(pid);
	CHECK(s && s != 'Z'); /* still running: SIGHUP is ignored */
	ui_test_wait(1200);
	CHECK(ui_test_wait_until(shell_gone, WAIT));
}

/* No shell started: said in the terminal */
TEST(no_shell)
{
	forkpty_fails = true;
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_shows("No shell: see phone-ui's log"));
}

/* ...in the phone's language: its own catalog (apps/terminal/po) */
TEST(no_shell_romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	forkpty_fails = true;
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_shows("Niciun shell: vezi jurnalul phone-ui"));
}
