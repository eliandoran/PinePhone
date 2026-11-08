/*
 * gen-abi.py (GEN_ABI), which makes abi.h: UI_ABI, the fingerprint of each declaration of
 * the headers plugin apps are built against, and one of LVGL's headers together. Run on
 * headers written in /tmp: every line must end up in an entry named for its
 * declaration, a comment or an addition must not alter the others, a change only its own.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"

#define DIR "/tmp/abi"
#define MAX_ENTRIES 64

/* The last run's entries, "name:hash" */
static char *entries[MAX_ENTRIES];
static unsigned entry_count;

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		TEST_FAIL("cannot write %s", path);
		return;
	}
	fputs(text, f);
	fclose(f);
}

/* Runs the script on `header` (NULL: none) with DIR/lvgl as LVGL's include directory,
 * the entries of the abi.h it wrote in `entries`; false if it failed */
static bool run(const char *header)
{
	for (unsigned i = 0; i < entry_count; i++)
		free(entries[i]);
	entry_count = 0;
	mkdir(DIR, 0755);
	mkdir(DIR "/lvgl", 0755);
	char cmd[512];
	if (header)
		write_file(DIR "/ui.h", header);
	snprintf(cmd, sizeof(cmd), "python3 -I " GEN_ABI " " DIR "/abi.h " DIR "/lvgl%s",
	         header ? " " DIR "/ui.h" : "");
	if (system(cmd) != 0)
		return false;
	FILE *f = fopen(DIR "/abi.h", "r");
	if (!f)
		return false;
	char line[512];
	bool defined = false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "#define UI_ABI", 14) == 0)
			defined = true;
		/* Each entry a quoted string of its own, the literal concatenated */
		char *q = strchr(line, '"');
		if (!q)
			continue;
		char *end = strchr(q + 1, '"');
		if (!end || entry_count == MAX_ENTRIES)
			break;
		*end = 0;
		char *e = q + 1;
		if (e[strlen(e) - 1] == ' ')
			e[strlen(e) - 1] = 0;
		entries[entry_count++] = strdup(e);
	}
	fclose(f);
	return defined;
}

/* The hash of the entry named so, NULL if there is none */
static const char *entry(const char *name)
{
	size_t n = strlen(name);
	for (unsigned i = 0; i < entry_count; i++)
		if (strncmp(entries[i], name, n) == 0 && entries[i][n] == ':')
			return entries[i] + n + 1;
	return NULL;
}

/* The entries' names in order, space-separated (a static buffer) */
static const char *names(void)
{
	static char buf[4096];
	buf[0] = 0;
	for (unsigned i = 0; i < entry_count; i++) {
		char *colon = strchr(entries[i], ':');
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%.*s", i ? " " : "",
		         (int)(colon ? colon - entries[i] : (long)strlen(entries[i])), entries[i]);
	}
	return buf;
}

/* A copy of the last run's entries, for comparing two runs: kept until the test's process ends */
static char **volatile kept_copy; /* volatile: a store never read is not left out */
static char **keep(unsigned *count)
{
	char **copy = malloc(entry_count * sizeof(*copy));
	for (unsigned i = 0; i < entry_count; i++)
		copy[i] = strdup(entries[i]);
	*count = entry_count;
	kept_copy = copy;
	return copy;
}

/* The names of the kept entries that the last run does not have unchanged, space-separated */
static const char *differing(char **kept, unsigned count)
{
	static char buf[4096];
	buf[0] = 0;
	for (unsigned i = 0; i < count; i++) {
		bool same = false;
		for (unsigned j = 0; j < entry_count && !same; j++)
			same = strcmp(kept[i], entries[j]) == 0;
		if (!same)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%.*s", buf[0] ? " " : "",
			         (int)(strchr(kept[i], ':') - kept[i]), kept[i]);
	}
	return buf;
}

