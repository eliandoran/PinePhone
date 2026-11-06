/*
 * Markdown (GitHub's) as LVGL widgets in a chat bubble or a column of a page (a
 * README), parsed by md4c (md4c.c):
 * - paragraphs and headings: a label, or a span group when styles mix (bold,
 *   italic, code, links, strikethrough); line breaks kept, as GitHub's comments do
 * - lists: a bullet, a number or a task's box before each item, nested
 * - quotes: a bar on their left; code blocks: monospace in a darker box,
 *   scrolled sideways rather than wrapped; rules: a line
 * - tables: a grid, its header in bold, scrolled sideways when wider than the bubble
 * A text with HTML is GitHub's rendering of it instead (a comment's body_html, a
 * README's HTML), laid out as a page by phone-ui (ui_html_create()); without one,
 * HTML is shown as written.
 * Fonts beyond phone-ui's are md_fonts.c's (gen-fonts.sh).
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "github.h"
#include "md4c.h"

LV_FONT_DECLARE(md_bold_24)
LV_FONT_DECLARE(md_bold_26)
LV_FONT_DECLARE(md_bold_32)
LV_FONT_DECLARE(md_italic_24)
LV_FONT_DECLARE(md_mono_20)

#define DEPTH 16      /* nested containers kept apart; deeper ones go in the last */
#define LIST_INDENT 44 /* a list's marker and the gap after it */
#define QUOTE_INDENT 20 /* a quote's bar and the gap after it */
#define MAX_HTML_H 4096 /* a message as a page: cut there */
#define MAX_LABEL 3000 /* bytes of text in a label: cut there */
#define FLAGS (MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEURLAUTOLINKS | \
	       MD_FLAG_PERMISSIVEWWWAUTOLINKS)

enum style { EM = 1, STRONG = 2, CODE = 4, LINK = 8, DEL = 16 };

struct run {
	char *text;
	size_t len;
	unsigned style;
};

struct md {
	bool outgoing;
	lv_color_t color, bg; /* the bubble's text and background */
	/* Containers: the bubble, then quotes, lists, items */
	lv_obj_t *box[DEPTH];
	int32_t width[DEPTH]; /* what their text may take */
	int depth;
	/* Lists: the next item's number, 0 for bullets */
	unsigned number[DEPTH];
	int lists;
	/* The block being collected: its text by style */
	struct run *runs;
	unsigned count, cap;
	unsigned style;
	int heading; /* 1-6, 0: not one */
	bool code_block;
	/* The table being made: its grid, the cell's row and column */
	lv_obj_t *table;
	int row, col;
	bool header_cell; /* in bold */
};

static lv_obj_t *current(struct md *m)
{
	return m->box[m->depth < DEPTH ? m->depth : DEPTH - 1];
}

static int32_t avail(struct md *m)
{
	return m->width[m->depth < DEPTH ? m->depth : DEPTH - 1];
}

/* A column in the current container, `indent` narrower for its text */
static lv_obj_t *push(struct md *m, int32_t indent)
{
	lv_obj_t *box = ui_box(current(m));
	lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, 10, 0);
	int32_t w = avail(m) - indent;
	m->depth++;
	if (m->depth < DEPTH) {
		m->box[m->depth] = box;
		m->width[m->depth] = w > 100 ? w : 100;
	}
	return box;
}

static void pop(struct md *m)
{
	if (m->depth > 0)
		m->depth--;
}

static void append(struct md *m, const char *text, size_t len)
{
	if (!len)
		return;
	struct run *r = m->count ? &m->runs[m->count - 1] : NULL;
	if (!r || r->style != m->style) {
		if (m->count == m->cap) {
			unsigned cap = m->cap ? m->cap * 2 : 8;
			struct run *runs = realloc(m->runs, cap * sizeof(*runs));
			if (!runs)
				return;
			m->runs = runs;
			m->cap = cap;
		}
		r = &m->runs[m->count++];
		*r = (struct run){ NULL, 0, m->style };
	}
	char *t = realloc(r->text, r->len + len + 1);
	if (!t)
		return;
	memcpy(t + r->len, text, len);
	r->len += len;
	t[r->len] = '\0';
	r->text = t;
}

static void runs_clear(struct md *m)
{
	for (unsigned i = 0; i < m->count; i++)
		free(m->runs[i].text);
	m->count = 0;
}

