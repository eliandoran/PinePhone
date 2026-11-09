/*
 * Trilium: jump to note, see trilium.h. As Trilium's (its fast search, autocomplete.ts
 * and search_result.ts, simplified): each word typed must be in the note's title or in
 * the titles of its path, case and diacritics aside; the best MAX_RESULTS listed,
 * scored by the title as a whole (the same, starting so, a word of it) and by each word
 * (a whole word, its start, inside one), counted more in the title than in the path.
 * Each with where it is (its parent's path) under its title; Enter opens the first.
 * The notes are read once, as the screen opens; Trilium's own (the hidden subtree), the
 * deleted and the protected (their titles encrypted) are left out.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"

#define ROW_H 112
#define MAX_RESULTS 25 /* as Trilium's AUTOCOMPLETE_RESULT_LIMIT */
#define MAX_WORDS 8
#define MAX_DEPTH 64   /* a path deeper is a loop */

/* Trilium's SCORE_WEIGHTS, times 10 */
#define TITLE_EXACT 20000
#define TITLE_PREFIX 5000
#define TITLE_WORD 3000
#define TITLE_FACTOR 20
#define PATH_FACTOR 3
enum { WORD_CONTAINS = 1, WORD_PREFIX = 2, WORD_EXACT = 4 };

struct entry {
	char id[48];
	char *title, *folded;
	int parent;     /* its index, -1 for none */
	bool parent_hidden;
	long long parent_position;
	bool folder, protected, listed;
	char icon[8];  /* in tr_icon_font() */
	char *type, *mime, *icon_class; /* while loading */
};

struct result {
	int entry, score;
	char path[300];
};

struct jump {
	struct entry *entries;
	unsigned count;
	struct result results[MAX_RESULTS];
	unsigned found;
	lv_obj_t *ta, *kb, *card, *empty;
};

/* Folding: lower case, Latin letters without their diacritics */

/* U+00C0 to U+017F by their letter, '.' kept as they are */
static const char latin[] = "aaaaaaaceeeeiiii" "dnooooo.ouuuuy.s" "aaaaaaaceeeeiiii" "dnooooo.ouuuuy.y"
                            "aaaaaaccccccccdd" "ddeeeeeeeeeegggg" "gggghhhhiiiiiiii" "ii..jjkkklllllll"
                            "lllnnnnnnnnnoooo" "oooorrrrrrssssss" "ssttttttuuuuuuuu" "uuuuwwyyyzzzzzzs";

static char *fold(const char *s)
{
	size_t n = strlen(s);
	char *out = malloc(n + 1), *o = out;
	if (!out)
		return NULL;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p < 0x80) {
			*o++ = (char)tolower(*p++);
			continue;
		}
		if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
			unsigned cp = (unsigned)(*p & 0x1f) << 6 | (p[1] & 0x3f);
			char c = cp >= 0xc0 && cp <= 0x17f ? latin[cp - 0xc0] : cp == 0x218 || cp == 0x219 ? 's'
			         : cp == 0x21a || cp == 0x21b ? 't' : '.';
			if (c != '.') {
				*o++ = c;
				p += 2;
				continue;
			}
		}
		*o++ = (char)*p++;
	}
	*o = '\0';
	return out;
}

/* The notes */

static int entry_cmp(const void *key, const void *e)
{
	return strcmp(key, ((const struct entry *)e)->id);
}

/* Whether a note is in the tree a user sees: its path reaches the root, not through
 * Trilium's hidden notes */
static bool listed(const struct jump *j, unsigned i)
{
	for (int depth = 0, at = (int)i; depth < MAX_DEPTH && at >= 0; depth++) {
		const char *id = j->entries[at].id;
		if (!strcmp(id, "root")) /* NON-NLS */
			return depth > 0;
		if (!strcmp(id, "_hidden")) /* NON-NLS */
			return false;
		at = j->entries[at].parent;
	}
	return false;
}

