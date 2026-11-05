/* Translations: the current language's table, sorted once, searched by the English text */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"

static const struct ui_language english = { "en", "English", NULL, 0 };
extern const struct ui_language ui_language_ro;

const struct ui_language *const ui_languages[] = { &english, &ui_language_ro };
const size_t ui_language_count = sizeof(ui_languages) / sizeof(ui_languages[0]);

static const struct ui_language *current = &english;

static int translation_cmp(const void *a, const void *b)
{
	return strcmp(((const struct ui_translation *)a)->text, ((const struct ui_translation *)b)->text);
}

const struct ui_language *ui_language(void)
{
	return current;
}

bool ui_language_use(const char *code)
{
	static bool sorted[sizeof(ui_languages) / sizeof(ui_languages[0])];
	for (size_t i = 0; i < ui_language_count; i++) {
		const struct ui_language *l = ui_languages[i];
		if (strcmp(l->code, code) != 0)
			continue;
		if (l->table && !sorted[i]) {
			qsort(l->table, l->count, sizeof(l->table[0]), translation_cmp);
			sorted[i] = true;
		}
		current = l;
		return true;
	}
	return false;
}

const char *ui_tr(const char *text)
{
	if (current->table) {
		struct ui_translation key = { text, NULL };
		const struct ui_translation *t = bsearch(&key, current->table, current->count, sizeof(key),
		                                         translation_cmp);
		if (t)
			return t->translated;
	}
	const char *context = strchr(text, '\004');
	return context ? context + 1 : text;
}