static const lv_font_t *base_font(const struct md *m)
{
	switch (m->heading) {
	case 1: return &md_bold_32;
	case 2: return &md_bold_26;
	case 0: return m->header_cell ? &md_bold_24 : &ui_font_24;
	default: return &md_bold_24;
	}
}

static const lv_font_t *run_font(const struct md *m, unsigned style)
{
	if (style & CODE)
		return &md_mono_20;
	if (m->heading)
		return base_font(m);
	if ((style & STRONG) || m->header_cell)
		return &md_bold_24;
	if (style & EM)
		return &md_italic_24;
	return &ui_font_24;
}

static lv_color_t link_color(const struct md *m)
{
	return m->outgoing ? lv_color_white() : lv_color_hex(UI_COLOR_ACCENT);
}

/* Emoji composed: a copy for the widget to copy again */
static void set_text(lv_obj_t *label, lv_span_t *span, lv_obj_t *group, const char *text)
{
	char *composed = ui_emoji_compose(text);
	if (label)
		lv_label_set_text(label, composed ? composed : text);
	else
		lv_spangroup_set_span_text(group, span, composed ? composed : text);
	free(composed);
}

/* The collected text cut at MAX_LABEL bytes, whole characters, then an ellipsis:
 * a label is a texture as large as it */
static void runs_cut(struct md *m)
{
	size_t total = 0;
	for (unsigned i = 0; i < m->count; i++) {
		struct run *r = &m->runs[i];
		if (total + r->len <= MAX_LABEL) {
			total += r->len;
			continue;
		}
		size_t cut = MAX_LABEL - total;
		while (cut > 0 && ((unsigned char)r->text[cut] & 0xc0) == 0x80)
			cut--;
		char *t = realloc(r->text, cut + 4);
		if (t) {
			memcpy(t + cut, "\xe2\x80\xa6", 4);
			r->text = t;
			r->len = cut + 3;
		}
		for (unsigned j = i + 1; j < m->count; j++)
			free(m->runs[j].text);
		m->count = i + 1;
		return;
	}
}

/* The paragraph or heading collected: a label if it has one style, a span group
 * otherwise */
static void flush_text(struct md *m)
{
	/* Without the line breaks it ends with */
	while (m->count) {
		struct run *r = &m->runs[m->count - 1];
		while (r->len && r->text[r->len - 1] == '\n')
			r->text[--r->len] = '\0';
		if (r->len)
			break;
		free(r->text);
		m->count--;
	}
	runs_cut(m);
	if (!m->count)
		return;

	int32_t w = avail(m);
	if (m->count == 1 && !(m->runs[0].style & (CODE | LINK | DEL)) &&
	    run_font(m, m->runs[0].style) == base_font(m)) {
		lv_obj_t *label = lv_label_create(current(m));
		set_text(label, NULL, NULL, m->runs[0].text);
		lv_obj_set_style_max_width(label, w, 0);
		lv_obj_set_style_text_font(label, base_font(m), 0);
		lv_obj_set_style_text_color(label, m->color, 0);
		runs_clear(m);
		return;
	}

	lv_obj_t *group = lv_spangroup_create(current(m));
	lv_obj_set_style_text_font(group, base_font(m), 0);
	lv_obj_set_style_text_color(group, m->color, 0);
	for (unsigned i = 0; i < m->count; i++) {
		const struct run *r = &m->runs[i];
		lv_span_t *span = lv_spangroup_add_span(group);
		set_text(NULL, span, group, r->text);
		lv_style_t *style = lv_span_get_style(span);
		const lv_font_t *font = run_font(m, r->style);
		if (font != base_font(m))
			lv_style_set_text_font(style, font);
		if (r->style & LINK) {
			lv_style_set_text_color(style, link_color(m));
			lv_style_set_text_decor(style, LV_TEXT_DECOR_UNDERLINE);
		}
		if (r->style & DEL)
			lv_style_set_text_decor(style, LV_TEXT_DECOR_STRIKETHROUGH);
	}
	lv_spangroup_refresh(group);
	/* As wide as its text, up to the bubble's width */
	uint32_t natural = lv_spangroup_get_expand_width(group, (uint32_t)w);
	lv_obj_set_size(group, natural && natural < (uint32_t)w ? (int32_t)natural : w, LV_SIZE_CONTENT);
	runs_clear(m);
}

/* The code block collected: monospace, not wrapped, in a darker box scrolled
 * sideways when wider than the bubble */
