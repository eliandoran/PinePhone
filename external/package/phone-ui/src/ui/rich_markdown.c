/*
 * Rich text as Markdown (rich_text.h): a block a line, as a note's plain text was, rather
 * than Markdown's paragraphs (which a single newline does not end):
 *
 * - inline: **bold**, *italic*, <u>underline</u> (Markdown has none: HTML's, as Trilium
 *   writes it), opened outside in (underline, bold, italic); spaces at a marked run's ends
 *   outside its delimiters, as CommonMark wants them
 * - lists: "- " a bullet item, "1. " a numbered one (numbered again from 1 when written),
 *   "- [ ] " and "- [x] " a to-do item
 * - what would read as Markdown escaped with a backslash: \ and * anywhere, <u> and </u>,
 *   and at a line's start what makes a list, a heading or a quote
 *
 * Read leniently, for notes written elsewhere: "*" and "+" bullets too, "1)", "[X]";
 * a delimiter nothing closes on its line is text.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rich_text.h"

#define MARKS_ORDER { RICH_UNDERLINE, RICH_BOLD, RICH_ITALIC } /* outside in */

struct buf {
	char *s;
	size_t len, cap;
};

static void put(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->s = realloc(b->s, b->cap);
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void puts_(struct buf *b, const char *s)
{
	put(b, s, strlen(s));
}

/* Writing */

static const char *delimiter(unsigned mark, bool open)
{
	switch (mark) {
	case RICH_UNDERLINE:
		return open ? "<u>" : "</u>";
	case RICH_BOLD:
		return "**";
	default:
		return "*";
	}
}

/* The marks open, outside in */
struct open {
	unsigned marks[3];
	int count;
};

/* From the marks open to `marks`: those inside the first to go closed, then opened again
 * if still wanted, with the new ones */
static void switch_marks(struct buf *b, struct open *o, unsigned marks)
{
	int keep = 0;
	while (keep < o->count && (marks & o->marks[keep]))
		keep++;
	while (o->count > keep)
		puts_(b, delimiter(o->marks[--o->count], false));
	unsigned open = 0;
	for (int i = 0; i < o->count; i++)
		open |= o->marks[i];
	static const unsigned order[] = MARKS_ORDER;
	for (int i = 0; i < 3; i++) {
		if ((marks & order[i]) && !(open & order[i])) {
			puts_(b, delimiter(order[i], true));
			o->marks[o->count++] = order[i];
		}
	}
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t';
}

/* Where text at a line's start would read as Markdown: the offset to put a backslash
 * before, -1 for none. `item`: after a list's marker (a to-do's box) rather than a
 * paragraph (a list, a heading, a quote). */
static long line_start_escape(const char *t, uint32_t len, bool item)
{
	if (item)
		return len >= 3 && t[0] == '[' && (t[1] == ' ' || t[1] == 'x' || t[1] == 'X') && t[2] == ']' ? 0 : -1;
	if (len && (t[0] == '#' || t[0] == '>'))
		return 0;
	if (len && (t[0] == '-' || t[0] == '+') && (len == 1 || t[1] == ' '))
		return 0;
	uint32_t d = 0;
	while (d < len && isdigit((unsigned char)t[d]))
		d++;
	if (d && d < len && (t[d] == '.' || t[d] == ')') && (d + 1 == len || t[d + 1] == ' '))
		return d;
	return -1;
}

static void write_inline(struct buf *out, const struct rich_block *b, bool item)
{
	struct open o = { .count = 0 };
	long esc = line_start_escape(b->text, b->len, item);
	uint32_t at = 0;
	for (uint32_t r = 0; r < b->run_count; r++) {
		const struct rich_run *run = &b->runs[r];
		unsigned prev = r ? b->runs[r - 1].marks : 0, next = r + 1 < b->run_count ? b->runs[r + 1].marks : 0;
		/* Its spaces at either end: in the marks it shares with the run on that side */
		uint32_t lead = 0, trail = 0;
		while (lead < run->len && is_space(b->text[at + lead]))
			lead++;
		while (trail < run->len - lead && is_space(b->text[at + run->len - 1 - trail]))
			trail++;
		for (uint32_t i = 0; i < run->len; i++) {
			uint32_t pos = at + i;
			unsigned marks = run->marks;
			if (lead == run->len)
				marks &= prev & next;
			else if (i < lead)
				marks &= prev;
			else if (i >= run->len - trail)
				marks &= next;
			switch_marks(out, &o, marks);
			char c = b->text[pos];
			if ((long)pos == esc || c == '\\' || c == '*' ||
			    (c == '<' && (!strncmp(b->text + pos, "<u>", 3) || !strncmp(b->text + pos, "</u>", 4))))
				puts_(out, "\\");
			put(out, &c, 1);
		}
		at += run->len;
	}
	switch_marks(out, &o, 0);
}

char *rich_to_markdown(const struct rich_doc *doc)
{
	struct buf out = { 0 };
	puts_(&out, "");
	uint32_t number = 0;
	for (uint32_t bi = 0; bi < doc->count; bi++) {
		const struct rich_block *b = &doc->blocks[bi];
		if (bi)
			puts_(&out, "\n");
		number = b->type == RICH_NUMBERED ? number + 1 : 0;
		char prefix[24] = "";
		switch (b->type) {
		case RICH_BULLET:
			strcpy(prefix, "- ");
			break;
		case RICH_NUMBERED:
			snprintf(prefix, sizeof(prefix), "%u. ", (unsigned)number);
			break;
		case RICH_TODO:
			strcpy(prefix, b->checked ? "- [x] " : "- [ ] ");
			break;
		case RICH_PARAGRAPH:
			break;
		}
		puts_(&out, prefix);
		write_inline(&out, b, b->type != RICH_PARAGRAPH);
	}
	return out.s;
}

/* Reading */

/* A line's list marker: its type (RICH_PARAGRAPH for none) and how long it is */
static enum rich_block_type line_type(const char *s, size_t len, size_t *skip, bool *checked)
{
	*skip = 0;
	*checked = false;
	size_t n = 0;
	if (len && (s[0] == '-' || s[0] == '*' || s[0] == '+') && (len == 1 || s[1] == ' ')) {
		n = len == 1 ? 1 : 2;
		if (len >= n + 3 && s[n] == '[' && (s[n + 1] == ' ' || s[n + 1] == 'x' || s[n + 1] == 'X') &&
		    s[n + 2] == ']' && (len == n + 3 || s[n + 3] == ' ')) {
			*checked = s[n + 1] != ' ';
			*skip = len == n + 3 ? len : n + 4;
			return RICH_TODO;
		}
		*skip = n;
		return RICH_BULLET;
	}
	while (n < len && isdigit((unsigned char)s[n]))
		n++;
	if (n && n < len && (s[n] == '.' || s[n] == ')') && (n + 1 == len || s[n + 1] == ' ')) {
		*skip = n + 1 == len ? len : n + 2;
		return RICH_NUMBERED;
	}
	return RICH_PARAGRAPH;
}

/* A piece of a line: text (escapes undone) or a delimiter */
struct piece {
	const char *s;
	size_t len;
	unsigned marks;     /* a delimiter's: what it toggles */
	unsigned paired;    /* of those, what another closes or opens */
	bool open, escaped; /* <u> rather than </u>; text after a backslash */
};

struct pieces {
	struct piece *p;
	size_t count, cap;
};

static void add(struct pieces *ps, struct piece p)
{
	if (ps->count == ps->cap) {
		ps->cap = ps->cap ? ps->cap * 2 : 16;
		ps->p = realloc(ps->p, ps->cap * sizeof(*ps->p));
	}
	ps->p[ps->count++] = p;
}

static void split_line(struct pieces *ps, const char *s, size_t len)
{
	size_t i = 0;
	while (i < len) {
		if (s[i] == '\\' && i + 1 < len && ispunct((unsigned char)s[i + 1])) {
			add(ps, (struct piece){ .s = s + i + 1, .len = 1, .escaped = true });
			i += 2;
		} else if (s[i] == '*') {
			size_t n = 1;
			while (i + n < len && s[i + n] == '*')
				n++;
			unsigned marks = n == 1 ? RICH_ITALIC : n == 2 ? RICH_BOLD : n == 3 ? RICH_BOLD | RICH_ITALIC : 0;
			add(ps, (struct piece){ .s = s + i, .len = n, .marks = marks });
			i += n;
		} else if (len - i >= 3 && !strncmp(s + i, "<u>", 3)) {
			add(ps, (struct piece){ .s = s + i, .len = 3, .marks = RICH_UNDERLINE, .open = true });
			i += 3;
		} else if (len - i >= 4 && !strncmp(s + i, "</u>", 4)) {
			add(ps, (struct piece){ .s = s + i, .len = 4, .marks = RICH_UNDERLINE });
			i += 4;
		} else {
			size_t n = 1;
			while (i + n < len && !strchr("\\*<", s[i + n]))
				n++;
			add(ps, (struct piece){ .s = s + i, .len = n });
			i += n;
		}
	}
}

/* The delimiters that close one before them: * and ** in pairs, </u> after a <u> */
static void pair(struct pieces *ps)
{
	static const unsigned toggles[] = { RICH_BOLD, RICH_ITALIC };
	for (int t = 0; t < 2; t++) {
		size_t opener = SIZE_MAX;
		for (size_t i = 0; i < ps->count; i++) {
			if (!(ps->p[i].marks & toggles[t]))
				continue;
			if (opener == SIZE_MAX) {
				opener = i;
			} else {
				ps->p[opener].paired |= toggles[t];
				ps->p[i].paired |= toggles[t];
				opener = SIZE_MAX;
			}
		}
	}
	size_t opener = SIZE_MAX;
	for (size_t i = 0; i < ps->count; i++) {
		if (ps->p[i].marks != RICH_UNDERLINE)
			continue;
		if (ps->p[i].open) {
			opener = i;
		} else if (opener != SIZE_MAX) {
			ps->p[opener].paired = ps->p[i].paired = RICH_UNDERLINE;
			opener = SIZE_MAX;
		}
	}
}

struct rich_doc *rich_from_markdown(const char *md)
{
	struct rich_doc *doc = rich_doc_new("");
	struct pieces ps = { 0 };
	uint32_t end = 0; /* the document's length */
	for (const char *line = md;;) {
		size_t len = strcspn(line, "\n"), skip;
		bool checked;
		enum rich_block_type type = line_type(line, len, &skip, &checked);
		rich_set_type(doc, end, end, type);
		rich_set_checked(doc, end, checked);

		ps.count = 0;
		split_line(&ps, line + skip, len - skip);
		pair(&ps);
		unsigned marks = 0;
		for (size_t i = 0; i < ps.count; i++) {
			const struct piece *p = &ps.p[i];
			/* What is not paired is text: a lone *, the * of a *** whose bold is, ****... */
			const char *text = p->s;
			size_t n = p->len;
			if (p->marks && !p->escaped) {
				if (p->marks == RICH_UNDERLINE) {
					n = p->paired ? 0 : n;
				} else {
					n = ((p->marks & ~p->paired & RICH_BOLD) ? 2 : 0) + ((p->marks & ~p->paired & RICH_ITALIC) ? 1 : 0);
					n = p->len > 3 ? p->len : n;
				}
				marks ^= p->paired;
			}
			if (n)
				rich_insert(doc, end, text, (uint32_t)n, marks);
			end += (uint32_t)n;
		}
		if (!line[len])
			break;
		rich_insert(doc, end++, "\n", 1, 0);
		line += len + 1;
	}
	free(ps.p);
	return doc;
}
