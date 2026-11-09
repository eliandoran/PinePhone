/*
 * Trilium: a text note's HTML (its editor's, CKEditor 5's) to the rich text editor's
 * document and back, see trilium.h. Only what the document can hold without losing
 * anything is read: paragraphs, bullet, numbered and to-do lists (not nested), bold,
 * italic and underline, plain spans, entities; whitespace as HTML collapses it (an older
 * note's indented HTML reads the same). Anything else (a link, a heading, a style, a line
 * break...) and the note is not read: it stays as it is, shown but not edited. Written
 * back as CKEditor writes it: <p>, <ul>/<ol> of <li>, a to-do list's label and checkbox,
 * <strong>, <i>, <u>, &nbsp; for an empty block.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "trilium.h"

/* NON-NLS-BEGIN: HTML */

/* The document as it is read: its text (blocks joined by '\n') and each byte's marks */
struct reader {
	char *text;
	unsigned char *marks;
	size_t len, cap;
	enum rich_block_type *types;
	bool *checked;
	unsigned blocks, blocks_cap;
	bool in_block;
	size_t block_at; /* where the block being read starts */
	enum rich_block_type list; /* the list open, RICH_PARAGRAPH none */
	bool in_list, in_item, in_label, in_description;
	int bold, italic, underline; /* open tags of each */
	bool ok;
};

static void put(struct reader *r, char c)
{
	if (r->len + 1 >= r->cap) {
		size_t cap = r->cap ? r->cap * 2 : 256;
		char *text = realloc(r->text, cap);
		unsigned char *marks = text ? realloc(r->marks, cap) : NULL;
		if (text)
			r->text = text;
		if (!marks) {
			r->ok = false;
			return;
		}
		r->marks = marks;
		r->cap = cap;
	}
	r->marks[r->len] = (unsigned char)((r->bold ? RICH_BOLD : 0) | (r->italic ? RICH_ITALIC : 0) |
	                                   (r->underline ? RICH_UNDERLINE : 0));
	r->text[r->len++] = c;
}

static void block_start(struct reader *r, enum rich_block_type type)
{
	if (r->in_block) {
		r->ok = false;
		return;
	}
	if (r->blocks == r->blocks_cap) {
		unsigned cap = r->blocks_cap ? r->blocks_cap * 2 : 16;
		enum rich_block_type *types = realloc(r->types, cap * sizeof(*types));
		bool *checked = types ? realloc(r->checked, cap * sizeof(*checked)) : NULL;
		if (types)
			r->types = types;
		if (!checked) {
			r->ok = false;
			return;
		}
		r->checked = checked;
		r->blocks_cap = cap;
	}
	if (r->blocks)
		put(r, '\n');
	r->types[r->blocks] = type;
	r->checked[r->blocks++] = false;
	r->block_at = r->len;
	r->in_block = true;
}

static void block_end(struct reader *r)
{
	if (!r->in_block) {
		r->ok = false;
		return;
	}
	size_t from = r->block_at;
	/* Its last space collapsed away, as HTML does; an empty block written as &nbsp; */
	if (r->len > from && r->text[r->len - 1] == ' ')
		r->len--;
	if (r->len - from == 2 && !memcmp(r->text + from, "\xc2\xa0", 2))
		r->len = from;
	r->in_block = false;
}

static void text(struct reader *r, const char *s, size_t n)
{
	for (size_t i = 0; i < n && r->ok; i++) {
		char c = s[i];
		bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
		if (!r->in_block || (r->list == RICH_TODO && r->in_item && !r->in_description)) {
			/* Between blocks, and around a to-do item's parts, only layout */
			if (!space)
				r->ok = false;
			continue;
		}
		if (space) {
			/* Collapsed: one space, none at the block's start */
			if (r->len > r->block_at && r->text[r->len - 1] != ' ')
				put(r, ' ');
			continue;
		}
		if (c == '&') {
			const char *end = memchr(s + i, ';', n - i);
			if (!end || end - (s + i) > 10) {
				r->ok = false;
				return;
			}
			static const struct { const char *name, *text; } entities[] = {
				{"&nbsp;", "\xc2\xa0"}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
				{"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&#160;", "\xc2\xa0"},
			};
			size_t len = (size_t)(end - (s + i)) + 1;
			const char *out = NULL;
			for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++)
				if (strlen(entities[e].name) == len && !memcmp(entities[e].name, s + i, len))
					out = entities[e].text;
			if (!out) {
				r->ok = false;
				return;
			}
			for (const char *o = out; *o; o++)
				put(r, *o);
			i += len - 1;
			continue;
		}
		put(r, c);
	}
}