static void flush_code(struct md *m)
{
	runs_cut(m);
	size_t n = 0;
	for (unsigned i = 0; i < m->count; i++)
		n += m->runs[i].len;
	char *text = malloc(n + 1);
	if (text) {
		n = 0;
		for (unsigned i = 0; i < m->count; i++) {
			memcpy(text + n, m->runs[i].text, m->runs[i].len);
			n += m->runs[i].len;
		}
		while (n && text[n - 1] == '\n')
			n--;
		text[n] = '\0';

		lv_obj_t *box = ui_box(current(m));
		lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_max_width(box, avail(m), 0);
		lv_obj_set_style_pad_all(box, 12, 0);
		lv_obj_set_style_radius(box, 12, 0);
		lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(box, lv_color_darken(m->bg, LV_OPA_30), 0);
		lv_obj_set_scrollable(box, true);
		lv_obj_set_scroll_dir(box, LV_DIR_HOR);
		lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

		lv_obj_t *label = lv_label_create(box);
		lv_label_set_text(label, text);
		lv_obj_set_style_text_font(label, &md_mono_20, 0);
		lv_obj_set_style_text_color(label, m->color, 0);
		free(text);
	}
	runs_clear(m);
}

/* A list item's marker: a bullet, its number, or a task's box (checked: a tick) */
static void marker(struct md *m, lv_obj_t *row, const MD_BLOCK_LI_DETAIL *li)
{
	if (li->is_task) {
		lv_obj_t *box = ui_box(row);
		lv_obj_set_size(box, 26, 26);
		lv_obj_set_style_margin_top(box, 2, 0);
		lv_obj_set_style_radius(box, 6, 0);
		lv_obj_set_style_border_width(box, 2, 0);
		lv_obj_set_style_border_color(box, m->color, 0);
		if (li->task_mark != ' ') {
			lv_obj_t *tick = lv_label_create(box);
			lv_label_set_text_static(tick, LV_SYMBOL_OK);
			lv_obj_set_style_text_font(tick, &ui_font_16, 0);
			lv_obj_set_style_text_color(tick, m->color, 0);
			lv_obj_center(tick);
		}
		return;
	}
	lv_obj_t *label = lv_label_create(row);
	lv_obj_set_style_text_color(label, m->color, 0);
	unsigned *number = &m->number[m->lists > 0 ? (m->lists - 1) % DEPTH : 0];
	if (*number)
		ui_set_text_fmt(label, "%u.", (*number)++);
	else
		lv_label_set_text_static(label, "\xe2\x80\xa2"); /* • */
}

/* Tables: a grid, columns as wide as their widest cell, scrolled sideways when
 * wider than the bubble; lines between the cells */
