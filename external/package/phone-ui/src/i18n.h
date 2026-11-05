/*
 * Translations (i18n.c), as gettext's: the English text is the key. _("...") is its
 * translation in the current language, the text itself without one (English, or not
 * translated yet); C_("context", "...") when the same English needs two (an adjective's
 * gender: "Good" signal, battery health); N_("...") and NC_("context", "...") mark a text
 * translated later with _() (static tables). Translations are static: labels keep them with
 * lv_label_set_text_static(). A format's translation keeps its conversions in order, and
 * the compiler checks the English one against the arguments.
 *
 * Each language has a table of its own, i18n_<code>.c: { English, translation } pairs in
 * any order (sorted when first used), listed in i18n.c. tests/test_i18n.c fails for a text marked in the sources without a
 * translation in each, a translation of a text no longer there, or different conversions.
 *
 * Only Settings is translated so far.
 */

#ifndef PHONE_UI_I18N_H
#define PHONE_UI_I18N_H

#include <stdbool.h>
#include <stddef.h>

#define _(text) ui_tr(text)
#define C_(context, text) ui_tr(context "\004" text)
#define N_(text) (text)
#define NC_(context, text) (context "\004" text)

const char *ui_tr(const char *text) __attribute__((format_arg(1)));

struct ui_translation {
	const char *text, *translated;
};

struct ui_language {
	const char *code; /* the setting's value, "en" */
	const char *name; /* in itself: "Română" */
	struct ui_translation *table; /* NULL for English */
	size_t count;
};

/* English first; i18n_<code>.c defines ui_language_<code> */
extern const struct ui_language *const ui_languages[];
extern const size_t ui_language_count;

const struct ui_language *ui_language(void);
/* Makes `code` the current language, as it is: false if there is none such */
bool ui_language_use(const char *code);
/* ...and saves it, then rebuilds every screen as ui_set_light() does, `reopen` pushing
 * the ones to show on top again (ui.c) */
void ui_language_set(const char *code, void (*reopen)(void));

#endif