/* A tag's attributes, `<name attrs>`: whether they are exactly those of `allowed`
 * (name="value" pairs, any order; a NULL value any of an attribute that may be left
 * out), each at most once */
static bool attributes(const char *attrs, size_t n, const char *const allowed[][2], size_t count, unsigned *seen)
{
	*seen = 0;
	size_t i = 0;
	while (i < n) {
		while (i < n && (attrs[i] == ' ' || attrs[i] == '\n' || attrs[i] == '\t' || attrs[i] == '\r' || attrs[i] == '/'))
			i++;
		if (i == n)
			break;
		size_t name = i;
		while (i < n && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '/')
			i++;
		size_t name_len = i - name;
		const char *value = "";
		size_t value_len = 0;
		if (i < n && attrs[i] == '=') {
			i++;
			if (i >= n || attrs[i] != '"')
				return false;
			value = attrs + ++i;
			while (i < n && attrs[i] != '"')
				i++;
			if (i == n)
				return false;
			value_len = (size_t)(attrs + i - value);
			i++;
		}
		bool known = false;
		for (size_t a = 0; a < count && !known; a++)
			if (strlen(allowed[a][0]) == name_len && !memcmp(allowed[a][0], attrs + name, name_len) &&
			    (!allowed[a][1] || (strlen(allowed[a][1]) == value_len && !memcmp(allowed[a][1], value, value_len)))) {
				if (*seen & 1u << a)
					return false;
				*seen |= 1u << a;
				known = true;
			}
		if (!known)
			return false;
	}
	return true;
}

static bool no_attributes(const char *attrs, size_t n)
{
	unsigned seen;
	return attributes(attrs, n, NULL, 0, &seen);
}

static void open_tag(struct reader *r, const char *name, const char *attrs, size_t n)
{
	bool plain = no_attributes(attrs, n);
	if (!strcasecmp(name, "p") && plain && !r->in_list)
		block_start(r, RICH_PARAGRAPH);
	else if ((!strcasecmp(name, "ul") || !strcasecmp(name, "ol")) && !r->in_list && !r->in_block) {
		static const char *const todo[][2] = {{"class", "todo-list"}};
		unsigned seen;
		if (plain)
			r->list = tolower(name[0]) == 'o' ? RICH_NUMBERED : RICH_BULLET;
		else if (tolower(name[0]) == 'u' && attributes(attrs, n, todo, 1, &seen))
			r->list = RICH_TODO;
		else
			r->ok = false;
		r->in_list = true;
	} else if (!strcasecmp(name, "li") && plain && r->in_list && !r->in_item) {
		r->in_item = true;
		block_start(r, r->list);
	} else if (!strcasecmp(name, "label") && r->list == RICH_TODO && r->in_item && !r->in_label) {
		static const char *const label[][2] = {{"class", "todo-list__label"}};
		unsigned seen;
		r->in_label = attributes(attrs, n, label, 1, &seen) && seen == 1;
		r->ok = r->in_label;
	} else if (!strcasecmp(name, "input") && r->in_label && !r->in_description) {
		static const char *const input[][2] = {{"type", "checkbox"}, {"disabled", "disabled"},
		                                       {"checked", "checked"}, {"class", "todo-list__label__checkmark"}};
		unsigned seen;
		r->ok = attributes(attrs, n, input, 4, &seen) && (seen & 3) == 3;
		if (r->ok)
			r->checked[r->blocks - 1] = seen & 4;
	} else if (!strcasecmp(name, "span") && r->in_label && !r->in_description) {
		static const char *const description[][2] = {{"class", "todo-list__label__description"}};
		unsigned seen;
		r->in_description = attributes(attrs, n, description, 1, &seen) && seen == 1;
		r->ok = r->in_description;
	} else if (r->in_block && plain && (r->list != RICH_TODO || r->in_description) &&
	           (!strcasecmp(name, "strong") || !strcasecmp(name, "b")))
		r->bold++;
	else if (r->in_block && plain && (r->list != RICH_TODO || r->in_description) &&
	         (!strcasecmp(name, "i") || !strcasecmp(name, "em")))
		r->italic++;
	else if (r->in_block && plain && (r->list != RICH_TODO || r->in_description) && !strcasecmp(name, "u"))
		r->underline++;
	else if (r->in_block && plain && (r->list != RICH_TODO || r->in_description) && !strcasecmp(name, "span"))
		; /* nothing to it */
	else
		r->ok = false;
}