static void grid_free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void table_create(struct md *m, const MD_BLOCK_TABLE_DETAIL *t)
{
	lv_obj_t *wrap = ui_box(current(m));
	lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(wrap, avail(m), 0);
	lv_obj_set_scrollable(wrap, true);
	lv_obj_set_scroll_dir(wrap, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);

	lv_obj_t *grid = ui_box(wrap);
	lv_obj_set_size(grid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_border_side(grid, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(grid, 1, 0);
	lv_obj_set_style_border_color(grid, lv_color_mix(m->color, m->bg, LV_OPA_30), 0);
	unsigned cols = t->col_count, rows = t->head_row_count + t->body_row_count;
	/* Kept by the grid: freed with it */
	int32_t *dsc = malloc((cols + rows + 2) * sizeof(*dsc));
	if (dsc) {
		for (unsigned i = 0; i < cols + rows + 2; i++)
			dsc[i] = LV_GRID_CONTENT;
		dsc[cols] = dsc[cols + rows + 1] = LV_GRID_TEMPLATE_LAST;
		lv_obj_set_grid_dsc_array(grid, dsc, dsc + cols + 1);
		lv_obj_add_event_cb(grid, grid_free_cb, LV_EVENT_DELETE, dsc);
	}
	m->table = grid;
	m->row = -1;

	m->depth++;
	if (m->depth < DEPTH) {
		m->box[m->depth] = grid;
		m->width[m->depth] = m->width[m->depth - 1] * 2 / 3; /* a cell's text */
	}
}

static void cell_create(struct md *m, bool header)
{
	lv_obj_t *cell = ui_box(m->table ? m->table : current(m));
	lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, m->col, 1, LV_GRID_ALIGN_STRETCH, m->row < 0 ? 0 : m->row, 1);
	lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(cell, 10, 0);
	lv_obj_set_style_pad_ver(cell, 6, 0);
	lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(cell, 1, 0);
	lv_obj_set_style_border_color(cell, lv_color_mix(m->color, m->bg, LV_OPA_30), 0);
	m->header_cell = header;
	m->depth++;
	if (m->depth < DEPTH) {
		m->box[m->depth] = cell;
		m->width[m->depth] = m->width[m->depth - 1];
	}
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	struct md *m = user;
	/* A tight list item's text, before what it nests */
	flush_text(m);
	switch (type) {
	case MD_BLOCK_QUOTE: {
		lv_obj_t *box = push(m, QUOTE_INDENT);
		lv_obj_set_style_border_side(box, LV_BORDER_SIDE_LEFT, 0);
		lv_obj_set_style_border_width(box, 4, 0);
		lv_obj_set_style_border_color(box, lv_color_mix(m->color, m->bg, LV_OPA_50), 0);
		lv_obj_set_style_pad_left(box, 16, 0);
		break;
	}
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		push(m, 0);
		m->number[m->lists % DEPTH] = type == MD_BLOCK_OL ? ((MD_BLOCK_OL_DETAIL *)detail)->start : 0;
		m->lists++;
		break;
	case MD_BLOCK_LI: {
		lv_obj_t *row = ui_box(current(m));
		lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(row, 12, 0);
		marker(m, row, detail);
		/* The item's content beside its marker */
		m->depth++;
		if (m->depth < DEPTH) {
			m->box[m->depth] = row;
			m->width[m->depth] = m->width[m->depth - 1];
		}
		push(m, LIST_INDENT);
		break;
	}
	case MD_BLOCK_H:
		m->heading = (int)((MD_BLOCK_H_DETAIL *)detail)->level;
		break;
	case MD_BLOCK_TABLE:
		table_create(m, detail);
		break;
	case MD_BLOCK_TR:
		m->row++;
		m->col = 0;
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		cell_create(m, type == MD_BLOCK_TH);
		break;
	case MD_BLOCK_CODE:
		m->code_block = true;
		break;
	default:
		break;
	}
	return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	LV_UNUSED(detail);
	struct md *m = user;
	switch (type) {
	case MD_BLOCK_QUOTE:
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		pop(m);
		if (type != MD_BLOCK_QUOTE)
			m->lists--;
		break;
	case MD_BLOCK_LI:
		/* Leaves a tight item's text first */
		flush_text(m);
		pop(m);
		pop(m);
		break;
	case MD_BLOCK_P:
	case MD_BLOCK_HTML:
		flush_text(m);
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		flush_text(m);
		pop(m);
		m->header_cell = false;
		m->col++;
		break;
	case MD_BLOCK_TABLE:
		pop(m);
		m->table = NULL;
		break;
	case MD_BLOCK_H:
		flush_text(m);
		m->heading = 0;
		break;
	case MD_BLOCK_CODE:
		flush_code(m);
		m->code_block = false;
		break;
	case MD_BLOCK_HR: {
		lv_obj_t *line = ui_box(current(m));
		lv_obj_set_size(line, avail(m), 2);
		lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(line, lv_color_mix(m->color, m->bg, LV_OPA_30), 0);
		break;
	}
	default:
		break;
	}
	return 0;
}

static unsigned span_style(MD_SPANTYPE type)
{
	switch (type) {
	case MD_SPAN_EM: return EM;
	case MD_SPAN_STRONG: return STRONG;
	case MD_SPAN_CODE: return CODE;
	case MD_SPAN_A: return LINK;
	case MD_SPAN_DEL: return DEL;
	default: return 0;
	}
}

static int enter_span(MD_SPANTYPE type, void *detail, void *user)
{
	LV_UNUSED(detail);
	struct md *m = user;
	m->style |= span_style(type);
	return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *user)
{
	LV_UNUSED(detail);
	struct md *m = user;
	m->style &= ~span_style(type);
	return 0;
}

/* &amp; &#233; &#x1F600;: UTF-8 into out (at least 5 bytes), its length; 0 if unknown */
static size_t entity(const char *text, size_t len, char *out)
{
	static const struct {
		const char *name, *utf8;
	} named[] = {
		{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
		{"&nbsp;", "\xc2\xa0"}, {"&copy;", "\xc2\xa9"}, {"&mdash;", "\xe2\x80\x94"},
		{"&ndash;", "\xe2\x80\x93"}, {"&hellip;", "\xe2\x80\xa6"},
	};
	for (size_t i = 0; i < LV_ARRAYLEN(named); i++) {
		if (strlen(named[i].name) == len && !memcmp(named[i].name, text, len)) {
			strcpy(out, named[i].utf8);
			return strlen(out);
		}
	}
	if (len < 4 || text[1] != '#')
		return 0;
	unsigned long cp = text[2] == 'x' || text[2] == 'X' ? strtoul(text + 3, NULL, 16) : strtoul(text + 2, NULL, 10);
	if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		cp = 0xfffd;
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static int shortcode_cmp(const void *key, const void *entry)
{
	return strcmp(key, ((const struct shortcode *)entry)->name);
}

/* Text with the emoji's names (:white_check_mark:) as the emoji, as GitHub shows them */
static void append_emoji(struct md *m, const char *text, size_t size)
{
	size_t done = 0;
	for (size_t i = 0; i < size; i++) {
		if (text[i] != ':')
			continue;
		size_t j = i + 1;
		while (j < size && j - i <= 40 &&
		       ((text[j] >= 'a' && text[j] <= 'z') || (text[j] >= '0' && text[j] <= '9') ||
			text[j] == '_' || text[j] == '+' || text[j] == '-'))
			j++;
		if (j == i + 1 || j >= size || text[j] != ':')
			continue;
		char name[48];
		memcpy(name, text + i + 1, j - i - 1);
		name[j - i - 1] = '\0';
		const struct shortcode *s = bsearch(name, shortcodes, shortcode_count, sizeof(*shortcodes), shortcode_cmp);
		if (!s)
			continue;
		append(m, text + done, i - done);
		append(m, s->emoji, strlen(s->emoji));
		done = j + 1;
		i = j;
	}
	append(m, text + done, size - done);
}

static int text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *user)
{
	struct md *m = user;
	char buf[8];
	size_t n;
	switch (type) {
	case MD_TEXT_NULLCHAR:
		append(m, "\xef\xbf\xbd", 3);
		break;
	case MD_TEXT_BR:
	case MD_TEXT_SOFTBR: /* kept: GitHub's comments break lines where they are written */
		append(m, "\n", 1);
		break;
	case MD_TEXT_ENTITY:
		n = entity(text, size, buf);
		if (n)
			append(m, buf, n);
		else
			append(m, text, size);
		break;
	case MD_TEXT_NORMAL:
		append_emoji(m, text, size);
		break;
	default: /* code, HTML as written, LaTeX */
		append(m, text, size);
		break;
	}
	return 0;
}

/* A table's delimiter row (|---|:--:|), the line from s to its end */
static bool delimiter_row(const char *s)
{
	bool dash = false, pipe = false;
	for (; *s && *s != '\n'; s++) {
		if (*s == '-')
			dash = true;
		else if (*s == '|')
			pipe = true;
		else if (*s != ':' && *s != ' ' && *s != '\t')
			return false;
	}
	return dash && pipe;
}

/* A copy with a blank line before each table that follows a paragraph's line:
 * GitHub's tables can interrupt a paragraph, md4c's cannot (malloc'd) */
static char *tables_apart(const char *text)
{
	size_t n = strlen(text), extra = 0;
	for (const char *s = text; (s = strchr(s, '\n')); s++)
		extra++;
	char *out = malloc(n + extra + 1), *o = out;
	if (!out)
		return NULL;
	bool blank = true; /* the line before */
	for (const char *line = text; *line;) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line + 1) : strlen(line);
		if (!blank && end && memchr(line, '|', len) && delimiter_row(end + 1))
			*o++ = '\n';
		blank = line[strspn(line, " \t")] == '\n' || !line[strspn(line, " \t")];
		memcpy(o, line, len);
		o += len;
		line += len;
	}
	*o = '\0';
	return out;
}

