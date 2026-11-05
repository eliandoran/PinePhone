/* Translations: the current language's table (gen-translations.py, sorted by the English
 * text), searched by the English text */

#include <stdlib.h>
#include <string.h>

#include "i18n.h"

static const struct ui_language *current; /* NULL: English, the first */

static int translation_cmp(const void *a, const void *b)
{
	return strcmp(((const struct ui_translation *)a)->text, ((const struct ui_translation *)b)->text);
}

const struct ui_language *ui_language(void)
{
	return current ? current : ui_languages[0];
}

bool ui_language_use(const char *code)
{
	for (size_t i = 0; i < ui_language_count; i++)
		if (strcmp(ui_languages[i]->code, code) == 0) {
			current = ui_languages[i];
			return true;
		}
	return false;
}

const char *ui_tr(const char *text)
{
	if (current && current->table) {
		struct ui_translation key = { text, NULL };
		const struct ui_translation *t = bsearch(&key, current->table, current->count, sizeof(key),
		                                         translation_cmp);
		if (t)
			return t->translated;
	}
	const char *context = strchr(text, '\004');
	return context ? context + 1 : text;
}
