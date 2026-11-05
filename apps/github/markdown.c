/*
 * Markdown (GitHub's) as LVGL widgets in a chat bubble, parsed by md4c (md4c.c):
 * - paragraphs and headings: a label, or a span group when styles mix (bold,
 *   italic, code, links, strikethrough); line breaks kept, as GitHub's comments do
 * - lists: a bullet, a number or a task's box before each item, nested
 * - quotes: a bar on their left; code blocks: monospace in a darker box,
 *   scrolled sideways rather than wrapped; rules: a line
 * HTML is shown as written (tags not supported), tables as their text. Fonts
 * beyond phone-ui's are md_fonts.c's (gen-fonts.sh).
 */

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
	case 0: return &ui_font_24;
	default: return &md_bold_24;
	}
}

static const lv_font_t *run_font(const struct md *m, unsigned style)
{
	if (style & CODE)
		return &md_mono_20;
	if (m->heading)
		return base_font(m);
	if (style & STRONG)
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
	case MD_BLOCK_TABLE:
		flush_text(m);
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
	default: /* normal, code, HTML as written, LaTeX */
		append(m, text, size);
		break;
	}
	return 0;
}

void markdown_bubble(lv_obj_t *group, bool outgoing, const char *text_md)
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
	lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(bubble, 20, 0);

	MD_PARSER parser = {
		.abi_version = 0,
		.flags = MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEURLAUTOLINKS |
			 MD_FLAG_PERMISSIVEWWWAUTOLINKS,
		.enter_block = enter_block,
		.leave_block = leave_block,
		.enter_span = enter_span,
		.leave_span = leave_span,
		.text = text,
	};
	if (md_parse(text_md, (MD_SIZE)strlen(text_md), &parser, &m) != 0)
		printf("github: Markdown not parsed\n");
	/* What a failed parse left */
	flush_text(&m);
	runs_clear(&m);
	free(m.runs);
}