/* What the widgets do not show: HTML (comments aside: they were taken out) */
static int needs_html_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	LV_UNUSED(detail);
	if (type == MD_BLOCK_HTML) {
		*(bool *)user = true;
		return 1; /* found: the parse stops */
	}
	return 0;
}

static int needs_html_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *user)
{
	LV_UNUSED(text);
	LV_UNUSED(size);
	if (type == MD_TEXT_HTML) {
		*(bool *)user = true;
		return 1;
	}
	return 0;
}

static int no_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	LV_UNUSED(type);
	LV_UNUSED(detail);
	LV_UNUSED(user);
	return 0;
}

static int no_span(MD_SPANTYPE type, void *detail, void *user)
{
	LV_UNUSED(type);
	LV_UNUSED(detail);
	LV_UNUSED(user);
	return 0;
}

static bool needs_html(const char *text_md)
{
	bool found = false;
	MD_PARSER parser = {
		.flags = FLAGS,
		.enter_block = needs_html_block,
		.leave_block = no_block,
		.enter_span = no_span,
		.leave_span = no_span,
		.text = needs_html_text,
	};
	md_parse(text_md, (MD_SIZE)strlen(text_md), &parser, &found);
	return found;
}

static uint32_t hex(lv_color_t c)
{
	return lv_color_to_u32(c) & 0xffffff;
}

