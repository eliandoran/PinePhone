/*
 * Syntax highlighting of a diff's code (files.c), cheap: comments, strings, numbers
 * and keywords, by the file's extension, as LVGL's label recolor marks
 * ("#rrggbb text#", "##" a #). A line at a time, its first character the diff's
 * (+, -, space) left as is, a hunk's header line too; a block comment or a string
 * over lines goes on to the next line, until a hunk's header (what came before it
 * is not in the patch). A # cannot be colored: it would end the color, so it is
 * drawn in the text's. GitHub's colors, the dark theme's or the light one's.
 */

#define _GNU_SOURCE /* strchrnul */
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "github.h"

enum kind { PLAIN, KEYWORD, STRING, NUMBER, COMMENT };

/* A comment or string over lines */
struct block {
	const char *open, *close;
	enum kind kind;
};

struct lang {
	const char *const *exts; /* "c", "h"…; NULL-terminated */
	const char *line_comment[2]; /* "//", "#" */
	struct block blocks[3];
	const char *quotes; /* string delimiters within a line */
	const char *const *keywords; /* NULL-terminated */
};

/* NON-NLS-BEGIN: languages */
static const char *const c_exts[] = {"c", "h", "cc", "cpp", "cxx", "hh", "hpp", "hxx", "m", "mm", NULL};
static const char *const c_keywords[] = {
	"auto", "bool", "break", "case", "char", "class", "const", "constexpr", "continue", "default",
	"delete", "do", "double", "else", "enum", "extern", "false", "float", "for", "goto", "if",
	"inline", "int", "long", "namespace", "new", "nullptr", "private", "protected", "public",
	"register", "return", "short", "signed", "sizeof", "static", "struct", "switch", "template",
	"this", "true", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
	"volatile", "while", "NULL", NULL};
static const char *const js_exts[] = {"js", "mjs", "cjs", "jsx", "ts", "tsx", "mts", NULL};
static const char *const js_keywords[] = {
	"as", "async", "await", "break", "case", "catch", "class", "const", "continue", "default",
	"delete", "do", "else", "enum", "export", "extends", "false", "finally", "for", "from",
	"function", "if", "implements", "import", "in", "instanceof", "interface", "let", "new", "null",
	"of", "private", "protected", "public", "readonly", "return", "static", "super", "switch",
	"this", "throw", "true", "try", "type", "typeof", "undefined", "var", "void", "while", "yield",
	NULL};
static const char *const py_exts[] = {"py", "pyi", NULL};
static const char *const py_keywords[] = {
	"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue",
	"def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import",
	"in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while",
	"with", "yield", NULL};
static const char *const sh_exts[] = {"sh", "bash", "zsh", NULL};
static const char *const sh_keywords[] = {
	"case", "do", "done", "elif", "else", "esac", "export", "fi", "for", "function", "if", "in",
	"local", "return", "then", "until", "while", NULL};
static const char *const rs_exts[] = {"rs", NULL};
static const char *const rs_keywords[] = {
	"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "false",
	"fn", "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref",
	"return", "self", "Self", "static", "struct", "super", "trait", "true", "type", "unsafe", "use",
	"where", "while", NULL};
static const char *const go_exts[] = {"go", NULL};
static const char *const go_keywords[] = {
	"break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough",
	"false", "for", "func", "go", "goto", "if", "import", "interface", "map", "nil", "package",
	"range", "return", "select", "struct", "switch", "true", "type", "var", NULL};
static const char *const nix_exts[] = {"nix", NULL};
static const char *const nix_keywords[] = {
	"assert", "else", "false", "if", "in", "inherit", "let", "null", "or", "rec", "then", "true",
	"with", NULL};
/* Comments and strings only */
static const char *const slash_exts[] = {"java", "kt", "kts", "cs", "swift", "scala", "dart", "css",
					 "scss", "less", "json", "jsonc", "proto", "glsl", NULL};
static const char *const hash_exts[] = {"rb", "pl", "pm", "yml", "yaml", "toml", "cmake", "mk",
					"conf", "ini", "cfg", "mak", NULL};
static const char *const dash_exts[] = {"lua", "sql", "hs", NULL};
static const char *const none[] = {NULL};

static const struct lang langs[] = {
	{c_exts, {"//"}, {{"/*", "*/", COMMENT}}, "\"'", c_keywords},
	{js_exts, {"//"}, {{"/*", "*/", COMMENT}, {"`", "`", STRING}}, "\"'", js_keywords},
	{py_exts, {"#"}, {{"\"\"\"", "\"\"\"", STRING}, {"'''", "'''", STRING}}, "\"'", py_keywords},
	{sh_exts, {"#"}, {{0}}, "\"'", sh_keywords},
	{rs_exts, {"//"}, {{"/*", "*/", COMMENT}}, "\"", rs_keywords},
	{go_exts, {"//"}, {{"/*", "*/", COMMENT}, {"`", "`", STRING}}, "\"'", go_keywords},
	{nix_exts, {"#"}, {{"/*", "*/", COMMENT}, {"''", "''", STRING}}, "\"", nix_keywords},
	{slash_exts, {"//"}, {{"/*", "*/", COMMENT}}, "\"'", none},
	{hash_exts, {"#"}, {{0}}, "\"'", none},
	{dash_exts, {"--"}, {{0}}, "\"'", none},
};

/* Names without an extension */
static const struct {
	const char *name;
	const struct lang *lang;
} names[] = {
	{"Makefile", &langs[8]}, {"makefile", &langs[8]}, {"GNUmakefile", &langs[8]},
	{"Dockerfile", &langs[8]}, {"justfile", &langs[8]}, {"CMakeLists.txt", &langs[8]},
	{"Config.in", &langs[8]},
};
/* NON-NLS-END */