static void close_tag(struct reader *r, const char *name)
{
	if (!strcasecmp(name, "p") && !r->in_list)
		block_end(r);
	else if ((!strcasecmp(name, "ul") || !strcasecmp(name, "ol")) && r->in_list && !r->in_item) {
		r->in_list = false;
		r->list = RICH_PARAGRAPH;
	} else if (!strcasecmp(name, "li") && r->in_item && !r->in_label) {
		r->in_item = false;
		block_end(r);
	} else if (!strcasecmp(name, "label") && r->in_label && !r->in_description)
		r->in_label = false;
	else if (!strcasecmp(name, "span") && r->in_description && !r->bold && !r->italic && !r->underline)
		r->in_description = false;
	else if ((!strcasecmp(name, "strong") || !strcasecmp(name, "b")) && r->bold)
		r->bold--;
	else if ((!strcasecmp(name, "i") || !strcasecmp(name, "em")) && r->italic)
		r->italic--;
	else if (!strcasecmp(name, "u") && r->underline)
		r->underline--;
	else if (!strcasecmp(name, "span") && r->in_block)
		; /* a plain span's */
	else
		r->ok = false;
}

/* The document from what was read */
static struct rich_doc *doc_make(struct reader *r)
{
	put(r, '\0');
	if (!r->ok)
		return NULL;
	struct rich_doc *doc = rich_doc_new(r->text);
	if (!doc)
		return NULL;
	size_t len = r->len - 1;
	for (unsigned b = 0; b < r->blocks; b++) {
		uint32_t pos = rich_block_pos(doc, b);
		if (r->types[b] != RICH_PARAGRAPH)
			rich_set_type(doc, pos, pos, r->types[b]);
		if (r->checked[b])
			rich_set_checked(doc, pos, true);
	}
	for (size_t i = 0; i < len;) {
		size_t j = i;
		while (j < len && r->marks[j] == r->marks[i] && r->text[j] != '\n')
			j++;
		if (j == i) {
			i++;
			continue;
		}
		if (r->marks[i])
			rich_set_marks(doc, (uint32_t)i, (uint32_t)j, r->marks[i], true);
		i = j;
	}
	return doc;
}

struct rich_doc *tr_html_to_doc(const char *html)
{
	struct reader r = {.ok = true, .list = RICH_PARAGRAPH};
	const char *p = html;
	while (*p && r.ok) {
		if (*p != '<') {
			const char *lt = strchr(p, '<');
			size_t n = lt ? (size_t)(lt - p) : strlen(p);
			text(&r, p, n);
			p += n;
			continue;
		}
		const char *gt = strchr(p, '>');
		if (!gt || p[1] == '!') { /* a comment, a doctype */
			r.ok = false;
			break;
		}
		bool closing = p[1] == '/';
		const char *name = p + 1 + closing;
		size_t name_len = strcspn(name, " \t\r\n/>");
		char tag[16];
		if (!name_len || name_len >= sizeof(tag)) {
			r.ok = false;
			break;
		}
		memcpy(tag, name, name_len);
		tag[name_len] = '\0';
		if (closing)
			close_tag(&r, tag);
		else
			open_tag(&r, tag, name + name_len, (size_t)(gt - (name + name_len)));
		p = gt + 1;
	}
	/* All closed */
	if (r.in_block || r.in_list || r.bold || r.italic || r.underline)
		r.ok = false;
	struct rich_doc *doc = doc_make(&r);
	free(r.text);
	free(r.marks);
	free(r.types);
	free(r.checked);
	return doc;
}

/* Writing */