/* GitHub's rendering in the text's colors, laid out as a page */
static void html_create(const struct md *m, const char *html, const char *base_url, int32_t max_h)
{
	char css[1024];
	snprintf(css, sizeof(css),
		 "body { color: #%06" PRIx32 "; background: #%06" PRIx32 "; font-size: 16px; }\n"
		 "a { color: #%06" PRIx32 "; }\n"
		 "p, ul, ol, pre, table, blockquote, details { margin: 0 0 10px 0; }\n"
		 "body > :last-child { margin-bottom: 0; }\n"
		 "code, pre { background: #%06" PRIx32 "; }\n"
		 "pre { padding: 6px; }\n"
		 "table { border-collapse: collapse; }\n"
		 "td, th { border: 1px solid #%06" PRIx32 "; padding: 2px 6px; }\n"
		 "blockquote { padding-left: 10px; border-left: 3px solid #%06" PRIx32 "; }\n",
		 hex(m->color), hex(m->bg), hex(link_color(m)), hex(lv_color_darken(m->bg, LV_OPA_30)),
		 hex(lv_color_mix(m->color, m->bg, LV_OPA_30)), hex(lv_color_mix(m->color, m->bg, LV_OPA_50)));
	ui_html_create(m->box[0], html, base_url, m->width[0], max_h, css);
}

bool markdown_needs_html(const char *text_md)
{
	char *apart = tables_apart(text_md);
	bool needs = needs_html(apart ? apart : text_md);
	free(apart);
	return needs;
}

/* Into m's column: GitHub's HTML when the widgets would not show it all */
static void render(struct md *m, const char *text_md, const char *html, const char *base_url, int32_t max_h)
{
	lv_obj_set_flex_flow(m->box[0], LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(m->box[0], 20, 0);

	char *apart = tables_apart(text_md);
	const char *src = apart ? apart : text_md;
	if (html && needs_html(src)) {
		free(apart);
		html_create(m, html, base_url, max_h);
		return;
	}

	MD_PARSER parser = {
		.flags = FLAGS,
		.enter_block = enter_block,
		.leave_block = leave_block,
		.enter_span = enter_span,
		.leave_span = leave_span,
		.text = text,
	};
	if (md_parse(src, (MD_SIZE)strlen(src), &parser, m) != 0)
		printf("github: Markdown not parsed\n");
	/* What a failed parse left */
	flush_text(m);
	runs_clear(m);
	free(m->runs);
	free(apart);
}

void markdown_bubble(lv_obj_t *group, bool outgoing, const char *text_md, const char *html)
{
	/* The bubble ui_chat makes, its label replaced by a column */
	lv_obj_t *label = ui_chat_bubble_create(group, outgoing);
	lv_obj_t *bubble = lv_obj_get_parent(label);
	struct md m = {
		.outgoing = outgoing,
		.color = lv_obj_get_style_text_color(label, 0),
		.bg = lv_obj_get_style_bg_color(bubble, 0),
	};
	m.box[0] = bubble;
	m.width[0] = lv_obj_get_style_max_width(label, 0);
	lv_obj_delete(label);
	render(&m, text_md, html, "https://github.com/", MAX_HTML_H);
}

void markdown_create(lv_obj_t *box, int32_t width, const char *text_md, const char *html, const char *base_url,
		     int32_t max_html_h)
{
	struct md m = {
		.color = lv_color_hex(UI_COLOR_TEXT),
		.bg = lv_obj_get_style_bg_color(box, 0),
	};
	m.box[0] = box;
	m.width[0] = width;
	render(&m, text_md, html, base_url, max_html_h);
}
