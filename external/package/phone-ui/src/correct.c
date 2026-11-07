/*
 * Autocorrection: the word a user meant from the keys they pressed and where the
 * finger landed on each, as a noisy channel. A typed word is a sequence of touches,
 * each a letter's key and the finger's offset from its centre; a dictionary word is as
 * likely as its letters' keys are close to the touches (a bell over each key, SIGMA
 * wide, so a finger at the edge between two keys means either, a key away a slip costs
 * more than an edit), times how common the word is, with one edit allowed (a letter typed too many or missed, two the other way
 * round, or a key far away; two edits in a long word). Costs are -log2 of a probability
 * in tenths of a bit: a word costs 2.5 for each halving of its frequency, a letter the
 * square of its distance in key widths times SPATIAL less what the key hit costs the same
 * way (paid whatever the word: fingers land off-centre; the offset tells between a key and
 * its neighbours, and a far letter is an edit, not a slip), an edit EDIT (a letter without a
 * key, ' or -, missed only PUNCT: the user cannot type it from the letters; a letter
 * doubled or its double missed DOUBLE, the commonest slip). The three
 * cheapest words win, found by walking the dictionary's trie (words.c) and pruning what
 * already costs more than the third, or more in slips and edits than a word meant can
 * (CLOSE: its frequency, however rare, does not count against it).
 */

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include "correct.h"

#define SPATIAL 80 /* per key width squared: a bell with SIGMA 0.3 key, 1 / (2 σ² ln 2) */
#define EDIT 60
#define PUNCT 25
#define DOUBLE 30  /* a letter doubled, or its double missed: the commonest slip */
#define CLOSE 200  /* slips and edits costing more are not a word meant: its frequency does not count */
#define TWO_EDITS 8 /* letters typed from which two edits are allowed, when one finds nothing */

/* A node: its first child's index (0: none; the root's children are first and are no
 * node's), whether it is its parent's last child, its letter and the frequency of the
 * word ending there (0: none), as gen-words.py packs them */
#define CHILD(n) ((n) & 0x3ffff)
#define LAST(n) ((n) >> 18 & 1)
#define LETTER(n) ((n) >> 19 & 0x3f)
#define FREQ(n) ((n) >> 25)

/* The letter rows as keyboard.c lays them out: each key two units of twenty, the rows
 * indented by half a key and a key and a half */
static const struct {
	const char *keys;
	float x0;
} kb_rows[] = {
	{ "qwertyuiop", 0 },
	{ "asdfghjkl", 0.5f },
	{ "zxcvbnm", 1.5f },
};

static bool key_position(char key, float *x, float *y)
{
	for (unsigned r = 0; r < sizeof(kb_rows) / sizeof(*kb_rows); r++) {
		const char *k = strchr(kb_rows[r].keys, key);
		if (k && key) {
			*x = kb_rows[r].x0 + (float)(k - kb_rows[r].keys) + 0.5f;
			*y = (float)r + 0.5f;
			return true;
		}
	}
	return false;
}

const struct correct_dict *correct_dict(const char *language)
{
	for (int i = 0; i < correct_dict_count; i++)
		if (strcmp(correct_dicts[i].language, language) == 0)
			return &correct_dicts[i];
	return NULL;
}

struct search {
	const struct correct_dict *d;
	const struct correct_touch *t;
	int own[CORRECT_LETTERS_MAX]; /* what each touch costs on the key it hit: paid whatever the word */
	int n;
	int max_edits;
	uint8_t path[CORRECT_LETTERS_MAX]; /* the letters taken so far */
	struct correct_word *out;
	int found;
	int bound; /* a word costing this much in all is not kept: the third candidate's cost */
};

/* What the touch costs for the letter on top of what it costs on the key it hit: -1 when
 * more than an edit, so a far letter is one, not a slip of the finger. Rounding aside, 0
 * for the key hit, so a word typed as it is pays nothing however off-centre the fingers,
 * and the offset only tells between a key and its neighbours */
static int touch_cost(const struct search *s, int i, int letter)
{
	const struct correct_touch *t = &s->t[i];
	char base = s->d->base[letter];
	if (!base) { /* no key: typed as itself from the symbols, or an edit */
		const char *l = s->d->letters[letter];
		return t->key == l[0] && !l[1] ? 0 : -1;
	}
	float kx, ky, lx, ly;
	if (!key_position(t->key, &kx, &ky))
		return t->key == base ? 0 : -1;
	key_position(base, &lx, &ly);
	float dx = kx + t->dx - lx, dy = ky + t->dy - ly;
	int cost = (int)(SPATIAL * (dx * dx + dy * dy) + 0.5f) - s->own[i];
	return cost > EDIT ? -1 : cost < 0 ? 0 : cost;
}

/* What a touch costs on the key it hit */
static int own_cost(const struct correct_touch *t)
{
	float x, y;
	if (!key_position(t->key, &x, &y))
		return 0;
	return (int)(SPATIAL * (t->dx * t->dx + t->dy * t->dy) + 0.5f);
}

static int freq_cost(const struct search *s, int freq)
{
	return (s->d->freq_max - freq) * 5 / 2;
}

/* The path, a word of that frequency, costing this much in slips and edits: among the
 * candidates if cheap enough in all, once */
