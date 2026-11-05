/* i18n.c: texts looked up in the current language, its tables against the sources: each
 * text marked _("..."), C_("...", "..."), N_("..."), NC_() or P_() in the .c files of
 * ../src (SRC_DIR) has a translation in every language, each translation is of one, with
 * the same printf conversions (each of a plural's forms) */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "i18n.h"

TEST(english_by_default)
{
	CHECK_STR(ui_language()->code, "en");
	CHECK_STR(_("Settings"), "Settings");
	CHECK_STR(C_("signal", "Good"), "Good");
}

TEST(romanian)
{
	CHECK(ui_language_use("ro"));
	CHECK_STR(ui_language()->code, "ro");
	CHECK_STR(ui_language()->name, "Română");
	CHECK_STR(_("Settings"), "Setări");
	CHECK_STR(_("%s free"), "%s liberi");
	/* The same English word, two translations */
	CHECK_STR(C_("signal", "Good"), "Bun");
	CHECK_STR(C_("battery health", "Good"), "Bună");
	CHECK(ui_language_use("en"));
	CHECK_STR(_("Settings"), "Settings");
}

/* Not translated: the text itself, as given (it may not be a literal) */
TEST(untranslated_text_as_is)
{
	CHECK(ui_language_use("ro"));
	char text[] = "No such text";
	CHECK(_(text) == text);
	CHECK_STR(C_("nowhere", "No such text"), "No such text");
}

TEST(unknown_language_refused)
{
	CHECK(ui_language_use("ro"));
	CHECK(!ui_language_use("xx"));
	CHECK_STR(ui_language()->code, "ro");
}

TEST(plural_english)
{
	CHECK_STR(P_("%u contact", "%u contacts", 1), "%u contact");
	CHECK_STR(P_("%u contact", "%u contacts", 0), "%u contacts");
	CHECK_STR(P_("%u contact", "%u contacts", 2), "%u contacts");
}

/* Romanian's three forms: one; zero and those ending in 01 to 19 but one ("101 contacte");
 * the rest ("20 de contacte") */
TEST(plural_romanian)
{
	CHECK(ui_language_use("ro"));
	CHECK_STR(P_("%u contact", "%u contacts", 1), "%u contact");
	CHECK_STR(P_("%u contact", "%u contacts", 0), "%u contacte");
	CHECK_STR(P_("%u contact", "%u contacts", 2), "%u contacte");
	CHECK_STR(P_("%u contact", "%u contacts", 19), "%u contacte");
	CHECK_STR(P_("%u contact", "%u contacts", 20), "%u de contacte");
	CHECK_STR(P_("%u contact", "%u contacts", 100), "%u de contacte");
	CHECK_STR(P_("%u contact", "%u contacts", 101), "%u contacte");
	/* Not translated: English's */
	CHECK_STR(P_("%u thing", "%u things", 1), "%u thing");
	CHECK_STR(P_("%u thing", "%u things", 20), "%u things");
}

/* The phone has no locales: the names are the C locale's in English, as strftime() gives */
TEST(day_and_month_names)
{
	CHECK_STR(ui_day_name(4, false), "Thursday");
	CHECK_STR(ui_day_name(4, true), "Thu");
	CHECK_STR(ui_month_name(10, false), "November");
	CHECK_STR(ui_month_name(10, true), "Nov");
	CHECK(ui_language_use("ro"));
	CHECK_STR(ui_day_name(4, false), "joi");
	CHECK_STR(ui_day_name(2, true), "mar.");
	CHECK_STR(ui_month_name(10, false), "noiembrie");
	CHECK_STR(ui_month_name(10, true), "nov.");
	CHECK_STR(ui_month_name(4, true), "mai");
}

/* The texts marked in the sources */

static char *marked[4096];
static int marked_count;

static void mark(const char *text)
{
	for (int i = 0; i < marked_count; i++)
		if (strcmp(marked[i], text) == 0)
			return;
	CHECK(marked_count < (int)(sizeof(marked) / sizeof(marked[0])));
	marked[marked_count++] = strdup(text);
}

/* A C string literal at *p (after its quote) into out, *p after its closing quote */
static void literal(const char **p, char *out, size_t size)
{
	const char *s = *p;
	size_t n = 0;
	while (*s && *s != '"') {
		char c = *s++;
		if (c == '\\') {
			c = *s++;
			switch (c) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'x': c = (char)strtol(s, (char **)&s, 16); break;
			default: break; /* \" \\ */
			}
		}
		CHECK(n + 1 < size);
		out[n++] = c;
	}
	out[n] = '\0';
	*p = *s ? s + 1 : s;
}

