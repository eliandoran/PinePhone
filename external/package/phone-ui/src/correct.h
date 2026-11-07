/*
 * Autocorrection for the on-screen keyboard: the word a user meant from the keys they
 * pressed and where the finger landed on each. The dictionaries are words.c, generated
 * by gen-words.py from words/<code>.txt (a language's words with their frequency).
 */

#ifndef CORRECT_H
#define CORRECT_H

#include <stdbool.h>
#include <stdint.h>

#define CORRECT_LETTERS_MAX 24                         /* letters of a word, typed or known */
#define CORRECT_WORD_MAX (CORRECT_LETTERS_MAX * 2 + 1) /* bytes: a letter is 2 at most */
#define CORRECT_MAX 3                                  /* candidates offered */

/* A language's words (words.c): a trie of letters, each node 32 bits */
struct correct_dict {
	const char *language;       /* its code, as i18n's: "en" */
	const char *const *letters; /* its alphabet, UTF-8, by the nodes' letter index */
	const char *base;           /* each letter's key on the keyboard (ș: s), 0 for none (') */
	int letter_count;
	const uint32_t *nodes;
	uint32_t node_count;
	int freq_max; /* the most frequent word's frequency (log2 of its count, in quarters) */
};

extern const struct correct_dict correct_dicts[];
extern const int correct_dict_count;

/* The dictionary of a language, NULL without one */
const struct correct_dict *correct_dict(const char *language);

/* A key pressed: the letter on it (lower case, as the keyboard's letter rows) and where
 * the finger landed, in key widths and heights from the key's centre: (0, 0) dead
 * centre, (0.5, 0) its right edge */
struct correct_touch {
	char key;
	float dx, dy;
};

struct correct_word {
	char text[CORRECT_WORD_MAX];
	int cost; /* -log2 of its likelihood in tenths of a bit; smaller is likelier */
};

/* The words most likely meant by these touches, likeliest first: how many (0: none
 * known comes close enough), in out[]. The typed word itself, if known, is among them. */
int correct(const struct correct_dict *d, const struct correct_touch *touches, int n,
            struct correct_word out[CORRECT_MAX]);

/* Whether the word (lower case) is in the dictionary */
bool correct_known(const struct correct_dict *d, const char *word);

#endif
