/*
 * Translations (i18n.c), as gettext's: the English text is the key. _("...") is its
 * translation in the current language, the text itself without one (English, or not
 * translated yet); C_("context", "...") when the same English needs two (an adjective's
 * gender: "Good" signal, battery health); N_("...") and NC_("context", "...") mark a text
 * translated later with _() (static tables); P_("%u contact", "%u contacts", n) the form
 * for a number, as ngettext() (Romanian has three). Translations are static: labels keep
 * them with lv_label_set_text_static(). A format's translation keeps its conversions in
 * order, and the compiler checks the English one against the arguments.
 *
 * Each language is a gettext catalog, po/<code>.po beside src/, which gen-translations.py
 * turns at build time into ui_languages and their tables ({ English, translation } pairs,
 * sorted), C_()'s context joined to the text with \004 (msgctxt), a plural's forms one
 * after the other, each ended by its NUL, with its catalog's Plural-Forms. tests/test_i18n.c
 * fails for a text marked in the sources without a translation in each, a translation of a
 * text no longer there, or different conversions.
 *
 * i18n-check.py (`just i18n`) gives each file's texts marked and not; a text shown as it is
 * in every language (a product name, "Bluetooth") carries a NON-NLS comment on its line.
 * `just test` fails on one not marked anywhere in src/ (i18n-check in tests/Makefile).
 *
 * A plugin app (external.c) has a catalog of its own, apps/<name>/po/<code>.po, in the
 * language phone-ui is in: its Makefile compiles gen-translations.py --app's C of it, and
 * everything that uses these macros, with -DUI_APP_CATALOG, which makes _(), C_() and P_()
 * look in ui_app_catalog (hidden: each plugin has its own) instead of phone-ui's tables.
 */

#ifndef PHONE_UI_I18N_H
#define PHONE_UI_I18N_H

#include <stdbool.h>
#include <stddef.h>

struct ui_translation {
	const char *text, *translated;
	unsigned forms; /* 1, or a plural's: its language's count of them */
};

struct ui_language {
	const char *code; /* the setting's value, "en" */
	const char *name; /* in itself: "Română" */
	const struct ui_translation *table; /* NULL for English */
	size_t count;
	unsigned (*plural)(unsigned long n); /* the form for n (Plural-Forms); NULL: English's */
};

/* A plugin app's catalog: its languages (English left out), by their code */
struct ui_catalog {
	const struct ui_language *const *languages;
	size_t count;
};

const char *ui_tr(const char *text) __attribute__((format_arg(1)));
const char *ui_trn(const char *one, const char *other, unsigned long n)
	__attribute__((format_arg(1))) __attribute__((format_arg(2)));
/* As those, from `catalog`'s table for the current language */
const char *ui_tr_from(const struct ui_catalog *catalog, const char *text) __attribute__((format_arg(2)));
const char *ui_trn_from(const struct ui_catalog *catalog, const char *one, const char *other,
                        unsigned long n) __attribute__((format_arg(2))) __attribute__((format_arg(3)));

#ifdef UI_APP_CATALOG
extern const struct ui_catalog ui_app_catalog __attribute__((visibility("hidden")));
#define _(text) ui_tr_from(&ui_app_catalog, text)
#define P_(one, other, n) ui_trn_from(&ui_app_catalog, one, other, n)
#else
#define _(text) ui_tr(text)
#define P_(one, other, n) ui_trn(one, other, n)
#endif
#define C_(context, text) _(context "\004" text)
#define N_(text) (text)
#define NC_(context, text) (context "\004" text)

/* English first, then po/'s by file name (generated) */
extern const struct ui_language *const ui_languages[];
extern const size_t ui_language_count;

const struct ui_language *ui_language(void);
/* Makes `code` the current language, as it is: false if there is none such */
bool ui_language_use(const char *code);
/* ...and saves it, then rebuilds every screen as ui_set_light() does, `reopen` pushing
 * the ones to show on top again (ui.c) */
void ui_language_set(const char *code, void (*reopen)(void));

/* The names of a day (struct tm's tm_wday, 0 Sunday) and a month (tm_mon, 0 January), whole
 * or short ("Thu", "Nov"), in the current language: the phone has no locales for strftime() */
const char *ui_day_name(int wday, bool short_name);
const char *ui_month_name(int mon, bool short_name);

/* The kernel's words for a battery's status and health (power supply class: "Charging",
 * "Good"), translated; others as they are (battery.c) */
const char *ui_battery_word(const char *word);

#endif