static bool ident(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void scan_file(const char *path)
{
	FILE *f = fopen(path, "r");
	CHECK(f);
	static char src[1 << 20];
	size_t len = fread(src, 1, sizeof(src) - 1, f);
	fclose(f);
	src[len] = '\0';
	for (const char *p = src; (p = strstr(p, "_(\"")); ) {
		char kind = p > src ? p[-1] : ' ';
		if ((kind == 'N' || kind == 'C' || kind == 'P') && (p - 1 == src || !ident(p[-2])))
			;
		else if (kind == 'C' && p - 2 >= src && p[-2] == 'N' && (p - 2 == src || !ident(p[-3])))
			; /* NC_ */
		else if (ident(kind)) {
			p += 3;
			continue;
		} else
			kind = '_';
		p += 3;
		char text[512], context[256];
		if (kind == 'C') {
			literal(&p, context, sizeof(context));
			CHECK(strncmp(p, ", \"", 3) == 0);
			p += 3;
			literal(&p, text, sizeof(text));
			char key[800];
			snprintf(key, sizeof(key), "%s\004%s", context, text);
			mark(key);
		} else {
			literal(&p, text, sizeof(text));
			mark(text);
			if (kind == 'P') { /* the plural, the same message */
				CHECK(strncmp(p, ", \"", 3) == 0);
				p += 3;
				literal(&p, text, sizeof(text));
			}
		}
	}
}

static void scan_dir(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	CHECK(d);
	struct dirent *ent;
	while ((ent = readdir(d))) {
		char path[512];
		size_t len = strlen(ent->d_name);
		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (ent->d_type == DT_DIR && depth == 0)
			scan_dir(path, 1);
		else if (len > 2 && strcmp(ent->d_name + len - 2, ".c") == 0)
			scan_file(path);
	}
	closedir(d);
}

static void scan(void)
{
	if (!marked_count)
		scan_dir(SRC_DIR, 0);
}

static const struct ui_translation *find(const struct ui_language *l, const char *text)
{
	for (size_t i = 0; i < l->count; i++)
		if (strcmp(l->table[i].text, text) == 0)
			return &l->table[i];
	return NULL;
}

TEST(sources_marked)
{
	scan();
	CHECK(marked_count > 100);
}

TEST(every_marked_text_translated)
{
	scan();
	int missing = 0;
	for (size_t l = 1; l < ui_language_count; l++)
		for (int i = 0; i < marked_count; i++)
			if (!find(ui_languages[l], marked[i])) {
				printf("%s: no translation of \"%s\"\n", ui_languages[l]->code, marked[i]);
				missing++;
			}
	CHECK_INT(missing, 0);
}

TEST(every_translation_used)
{
	scan();
	int unused = 0;
	for (size_t l = 1; l < ui_language_count; l++)
		for (size_t i = 0; i < ui_languages[l]->count; i++) {
			int j = 0;
			while (j < marked_count && strcmp(marked[j], ui_languages[l]->table[i].text) != 0)
				j++;
			if (j == marked_count) {
				printf("%s: \"%s\" is not in the sources\n", ui_languages[l]->code, ui_languages[l]->table[i].text);
				unused++;
			}
		}
	CHECK_INT(unused, 0);
}

/* A format's conversions, e.g. "%s free of %llu MB" gives "s llu " */
static void conversions(const char *fmt, char *out, size_t size)
{
	size_t n = 0;
	out[0] = '\0';
	for (const char *p = fmt; (p = strchr(p, '%')); ) {
		p++;
		if (*p == '%') {
			p++;
			continue;
		}
		p += strspn(p, "-+ #0123456789.*");
		size_t len = strspn(p, "hlLqjzt") + 1;
		n += (size_t)snprintf(out + n, size - n, "%.*s ", (int)len, p);
		p += len;
	}
}

TEST(same_conversions)
{
	int wrong = 0;
	for (size_t l = 1; l < ui_language_count; l++)
		for (size_t i = 0; i < ui_languages[l]->count; i++) {
			const struct ui_translation *t = &ui_languages[l]->table[i];
			char a[64], b[64];
			conversions(t->text, a, sizeof(a));
			const char *form = t->translated;
			for (unsigned k = 0; k < t->forms; k++, form += strlen(form) + 1) {
				conversions(form, b, sizeof(b));
				if (strcmp(a, b) != 0) {
					printf("%s: \"%s\" has \"%s\", \"%s\" \"%s\"\n", ui_languages[l]->code, t->text, a,
					       form, b);
					wrong++;
				}
			}
		}
	CHECK_INT(wrong, 0);
}

TEST(no_duplicates_or_empty)
{
	for (size_t l = 1; l < ui_language_count; l++)
		for (size_t i = 0; i < ui_languages[l]->count; i++) {
			const struct ui_translation *t = &ui_languages[l]->table[i];
			CHECK(t->translated[0]);
			CHECK(find(ui_languages[l], t->text) == t);
		}
}
