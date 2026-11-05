/* Translations: the current language's table (gen-translations.py, sorted by the English
 * text), searched by the English text; the names of days and months */

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

static const struct ui_translation *find(const char *text)
{
	if (!current || !current->table)
		return NULL;
	struct ui_translation key = { text, NULL, 0 };
	return bsearch(&key, current->table, current->count, sizeof(key), translation_cmp);
}

const char *ui_tr(const char *text)
{
	const struct ui_translation *t = find(text);
	if (t)
		return t->translated;
	const char *context = strchr(text, '\004');
	return context ? context + 1 : text;
}

const char *ui_trn(const char *one, const char *other, unsigned long n)
{
	const struct ui_translation *t = find(one);
	if (!t || !current->plural)
		return n == 1 ? one : other;
	unsigned form = current->plural(n);
	const char *s = t->translated;
	for (unsigned i = 0; i < form && i + 1 < t->forms; i++)
		s += strlen(s) + 1;
	return s;
}

/* As the C locale's strftime() has them */
static const char *const days[2][7] = {
	{ N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"), N_("Thursday"), N_("Friday"),
	  N_("Saturday") },
	{ N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"), N_("Fri"), N_("Sat") },
};
static const char *const months[2][12] = {
	{ N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"), N_("July"),
	  N_("August"), N_("September"), N_("October"), N_("November"), N_("December") },
	{ N_("Jan"), N_("Feb"), N_("Mar"), N_("Apr"), N_("May"), N_("Jun"), N_("Jul"), N_("Aug"),
	  N_("Sep"), N_("Oct"), N_("Nov"), N_("Dec") },
};

const char *ui_day_name(int wday, bool short_name)
{
	return _(days[short_name][wday >= 0 && wday < 7 ? wday : 0]);
}

const char *ui_month_name(int mon, bool short_name)
{
	return _(months[short_name][mon >= 0 && mon < 12 ? mon : 0]);
}