const struct lang *highlight_lang(const char *filename)
{
	const char *base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	for (size_t i = 0; i < LV_ARRAYLEN(names); i++)
		if (!strcmp(base, names[i].name))
			return names[i].lang;
	const char *dot = strrchr(base, '.');
	if (!dot || dot == base)
		return NULL;
	for (size_t i = 0; i < LV_ARRAYLEN(langs); i++)
		for (const char *const *e = langs[i].exts; *e; e++)
			if (!strcasecmp(dot + 1, *e))
				return &langs[i];
	return NULL;
}

/* The output, its color open */
struct out {
	char *s;
	size_t len, size;
	uint32_t colors[COMMENT + 1];
	enum kind open; /* PLAIN: none */
};

static void append(struct out *o, const char *s, size_t n)
{
	if (o->len + n + 1 > o->size) {
		size_t size = (o->len + n + 1) * 2;
		char *p = realloc(o->s, size);
		if (!p)
			return;
		o->s = p;
		o->size = size;
	}
	memcpy(o->s + o->len, s, n);
	o->len += n;
	o->s[o->len] = '\0';
}

static void close_color(struct out *o)
{
	if (o->open != PLAIN)
		append(o, "#", 1);
	o->open = PLAIN;
}

static void put(struct out *o, char c, enum kind kind)
{
	if (c == '#' || c == '\n') {
		close_color(o);
		append(o, c == '#' ? "##" : "\n", c == '#' ? 2 : 1);
		return;
	}
	if (kind != o->open) {
		close_color(o);
		if (kind != PLAIN) {
			char mark[9];
			lv_snprintf(mark, sizeof(mark), "#%06" PRIx32 " ", o->colors[kind]);
			append(o, mark, 8);
		}
		o->open = kind;
	}
	append(o, &c, 1);
}

static void put_n(struct out *o, const char *s, size_t n, enum kind kind)
{
	for (size_t i = 0; i < n; i++)
		put(o, s[i], kind);
}

static bool starts(const char *s, const char *end, const char *with)
{
	size_t n = strlen(with);
	return n && (size_t)(end - s) >= n && !memcmp(s, with, n);
}

static bool word_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static bool is_keyword(const struct lang *l, const char *s, size_t n)
{
	for (const char *const *k = l->keywords; *k; k++)
		if (strlen(*k) == n && !memcmp(*k, s, n))
			return true;
	return false;
}

/* One line's code, from s to end; *block: the block it starts in, after it ends in
 * (-1: none) */
static void line(struct out *o, const struct lang *l, const char *s, const char *end, int *block)
{
	while (s < end) {
		/* In a block: to its close, or the line's end */
		if (*block >= 0) {
			const struct block *b = &l->blocks[*block];
			const char *close = s;
			while (close < end && !starts(close, end, b->close))
				close++;
			if (close < end) {
				close += strlen(b->close);
				*block = -1;
			}
			put_n(o, s, close - s, b->kind);
			s = close;
			continue;
		}
		bool done = false;
		/* A line comment: the rest */
		for (int i = 0; i < 2 && !done; i++)
			if (l->line_comment[i] && starts(s, end, l->line_comment[i])) {
				put_n(o, s, end - s, COMMENT);
				s = end;
				done = true;
			}
		/* A block opening */
		for (int i = 0; i < 3 && !done && l->blocks[i].open; i++)
			if (starts(s, end, l->blocks[i].open)) {
				size_t n = strlen(l->blocks[i].open);
				put_n(o, s, n, l->blocks[i].kind);
				s += n;
				*block = i;
				done = true;
			}
		if (done)
			continue;
		const char *t = s;
		if (strchr(l->quotes, *s)) {
			/* A string, to its quote or the line's end */
			for (t++; t < end && *t != *s; t++)
				if (*t == '\\' && t + 1 < end)
					t++;
			if (t < end)
				t++;
			put_n(o, s, t - s, STRING);
		} else if (isdigit((unsigned char)*s)) {
			while (t < end && (word_char(*t) || *t == '.'))
				t++;
			put_n(o, s, t - s, NUMBER);
		} else if (word_char(*s)) {
			while (t < end && word_char(*t))
				t++;
			put_n(o, s, t - s, is_keyword(l, s, t - s) ? KEYWORD : PLAIN);
		} else {
			put(o, *s, PLAIN);
			t++;
		}
		s = t;
	}
}

char *highlight(const struct lang *l, const char *text)
{
	struct out o = {0};
	/* GitHub's: its dark theme's, or its light one's on a light card */
	bool light = lv_color_luminance(lv_color_hex(UI_COLOR_CARD)) > 128;
	static const uint32_t dark[] = {[KEYWORD] = 0xff7b72, [STRING] = 0xa5d6ff, [NUMBER] = 0x79c0ff,
					[COMMENT] = 0x8b949e};
	static const uint32_t bright[] = {[KEYWORD] = 0xcf222e, [STRING] = 0x0a3069, [NUMBER] = 0x0550ae,
					  [COMMENT] = 0x6e7781};
	memcpy(o.colors, light ? bright : dark, sizeof(o.colors));
	append(&o, "", 0);
	int block = -1;
	for (const char *s = text; *s;) {
		const char *end = strchrnul(s, '\n');
		if (*s == '@') {
			/* A hunk's header: as is, and what came before is unknown */
			put_n(&o, s, end - s, PLAIN);
			block = -1;
		} else if (s < end) {
			put(&o, *s, PLAIN);
			line(&o, l, s + 1, end, &block);
		}
		close_color(&o);
		if (*end)
			put(&o, '\n', PLAIN);
		s = *end ? end + 1 : end;
	}
	return o.s;
}