static void candidate(struct search *s, int depth, int cost, int freq)
{
	cost += freq_cost(s, freq);
	if (cost >= s->bound)
		return;
	char text[CORRECT_WORD_MAX];
	size_t len = 0;
	for (int i = 0; i < depth; i++) {
		const char *l = s->d->letters[s->path[i]];
		size_t n = strlen(l);
		if (len + n >= sizeof(text))
			return;
		memcpy(text + len, l, n);
		len += n;
	}
	text[len] = 0;

	int at = s->found;
	for (int i = 0; i < s->found; i++)
		if (strcmp(s->out[i].text, text) == 0) { /* reached another way */
			if (cost >= s->out[i].cost)
				return;
			at = i;
			break;
		}
	bool added = at == s->found; /* not one already there, moved up */
	if (at == CORRECT_MAX)
		at--; /* in place of the worst */
	while (at > 0 && s->out[at - 1].cost > cost) {
		s->out[at] = s->out[at - 1];
		at--;
	}
	strcpy(s->out[at].text, text);
	s->out[at].cost = cost;
	if (added && s->found < CORRECT_MAX)
		s->found++;
	if (s->found == CORRECT_MAX)
		s->bound = s->out[CORRECT_MAX - 1].cost;
}

static void walk(struct search *s, uint32_t first, int depth, int i, int edits, int cost);

/* The node's letter taken as the word's depth-th: a word when every touch is used, and
 * on to its children */
static void took(struct search *s, uint32_t node, int depth, int i, int edits, int cost)
{
	if (cost >= CLOSE || cost >= s->bound || depth >= CORRECT_LETTERS_MAX)
		return;
	s->path[depth] = (uint8_t)LETTER(node);
	if (i == s->n && FREQ(node))
		candidate(s, depth + 1, cost, (int)FREQ(node));
	if (CHILD(node))
		walk(s, CHILD(node), depth + 1, i, edits, cost);
}

/* The children from `first` against the touches from the i-th */
static void walk(struct search *s, uint32_t first, int depth, int i, int edits, int cost)
{
	if (cost >= CLOSE || cost >= s->bound)
		return;
	bool edit = edits < s->max_edits;
	if (edit && i < s->n) /* a letter typed too many */
		walk(s, first, depth, i + 1, edits + 1,
		     cost + (i > 0 && s->t[i].key == s->t[i - 1].key ? DOUBLE : EDIT));
	for (uint32_t c = first;; c++) {
		uint32_t node = s->d->nodes[c];
		int letter = LETTER(node);
		if (i < s->n) {
			int sc = touch_cost(s, i, letter);
			if (sc >= 0)
				took(s, node, depth, i + 1, edits, cost + sc);
			else if (edit) /* a key far away */
				took(s, node, depth, i + 1, edits + 1, cost + EDIT);
			/* This letter and the next typed the other way round */
			int a;
			if (edit && i + 1 < s->n && CHILD(node) && (a = touch_cost(s, i + 1, letter)) >= 0 &&
			    depth + 1 < CORRECT_LETTERS_MAX) {
				for (uint32_t g = CHILD(node);; g++) {
					uint32_t next = s->d->nodes[g];
					int b = touch_cost(s, i, LETTER(next));
					if (b >= 0) {
						s->path[depth] = (uint8_t)letter;
						took(s, next, depth + 1, i + 2, edits + 1, cost + EDIT + a + b);
					}
					if (LAST(next))
						break;
				}
			}
		}
		if (edit) /* a letter missed */
			took(s, node, depth, i, edits + 1,
		     cost + (!s->d->base[letter] ? PUNCT : depth > 0 && s->path[depth - 1] == letter ? DOUBLE : EDIT));
		if (LAST(node))
			break;
	}
}

int correct(const struct correct_dict *d, const struct correct_touch *touches, int n,
            struct correct_word out[CORRECT_MAX])
{
	if (!d || n <= 0 || n > CORRECT_LETTERS_MAX)
		return 0;
	struct correct_touch t[CORRECT_LETTERS_MAX];
	for (int i = 0; i < n; i++) {
		t[i] = touches[i];
		t[i].key = (char)tolower((unsigned char)t[i].key);
	}
	/* One edit; two in a long word only when that finds nothing: the search with two
	 * costs twenty times as much, and a long word typed right is the common case */
	struct search s = { .d = d, .t = t, .n = n, .max_edits = 1, .out = out, .bound = INT_MAX };
	for (int i = 0; i < n; i++)
		s.own[i] = own_cost(&t[i]);
	walk(&s, 0, 0, 0, 0, 0);
	if (!s.found && n >= TWO_EDITS) {
		s.max_edits = 2;
		walk(&s, 0, 0, 0, 0, 0);
	}
	return s.found;
}

/* The frequency of the word in the trie, 0 for none */
static int lookup(const struct correct_dict *d, const char *word)
{
	if (!d || !*word)
		return 0;
	uint32_t first = 0, node = 0;
	while (*word) {
		int letter = -1;
		size_t len = 0;
		for (int i = 0; i < d->letter_count; i++) {
			len = strlen(d->letters[i]);
			if (strncmp(word, d->letters[i], len) == 0) {
				letter = i;
				break;
			}
		}
		if (letter < 0)
			return 0;
		word += len;
		for (uint32_t c = first;; c++) {
			node = d->nodes[c];
			if (LETTER(node) == (uint32_t)letter)
				break;
			if (LAST(node))
				return 0;
		}
		first = CHILD(node);
		if (!first && *word)
			return 0;
	}
	return (int)FREQ(node);
}

bool correct_known(const struct correct_dict *d, const char *word)
{
	return lookup(d, word) != 0;
}

int correct_cost(const struct correct_dict *d, const char *word)
{
	int freq = lookup(d, word);
	return freq ? (d->freq_max - freq) * 5 / 2 : -1;
}