/* Every shape of declaration in ui.h, json.h and i18n.h */
#define FIXTURE                                                                               \
	"/* The fixture: a comment\n"                                                         \
	" * over two lines */\n"                                                              \
	"#ifndef FIXTURE_H\n"                                                                 \
	"#define FIXTURE_H\n"                                                                 \
	"\n"                                                                                  \
	"#include <stdint.h>\n"                                                               \
	"#include \"lvgl.h\"\n"                                                               \
	"\n"                                                                                  \
	"#define WIDTH 720 /* pixels */\n"                                                    \
	"#define MAX(a, b) ((a) > (b) ? \\\n"                                                 \
	"	(a) : (b))\n"                                                                 \
	"LV_FONT_DECLARE(ui_font_16)\n"                                                       \
	"\n"                                                                                  \
	"struct point {\n"                                                                    \
	"	int x; /* across */\n"                                                        \
	"	int y; // down\n"                                                             \
	"};\n"                                                                                \
	"enum kind { KIND_A, KIND_B };\n"                                                     \
	"typedef void (*callback)(int);\n"                                                    \
	"extern const struct point origin;\n"                                                 \
	"extern bool flag __attribute__((visibility(\"hidden\")));\n"                         \
	"int area(const struct point *a,\n"                                                   \
	"         const struct point *b);\n"                                                  \
	"static inline int twice(int n)\n"                                                    \
	"{\n"                                                                                 \
	"	return n * 2; /* } not a brace */\n"                                          \
	"}\n"                                                                                 \
	"#ifdef APP\n"                                                                        \
	"#define _(t) a(t)\n"                                                                 \
	"#else\n"                                                                             \
	"#define _(t) b(t)\n"                                                                 \
	"#endif\n"                                                                            \
	"#endif\n"

TEST(every_declaration_named)
{
	CHECK(run(FIXTURE));
	CHECK_STR(names(), "FIXTURE_H MAX WIDTH _ area callback enum.kind flag include.lvgl.h "
	                   "include.stdint.h lvgl origin struct.point twice ui_font_16");
	CHECK(entry("WIDTH") && strlen(entry("WIDTH")) == 8);
}

TEST(comments_and_spacing_do_not_count)
{
	CHECK(run(FIXTURE));
	unsigned count;
	char **kept = keep(&count);
	CHECK(run("/* Another comment */\n"
	          "#ifndef FIXTURE_H\n"
	          "#define FIXTURE_H\n"
	          "#include <stdint.h>\n"
	          "#include \"lvgl.h\"\n"
	          "#define WIDTH   720\n"
	          "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
	          "LV_FONT_DECLARE(ui_font_16)\n"
	          "struct point { int x; int y; }; /* one line */\n"
	          "enum kind {\n"
	          "	KIND_A, /* the first */\n"
	          "	KIND_B\n"
	          "};\n"
	          "typedef void (*callback)(int);\n"
	          "extern const struct point origin;\n"
	          "extern bool flag __attribute__((visibility(\"hidden\")));\n"
	          "int area(const struct point *a, const struct point *b);\n"
	          "static inline int twice(int n) { return n * 2; }\n"
	          "#ifdef APP\n"
	          "#define _(t) a(t)\n"
	          "#else\n"
	          "#define _(t) b(t)\n"
	          "#endif\n"
	          "#endif\n"));
	CHECK_STR(differing(kept, count), "");
	CHECK_INT(entry_count, count);
}

TEST(an_addition_keeps_the_rest)
{
	CHECK(run(FIXTURE));
	unsigned count;
	char **kept = keep(&count);
	CHECK(run("int perimeter(const struct point *p);\n" FIXTURE
	          "struct size {\n	int w, h;\n};\n"));
	CHECK_STR(differing(kept, count), "");
	CHECK_INT(entry_count, count + 2);
	CHECK(entry("perimeter"));
	CHECK(entry("struct.size"));
}

/* FIXTURE with `from` replaced by `to` */
static const char *edited(const char *from, const char *to)
{
	static char buf[sizeof(FIXTURE) + 256];
	const char *at = strstr(FIXTURE, from);
	if (!at)
		return NULL;
	snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(at - FIXTURE), FIXTURE, to, at + strlen(from));
	return buf;
}

TEST(a_change_alters_its_entry_only)
{
	CHECK(run(FIXTURE));
	unsigned count;
	char **kept = keep(&count);
	CHECK(run(edited("int y; // down", "int y, z;")));
	CHECK_STR(differing(kept, count), "struct.point");
	CHECK(run(edited("WIDTH 720", "WIDTH 800")));
	CHECK_STR(differing(kept, count), "WIDTH");
	CHECK(run(edited("const struct point *b);", "const struct point *b, int scale);")));
	CHECK_STR(differing(kept, count), "area");
	CHECK(run(edited("KIND_A, KIND_B", "KIND_B, KIND_A")));
	CHECK_STR(differing(kept, count), "enum.kind");
	CHECK(run(edited("#define _(t) b(t)", "#define _(t) c(t)")));
	CHECK_STR(differing(kept, count), "_");
	/* The same definition under another condition is another */
	CHECK(run(edited("#ifdef APP", "#ifndef APP")));
	CHECK_STR(differing(kept, count), "_");
	CHECK(run(edited("return n * 2;", "return n + n;")));
	CHECK_STR(differing(kept, count), "twice");
}

