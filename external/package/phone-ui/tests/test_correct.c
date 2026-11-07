/* The keyboard's autocorrection (correct.c) on its dictionaries (words.c) */

#include <string.h>

#include "test.h"

#include "correct.h"

static const struct correct_dict *en, *ro;

TEST_SETUP
{
	en = correct_dict("en");
	ro = correct_dict("ro");
}

/* The word's keys pressed dead centre, the i-th (0: none) at (dx, dy) from its centre */
static int typed_at(const struct correct_dict *d, const char *keys, int i, float dx, float dy,
                    struct correct_word out[CORRECT_MAX])
{
	struct correct_touch t[CORRECT_LETTERS_MAX + 1];
	int n = (int)strlen(keys);
	CHECK(n <= CORRECT_LETTERS_MAX + 1);
	for (int k = 0; k < n; k++)
		t[k] = (struct correct_touch){ keys[k], k + 1 == i ? dx : 0, k + 1 == i ? dy : 0 };
	return correct(d, t, n, out);
}

static const char *first_at(const struct correct_dict *d, const char *keys, int i, float dx, float dy)
{
	static struct correct_word out[CORRECT_MAX];
	return typed_at(d, keys, i, dx, dy, out) ? out[0].text : "(none)";
}

static const char *first(const struct correct_dict *d, const char *keys)
{
	return first_at(d, keys, 0, 0, 0);
}

/* Every key pressed at (dx, dy) from its centre */
static const char *first_all(const struct correct_dict *d, const char *keys, float dx, float dy)
{
	struct correct_touch t[CORRECT_LETTERS_MAX];
	static struct correct_word out[CORRECT_MAX];
	int n = (int)strlen(keys);
	for (int k = 0; k < n; k++)
		t[k] = (struct correct_touch){ keys[k], dx, dy };
	return correct(d, t, n, out) ? out[0].text : "(none)";
}

static bool offered(const struct correct_dict *d, const char *keys, const char *word)
{
	struct correct_word out[CORRECT_MAX];
	int n = typed_at(d, keys, 0, 0, 0, out);
	for (int i = 0; i < n; i++)
		if (strcmp(out[i].text, word) == 0)
			return true;
	return false;
}

TEST(dictionaries)
{
	CHECK(en && ro);
	CHECK(!correct_dict("xx"));
	CHECK_STR(en->language, "en");
	CHECK(correct_known(en, "the"));
	CHECK(!correct_known(en, "teh"));
	CHECK(!correct_known(en, ""));
	CHECK(correct_known(ro, "știi"));
	CHECK(!correct_known(ro, "stii")); /* folded into știi: subtitles without diacritics */
	CHECK(correct_known(ro, "fata"));  /* a word of its own beside fată */
	CHECK(correct_known(ro, "să-l"));
	/* What a word costs as typed: its frequency; the top word nothing */
	CHECK_INT(correct_cost(en, "you"), 0);
	CHECK(correct_cost(en, "lazy") > correct_cost(en, "lady"));
	CHECK_INT(correct_cost(en, "teh"), -1);
}

TEST(exact_word_first)
{
	CHECK_STR(first(en, "hello"), "hello");
	CHECK_STR(first(en, "the"), "the");
	CHECK_STR(first(ro, "sunt"), "sunt");
}

TEST(upper_case_keys)
{
	CHECK_STR(first(en, "Hello"), "hello");
}

TEST(swapped_letters)
{
	CHECK_STR(first(en, "teh"), "the");
	CHECK_STR(first(en, "hlelo"), "hello");
}

TEST(missed_letter)
{
	CHECK_STR(first(en, "helo"), "hello");
	CHECK_STR(first(en, "becuse"), "because");
}

TEST(extra_letter)
{
	CHECK_STR(first(en, "helllo"), "hello");
}

TEST(far_key)
{
	/* x is nowhere near l: an edit, not a slip of the finger */
	CHECK_STR(first(en, "hexlo"), "hello");
}