static struct entry *entry_find(const struct jump *j, const char *id)
{
	return id ? bsearch(id, j->entries, j->count, sizeof(*j->entries), entry_cmp) : NULL;
}

/* Each note's parent (a note in several places by its first, outside the hidden notes)
 * and whether it has children, from the branches in one pass: a query asking each note's
 * took 80 s for 22,000 notes on a desktop (SQLite sorted the branches each time) */
static void branches_load(struct jump *j, sqlite3 *db)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT noteId, parentNoteId, notePosition FROM branches WHERE isDeleted = 0", /* NON-NLS */
	                       -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot read the branches: %s\n", sqlite3_errmsg(db));
		return;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		struct entry *child = entry_find(j, (const char *)sqlite3_column_text(st, 0));
		struct entry *parent = entry_find(j, (const char *)sqlite3_column_text(st, 1));
		if (!child || !parent)
			continue;
		parent->folder = true;
		bool hidden = !strcmp(parent->id, "_hidden"); /* NON-NLS */
		long long position = sqlite3_column_int64(st, 2);
		if (child->parent < 0 || child->parent_hidden > hidden ||
		    (child->parent_hidden == hidden && position < child->parent_position)) {
			child->parent = (int)(parent - j->entries);
			child->parent_hidden = hidden;
			child->parent_position = position;
		}
	}
	sqlite3_finalize(st);
}

/* Each note's own icon (#iconClass), from the labels in one pass: its first */
static void icon_classes_load(struct jump *j, sqlite3 *db)
{
	/* NON-NLS-BEGIN: SQL */
	static const char sql[] = "SELECT noteId, value FROM attributes WHERE type = 'label' AND name = 'iconClass'"
	                          " AND isDeleted = 0 ORDER BY position";
	/* NON-NLS-END */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot read the icons: %s\n", sqlite3_errmsg(db));
		return;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		struct entry *e = entry_find(j, (const char *)sqlite3_column_text(st, 0));
		const char *value = (const char *)sqlite3_column_text(st, 1);
		if (e && !e->icon_class && value)
			e->icon_class = strdup(value);
	}
	sqlite3_finalize(st);
}

static bool notes_load(struct jump *j, sqlite3 *db)
{
	/* By their ID, to find them in */
	static const char sql[] = "SELECT noteId, title, isProtected, type, mime FROM notes WHERE isDeleted = 0" /* NON-NLS */
	                          " ORDER BY noteId"; /* NON-NLS */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "trilium: cannot read the notes: %s\n", sqlite3_errmsg(db));
		return false;
	}
	unsigned size = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (j->count == size) {
			unsigned more = size ? size * 2 : 1024;
			struct entry *entries = realloc(j->entries, more * sizeof(*entries));
			if (!entries)
				break;
			j->entries = entries;
			size = more;
		}
		struct entry *e = &j->entries[j->count];
		memset(e, 0, sizeof(*e));
		const char *id = (const char *)sqlite3_column_text(st, 0), *title = (const char *)sqlite3_column_text(st, 1);
		const char *type = (const char *)sqlite3_column_text(st, 3), *mime = (const char *)sqlite3_column_text(st, 4);
		snprintf(e->id, sizeof(e->id), "%s", id ? id : "");
		e->title = strdup(title ? title : "");
		e->folded = e->title ? fold(e->title) : NULL;
		if (!e->folded) { /* out of memory: left out */
			free(e->title);
			continue;
		}
		e->protected = sqlite3_column_int(st, 2);
		e->type = strdup(type ? type : "");
		e->mime = strdup(mime ? mime : "");
		e->parent = -1;
		j->count++;
	}
	sqlite3_finalize(st);
	branches_load(j, db);
	icon_classes_load(j, db);
	for (unsigned i = 0; i < j->count; i++) {
		struct entry *e = &j->entries[i];
		e->listed = !e->protected && listed(j, i);
		tr_note_icon(e->icon, e->icon_class, e->type, e->mime, e->folder);
		free(e->type);
		free(e->mime);
		free(e->icon_class);
		e->type = e->mime = e->icon_class = NULL;
	}
	return true;
}