struct writer {
	char *out;
	size_t len, cap;
	bool ok;
};

static void write_bytes(struct writer *w, const char *s, size_t n)
{
	if (w->len + n + 1 > w->cap) {
		size_t cap = w->cap ? w->cap : 256;
		while (w->len + n + 1 > cap)
			cap *= 2;
		char *out = realloc(w->out, cap);
		if (!out) {
			w->ok = false;
			return;
		}
		w->out = out;
		w->cap = cap;
	}
	memcpy(w->out + w->len, s, n);
	w->len += n;
	w->out[w->len] = '\0';
}

static void write_str(struct writer *w, const char *s)
{
	write_bytes(w, s, strlen(s));
}

static void write_text(struct writer *w, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '&')
			write_str(w, "&amp;");
		else if (s[i] == '<')
			write_str(w, "&lt;");
		else if (s[i] == '>')
			write_str(w, "&gt;");
		else if (i + 1 < n && !memcmp(s + i, "\xc2\xa0", 2)) {
			write_str(w, "&nbsp;");
			i++;
		} else
			write_bytes(w, s + i, 1);
	}
}

/* A block's text with its marks, nested as CKEditor nests them: bold, italic, underline */
static void write_inline(struct writer *w, const struct rich_block *b)
{
	static const struct { unsigned mark; const char *open, *close; } tags[] = {
		{RICH_BOLD, "<strong>", "</strong>"}, {RICH_ITALIC, "<i>", "</i>"}, {RICH_UNDERLINE, "<u>", "</u>"},
	};
	if (!b->len) {
		write_str(w, "&nbsp;");
		return;
	}
	unsigned open[3], depth = 0, on = 0;
	uint32_t at = 0;
	for (uint32_t i = 0; i < b->run_count; i++) {
		unsigned marks = b->runs[i].marks;
		/* Closed down to what this run keeps, then what it adds opened */
		while (depth && (on & ~marks)) {
			unsigned t = open[--depth];
			write_str(w, tags[t].close);
			on &= ~tags[t].mark;
		}
		for (unsigned t = 0; t < 3; t++)
			if ((marks & tags[t].mark) && !(on & tags[t].mark)) {
				write_str(w, tags[t].open);
				open[depth++] = t;
				on |= tags[t].mark;
			}
		write_text(w, b->text + at, b->runs[i].len);
		at += b->runs[i].len;
	}
	while (depth)
		write_str(w, tags[open[--depth]].close);
}

char *tr_html_from_doc(const struct rich_doc *doc)
{
	struct writer w = {.ok = true};
	write_str(&w, "");
	enum rich_block_type list = RICH_PARAGRAPH;
	for (uint32_t i = 0; i < doc->count; i++) {
		const struct rich_block *b = &doc->blocks[i];
		if (list != b->type && list != RICH_PARAGRAPH)
			write_str(&w, list == RICH_NUMBERED ? "</ol>" : "</ul>");
		if (list != b->type && b->type != RICH_PARAGRAPH)
			write_str(&w, b->type == RICH_NUMBERED ? "<ol>" : b->type == RICH_TODO ? "<ul class=\"todo-list\">" : "<ul>");
		list = b->type;
		if (b->type == RICH_PARAGRAPH) {
			write_str(&w, "<p>");
			write_inline(&w, b);
			write_str(&w, "</p>");
		} else if (b->type == RICH_TODO) {
			write_str(&w, b->checked ? "<li><label class=\"todo-list__label\"><input type=\"checkbox\" checked=\"checked\""
			                           " disabled=\"disabled\"><span class=\"todo-list__label__description\">"
			                         : "<li><label class=\"todo-list__label\"><input type=\"checkbox\""
			                           " disabled=\"disabled\"><span class=\"todo-list__label__description\">");
			write_inline(&w, b);
			write_str(&w, "</span></label></li>");
		} else {
			write_str(&w, "<li>");
			write_inline(&w, b);
			write_str(&w, "</li>");
		}
	}
	if (list != RICH_PARAGRAPH)
		write_str(&w, list == RICH_NUMBERED ? "</ol>" : "</ul>");
	if (!w.ok) {
		free(w.out);
		return NULL;
	}
	return w.out;
}

/* NON-NLS-END */