TEST(two_edits_in_a_long_word)
{
	CHECK_STR(first(en, "beutifull"), "beautiful");
	/* not in a short one: a word within one edit, or nothing */
	CHECK_STR(first(en, "ehlol"), "(none)");
}

TEST(neighbouring_key)
{
	/* g is beside h on the keyboard: no edit needed, the finger slipped */
	CHECK_STR(first(en, "tge"), "the");
}

TEST(finger_near_the_edge)
{
	/* "us" dead centre is us; the u pressed at its right edge, towards i, with "is" five
	 * times as common: is. The i of "is" pressed at its left edge stays is: the likelier
	 * word, with the same doubt about the key */
	CHECK_STR(first(en, "us"), "us");
	CHECK_STR(first_at(en, "us", 1, 0.45f, 0), "is");
	CHECK_STR(first_at(en, "is", 1, -0.45f, 0), "is");
}

TEST(long_rare_word_typed_a_little_off)
{
	/* Every key a fifth off its centre: a letter or two from a common word would cost less
	 * in slips, but a word is as likely as it is typed, whatever its frequency */
	CHECK_STR(first_all(en, "misunderstanding", 0.2f, -0.1f), "misunderstanding");
	CHECK_STR(first_all(en, "unfortunately", -0.2f, 0.2f), "unfortunately");
}

TEST(corner_presses_keep_the_word)
{
	/* Every key pressed in its corner: what the fingers cost is the same for every word, so
	 * the word typed (known) is not beaten by a commoner one an edit away, which would
	 * otherwise skip the slip of the letter it replaces */
	CHECK_STR(first_all(en, "lazy", 0.45f, 0.45f), "lazy");
	CHECK_STR(first_all(en, "fried", 0.45f, 0.45f), "fried");
	CHECK_STR(first_all(en, "fried", -0.45f, 0.45f), "fried");
	/* And a word an edit away is still found with the fingers in the gaps */
	CHECK_STR(first_all(en, "jumpz", 0.6f, 0.6f), "jumps");
}

TEST(romanian_diacritics)
{
	CHECK_STR(first(ro, "stii"), "știi");
	CHECK_STR(first(ro, "cand"), "când");
	CHECK_STR(first(ro, "tara"), "țară");
	CHECK_STR(first(ro, "fata"), "fata");
	/* față (face), nearly as common, is offered too; fată (girl) comes fourth */
	CHECK(offered(ro, "fata", "față"));
	/* sa (his, her) is folded into să, five times as common, as a spelling without the
	 * diacritic (gen-words.py cannot tell it from stii): the keyboard offers what was
	 * typed anyway */
	CHECK_STR(first(ro, "sa"), "să");
	CHECK(!correct_known(ro, "sa"));
	/* The hyphen has no key: cheap to add */
	CHECK_STR(first(ro, "sal"), "să-l");
}

TEST(english_contractions)
{
	CHECK_STR(first(en, "dont"), "don't");
	CHECK_STR(first(en, "im"), "i'm");
	CHECK(correct_known(en, "didn't"));
	CHECK(!correct_known(en, "didn"));
}

TEST(nothing_close)
{
	struct correct_word out[CORRECT_MAX];
	CHECK_INT(typed_at(en, "qzxvk", 0, 0, 0, out), 0);
	CHECK_INT(typed_at(en, "", 0, 0, 0, out), 0);
	CHECK_INT(typed_at(en, "abcdefghijklmnopqrstuvwxy", 0, 0, 0, out), 0); /* too long */
}

TEST(candidates_ordered)
{
	struct correct_word out[CORRECT_MAX];
	int n = typed_at(en, "ther", 0, 0, 0, out);
	CHECK(n >= 2);
	for (int i = 1; i < n; i++) {
		CHECK(out[i - 1].cost <= out[i].cost);
		CHECK(strcmp(out[i - 1].text, out[i].text) != 0);
	}
}