/* Matching */

static bool word_char(char c)
{
	return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

/* How `word` is in `text`: WORD_EXACT, _PREFIX, _CONTAINS, 0 not */
static int word_match(const char *text, const char *word)
{
	int best = 0;
	size_t n = strlen(word);
	for (const char *p = strstr(text, word); p; p = strstr(p + 1, word)) {
		bool start = p == text || !word_char(p[-1]);
		int m = !start ? WORD_CONTAINS : word_char(p[n]) ? WORD_PREFIX : WORD_EXACT;
		if (m > best)
			best = m;
		if (best == WORD_EXACT)
			break;
	}
	return best;
}

/* The note's score for the folded query and its words, 0 if one is missing */
static int score(const struct jump *j, const struct entry *e, const char *query, char **words, unsigned n)
{
	int s = 0;
	if (!strcmp(e->folded, query))
		s += TITLE_EXACT;
	else if (!strncmp(e->folded, query, strlen(query)))
		s += TITLE_PREFIX;
	else if (word_match(e->folded, query) == WORD_EXACT)
		s += TITLE_WORD;
	for (unsigned w = 0; w < n; w++) {
		int m = word_match(e->folded, words[w]);
		if (m) {
			s += m * TITLE_FACTOR;
			continue;
		}
		/* Its path's, the root's aside */
		for (int depth = 0, at = e->parent; depth < MAX_DEPTH && at >= 0 && j->entries[at].parent >= 0;
		     depth++, at = j->entries[at].parent) {
			int pm = word_match(j->entries[at].folded, words[w]);
			if (pm > m)
				m = pm;
		}
		if (!m)
			return 0;
		s += m * PATH_FACTOR;
	}
	return s;
}

/* Where the note is: its ancestors' titles, the root's aside, "A › B"; deeper, the last
 * two after an ellipsis (its parent is what tells it apart, and a row's line is short) */
static void path_text(const struct jump *j, const struct entry *e, char *out, size_t size)
{
	const struct entry *chain[MAX_DEPTH];
	unsigned n = 0;
	for (int at = e->parent; at >= 0 && j->entries[at].parent >= 0 && n < MAX_DEPTH; at = j->entries[at].parent)
		chain[n++] = &j->entries[at];
	size_t len = 0;
	out[0] = '\0';
	if (n > 2) {
		len = (size_t)snprintf(out, size, "\xe2\x80\xa6");
		n = 2;
	}
	while (n-- > 0 && len < size)
		len += (size_t)snprintf(out + len, size - len, "%s%s", len ? " \xe2\x80\xba " : "", chain[n]->title);
}

static void search(struct jump *j)
{
	char *query = fold(lv_textarea_get_text(j->ta));
	j->found = 0;
	char *words[MAX_WORDS];
	unsigned n = 0;
	if (query) {
		/* The whole query trimmed, for the title's scores; its words */
		char *q = query + strspn(query, " ");
		size_t len = strlen(q);
		while (len && q[len - 1] == ' ')
			q[--len] = '\0';
		char *copy = strdup(q), *save = NULL;
		for (char *w = copy ? strtok_r(copy, " ", &save) : NULL; w && n < MAX_WORDS; w = strtok_r(NULL, " ", &save))
			words[n++] = w;
		for (unsigned i = 0; n && i < j->count; i++) {
			const struct entry *e = &j->entries[i];
			int s = e->listed ? score(j, e, q, words, n) : 0;
			if (!s)
				continue;
			/* Kept in order, best first: a better score, or as good with a shorter title */
			unsigned at = j->found;
			while (at > 0) {
				const struct result *r = &j->results[at - 1];
				size_t rl = strlen(j->entries[r->entry].title), el = strlen(e->title);
				if (r->score > s || (r->score == s && rl <= el))
					break;
				at--;
			}
			if (at >= MAX_RESULTS)
				continue;
			unsigned last = j->found < MAX_RESULTS ? j->found : MAX_RESULTS - 1;
			memmove(&j->results[at + 1], &j->results[at], (last - at) * sizeof(j->results[0]));
			j->results[at].entry = (int)i;
			j->results[at].score = s;
			if (j->found < MAX_RESULTS)
				j->found++;
		}
		free(copy);
	}
	for (unsigned i = 0; i < j->found; i++)
		path_text(j, &j->entries[j->results[i].entry], j->results[i].path, sizeof(j->results[i].path));
	ui_list_set_count(j->card, j->found);
	lv_obj_set_hidden(j->card, !j->found);
	lv_obj_set_hidden(j->empty, j->found || !n);
	free(query);
}

/* The screen */

static void result_bind(lv_obj_t *row, unsigned index, void *user_data)
{
	const struct jump *j = user_data;
	const struct result *r = &j->results[index];
	const struct entry *e = &j->entries[r->entry];
	ui_list_row_set(row, e->icon, e->folder ? UI_COLOR_ACCENT : UI_COLOR_DIM, e->title, r->path, e->folder);
	ui_list_row_set_symbol_font(row, tr_icon_font());
}

static void result_click(unsigned index, void *user_data)
{
	const struct jump *j = user_data;
	if (index >= j->found)
		return;
	const struct entry *e = &j->entries[j->results[index].entry];
	ui_keyboard_show(j->kb, false);
	if (e->folder)
		tr_tree_open(e->id, e->title);
	else
		tr_note_open(e->id, e->title);
}

static void ta_cb(lv_event_t *e)
{
	struct jump *j = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(j->kb, true);
		break;
	case LV_EVENT_READY:
		result_click(0, j);
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(j->kb, false);
		break;
	case LV_EVENT_VALUE_CHANGED:
		search(j);
		break;
	default:
		break;
	}
}