TEST(lvgl_headers_are_one_entry)
{
	mkdir(DIR, 0755);
	mkdir(DIR "/lvgl", 0755);
	mkdir(DIR "/lvgl/src", 0755);
	write_file(DIR "/lvgl/lvgl.h", "#include \"src/lv_api.h\"\n");
	write_file(DIR "/lvgl/src/lv_api.h", "void lv_init(void);\n");
	CHECK(run(FIXTURE));
	unsigned count;
	char **kept = keep(&count);
	char lvgl[16];
	snprintf(lvgl, sizeof(lvgl), "%s", entry("lvgl"));
	write_file(DIR "/lvgl/src/lv_api.h", "void lv_init(void);\nvoid lv_deinit(void);\n");
	CHECK(run(FIXTURE));
	CHECK_STR(differing(kept, count), "lvgl");
	CHECK(strcmp(entry("lvgl"), lvgl) != 0);
	/* A header added counts too: lv_conf.h decides the structs' layouts */
	write_file(DIR "/lvgl/lv_conf.h", "#define LV_USE_X 1\n");
	snprintf(lvgl, sizeof(lvgl), "%s", entry("lvgl"));
	CHECK(run(FIXTURE));
	CHECK(strcmp(entry("lvgl"), lvgl) != 0);
	remove(DIR "/lvgl/lv_conf.h");
	remove(DIR "/lvgl/src/lv_api.h");
	remove(DIR "/lvgl/src");
	remove(DIR "/lvgl/lvgl.h");
}

TEST(unfinished_declaration_refused)
{
	CHECK(!run("int area(const struct point *a,\n"));
	CHECK(!run("struct point {\n	int x;\n"));
	CHECK(!run("#ifdef APP\nint x;\n"));
}

/* Or what includes it would compile again each time */
TEST(written_only_when_changed)
{
	CHECK(run(FIXTURE));
	struct stat before, after;
	CHECK(stat(DIR "/abi.h", &before) == 0);
	CHECK(run(FIXTURE));
	CHECK(stat(DIR "/abi.h", &after) == 0);
	CHECK(after.st_mtim.tv_sec == before.st_mtim.tv_sec && after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
	CHECK(run(edited("WIDTH 720", "WIDTH 800")));
	CHECK(stat(DIR "/abi.h", &after) == 0);
	CHECK(after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec);
}

/* --exports' list for `header`: its names in order, space-separated (a static buffer), NULL
 * if the script failed */
static const char *exported(const char *header)
{
	static char buf[1024];
	buf[0] = 0;
	mkdir(DIR, 0755);
	mkdir(DIR "/lvgl", 0755);
	write_file(DIR "/ui.h", header);
	if (system("python3 -I " GEN_ABI " --exports " DIR "/exports.list " DIR "/abi.h " DIR "/lvgl "
	           DIR "/ui.h") != 0)
		return NULL;
	FILE *f = fopen(DIR "/exports.list", "r");
	if (!f)
		return NULL;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *semi = strchr(line, ';');
		if (line[0] == '\t' && semi)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%.*s", buf[0] ? " " : "",
			         (int)(semi - line - 1), line + 1);
	}
	fclose(f);
	return buf;
}

/* What phone-ui exports to plugins: the functions and variables, not types, macros or
 * inline functions; one returning a struct by its own name (the fingerprint's entry is the
 * struct's) */
TEST(exports_the_functions_and_variables)
{
	CHECK_STR(exported("struct point {\n	int x, y;\n};\n"
	                   "typedef int (*filter_fn)(int);\n"
	                   "#define WIDTH 720\n"
	                   "static inline int twice(int n)\n{\n	return n * 2;\n}\n"
	                   "int area(const struct point *p);\n"
	                   "struct point *point_new(void);\n"
	                   "extern unsigned point_count;\n"
	                   "extern const char *const point_names[];\n"
	                   "LV_FONT_DECLARE(ui_font_20)\n"),
	          "lv_* _lv_* area point_count point_names point_new ui_font_20");
}