static void delete_cb(lv_event_t *e)
{
	struct jump *j = lv_event_get_user_data(e);
	for (unsigned i = 0; i < j->count; i++) {
		free(j->entries[i].title);
		free(j->entries[i].folded);
	}
	free(j->entries);
	free(j);
}

void tr_jump_open(void)
{
	struct jump *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	uint32_t start = lv_tick_get();
	sqlite3 *db = tr_db_open();
	if (db && notes_load(j, db))
		fprintf(stderr, "trilium: %u notes read in %u ms\n", j->count, lv_tick_elaps(start));
	sqlite3_close(db);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Jump to note"), NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, j);
	lv_obj_set_scrollable(body, false);

	lv_obj_t *field = ui_box(body);
	lv_obj_set_size(field, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(field, 16, 0);
	lv_obj_set_style_pad_bottom(field, 12, 0);
	j->ta = ui_text_field(field, 88, 200, ta_cb, j);
	lv_obj_add_event_cb(j->ta, ta_cb, LV_EVENT_VALUE_CHANGED, j);
	lv_textarea_set_placeholder_text(j->ta, _("Note title"));
	lv_obj_set_width(j->ta, LV_PCT(100));

	lv_obj_t *list = ui_box(body);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 16, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_clickable(list, true); /* dragged beside the rows too */
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	j->card = ui_list_create(list, ROW_H, ui_list_row_make, result_bind, result_click, j);
	lv_obj_set_hidden(j->card, true);
	j->empty = ui_empty_label(list, db ? _("No notes found") : _("The database cannot be opened"));
	lv_obj_set_style_text_font(j->empty, UI_FONT_BODY, 0);
	lv_obj_set_hidden(j->empty, db != NULL);

	j->kb = ui_keyboard_create(body, j->ta);
	ui_keyboard_show(j->kb, true);
	ui_push(scr);
}
