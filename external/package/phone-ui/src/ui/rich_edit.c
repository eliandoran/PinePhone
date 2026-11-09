/*
 * The rich text editor (rich_edit.h): a view of its own class that lays out and draws a
 * rich_doc, and a text area out of sight that the keyboard types into. Each change of the
 * text area's text is applied to the document as the deletion and insertion that make it
 * (one or the other when a key typed or deleted at the cursor), the insertion in the
 * marks to type (`typing`: the toolbar's, or the text's at the cursor once it moved).
 *
 * Layout: a block's text is cut into lines at the last space that fits (between
 * characters for a word wider than the view), each line into fragments of one run. Every
 * line is the font's line height: the marks' fonts share their base's (gen-md-fonts.sh).
 * Laid out again whole on each change; per block when notes grow long.
 */

#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_symbols.h"
#include "ui/rich_edit.h"
#include "src/core/lv_obj_class_private.h" /* the view's class */
#include "src/core/lv_obj_private.h"
#include "src/draw/lv_draw_private.h" /* the layer's clip area */
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_text_private.h" /* lv_text_encoded_*() */

LV_FONT_DECLARE(md_bold_24)
LV_FONT_DECLARE(md_bold_26)
LV_FONT_DECLARE(md_bold_32)
LV_FONT_DECLARE(md_italic_24)
LV_FONT_DECLARE(md_italic_26)
LV_FONT_DECLARE(md_bold_italic_26)

#define RICH_BLINK_MS 500   /* the cursor on, then off */
#define RICH_POLL_MS 100    /* the text area's cursor checked, while focused */
#define RICH_CURSOR_W 3
#define RICH_TOOLBAR_H 80
#define RICH_BUTTON_W 96

/* A piece of a line in one run's marks: block offsets, x from the line's start */
struct frag {
	uint32_t start, end;
	int32_t x;
	unsigned marks;
};

/* Lines are line_h apart, the first at the content's top */
struct line {
	uint32_t block, start, end;
	uint32_t frag, frag_count; /* in frags */
};

typedef struct {
	lv_obj_t obj;
	lv_obj_t *ta, *toolbar;
	struct rich_doc *doc;
	char *text; /* the text area's text as last applied */
	char *placeholder;
	struct line *lines;
	uint32_t line_count, line_cap;
	struct frag *frags;
	uint32_t frag_count, frag_cap;
	int32_t layout_w; /* the width laid out for, -1: none yet */
	uint32_t cursor;  /* byte offset */
	unsigned typing;
	lv_timer_t *timer;
	uint32_t blink_start;
	bool blink_on; /* as drawn */
} rich_edit_t;

/* Fonts */

static const struct {
	const lv_font_t *base, *bold, *italic, *bold_italic;
} mark_fonts[] = {
	{ &ui_font_26, &md_bold_26, &md_italic_26, &md_bold_italic_26 },
	{ &ui_font_24, &md_bold_24, &md_italic_24, &md_bold_24 },
	{ &ui_font_32, &md_bold_32, &ui_font_32, &md_bold_32 },
};

static const lv_font_t *base_font(const rich_edit_t *e)
{
	return lv_obj_get_style_text_font(&e->obj, LV_PART_MAIN);
}

/* The font of text in `marks` (underline is drawn); the base's for sizes without */
static const lv_font_t *mark_font(const rich_edit_t *e, unsigned marks)
{
	const lv_font_t *base = base_font(e);
	unsigned style = marks & (RICH_BOLD | RICH_ITALIC);
	for (size_t i = 0; style && i < sizeof(mark_fonts) / sizeof(mark_fonts[0]); i++) {
		if (mark_fonts[i].base != base)
			continue;
		return style == RICH_BOLD     ? mark_fonts[i].bold
		       : style == RICH_ITALIC ? mark_fonts[i].italic
		                              : mark_fonts[i].bold_italic;
	}
	return base;
}

static int32_t line_height(const rich_edit_t *e)
{
	return lv_font_get_line_height(base_font(e)) + lv_obj_get_style_text_line_space(&e->obj, LV_PART_MAIN);
}

/* The width of text[from, to) as drawn: each glyph's advance, kerned with the next */
static int32_t text_width(const lv_font_t *font, const char *text, uint32_t from, uint32_t to)
{
	int32_t w = 0;
	uint32_t i = from;
	while (i < to) {
		uint32_t c = lv_text_encoded_next(text, &i), next = 0;
		if (i < to) {
			uint32_t j = i;
			next = lv_text_encoded_next(text, &j);
		}
		w += lv_font_get_glyph_width(font, c, next);
	}
	return w;
}

/* Layout */

/* Where the line from `start` ends to fit in `w` */
static uint32_t line_end(const rich_edit_t *e, const struct rich_block *b, uint32_t start, int32_t w)
{
	uint32_t r = 0, run_start = 0;
	while (r < b->run_count && run_start + b->runs[r].len <= start)
		run_start += b->runs[r++].len;
	int32_t x = 0;
	uint32_t i = start, brk = 0; /* after the last space */
	while (i < b->len) {
		while (i >= run_start + b->runs[r].len)
			run_start += b->runs[r++].len;
		uint32_t next_i = i, peek;
		uint32_t c = lv_text_encoded_next(b->text, &next_i), next = 0;
		if (next_i < b->len) {
			peek = next_i;
			next = lv_text_encoded_next(b->text, &peek);
		}
		int32_t cw = lv_font_get_glyph_width(mark_font(e, b->runs[r].marks), c, next);
		/* Spaces hang past the edge */
		if (c != ' ' && x + cw > w && i > start)
			return brk > start ? brk : i;
		x += cw;
		i = next_i;
		if (c == ' ')
			brk = i;
	}
	return b->len;
}

static void add_line(rich_edit_t *e, uint32_t bi, uint32_t start, uint32_t end)
{
	if (e->line_count == e->line_cap) {
		e->line_cap = e->line_cap ? e->line_cap * 2 : 64;
		e->lines = realloc(e->lines, e->line_cap * sizeof(*e->lines));
	}
	struct line *l = &e->lines[e->line_count++];
	*l = (struct line){ bi, start, end, e->frag_count, 0 };

	const struct rich_block *b = &e->doc->blocks[bi];
	int32_t x = 0;
	uint32_t at = 0;
	for (uint32_t r = 0; r < b->run_count && at < end; r++) {
		uint32_t s = LV_MAX(at, start), en = LV_MIN(at + b->runs[r].len, end);
		at += b->runs[r].len;
		if (s >= en)
			continue;
		if (e->frag_count == e->frag_cap) {
			e->frag_cap = e->frag_cap ? e->frag_cap * 2 : 64;
			e->frags = realloc(e->frags, e->frag_cap * sizeof(*e->frags));
		}
		e->frags[e->frag_count++] = (struct frag){ s, en, x, b->runs[r].marks };
		l->frag_count++;
		x += text_width(mark_font(e, b->runs[r].marks), b->text, s, en);
	}
}

static void layout(rich_edit_t *e)
{
	int32_t w = lv_obj_get_content_width(&e->obj);
	e->layout_w = w;
	e->line_count = e->frag_count = 0;
	for (uint32_t bi = 0; bi < e->doc->count; bi++) {
		const struct rich_block *b = &e->doc->blocks[bi];
		uint32_t start = 0;
		do {
			uint32_t end = line_end(e, b, start, w);
			add_line(e, bi, start, end);
			start = end;
		} while (start < b->len);
	}
}

static void ensure_layout(rich_edit_t *e)
{
	if (e->layout_w != lv_obj_get_content_width(&e->obj))
		layout(e);
}

/* Where the content's top left is on the screen, scrolled */
static lv_point_t origin(rich_edit_t *e)
{
	lv_area_t a;
	lv_obj_get_content_coords(&e->obj, &a);
	return (lv_point_t){ a.x1 - lv_obj_get_scroll_x(&e->obj), a.y1 - lv_obj_get_scroll_y(&e->obj) };
}

/* The line the cursor is on and its x there */
static uint32_t cursor_line(rich_edit_t *e, int32_t *x)
{
	ensure_layout(e);
	uint32_t bi, off;
	rich_locate(e->doc, e->cursor, &bi, &off);
	uint32_t li = 0;
	while (li < e->line_count && e->lines[li].block < bi)
		li++;
	/* At a wrapped line's end: the next one's start */
	while (li + 1 < e->line_count && e->lines[li + 1].block == bi && off >= e->lines[li].end)
		li++;
	*x = 0;
	if (li >= e->line_count)
		return 0;
	const struct line *l = &e->lines[li];
	const char *text = e->doc->blocks[bi].text;
	for (uint32_t i = 0; i < l->frag_count; i++) {
		const struct frag *f = &e->frags[l->frag + i];
		if (off <= f->end) {
			*x = f->x + text_width(mark_font(e, f->marks), text, f->start, LV_MAX(off, f->start));
			break;
		}
	}
	return li;
}

/* The position nearest to a point on the screen */
static uint32_t pos_at(rich_edit_t *e, lv_point_t p)
{
	ensure_layout(e);
	lv_point_t o = origin(e);
	int32_t y = p.y - o.y, x = p.x - o.x;
	if (!e->line_count)
		return 0;
	if (y >= (int32_t)e->line_count * line_height(e))
		return rich_doc_length(e->doc);
	const struct line *l = &e->lines[y < 0 ? 0 : y / line_height(e)];
	const struct rich_block *b = &e->doc->blocks[l->block];
	uint32_t base = rich_block_pos(e->doc, l->block);
	for (uint32_t k = 0; k < l->frag_count; k++) {
		const struct frag *f = &e->frags[l->frag + k];
		const lv_font_t *font = mark_font(e, f->marks);
		int32_t cx = f->x;
		for (uint32_t i = f->start; i < f->end;) {
			uint32_t next_i = i, c = lv_text_encoded_next(b->text, &next_i), next = 0, peek = next_i;
			if (next_i < f->end)
				next = lv_text_encoded_next(b->text, &peek);
			int32_t cw = lv_font_get_glyph_width(font, c, next);
			if (x < cx + cw / 2)
				return base + i;
			cx += cw;
			i = next_i;
		}
	}
	/* A wrapped line's end: before its last space, which the next line starts after */
	uint32_t end = l->end;
	if (end < b->len && end > l->start && b->text[end - 1] == ' ')
		end--;
	return base + end;
}

/* The cursor */

static bool focused(rich_edit_t *e)
{
	return lv_obj_has_state(e->ta, LV_STATE_FOCUSED);
}

static void cursor_invalidate(rich_edit_t *e)
{
	int32_t x, lh = line_height(e);
	uint32_t li = cursor_line(e, &x);
	lv_point_t o = origin(e);
	lv_area_t a = { o.x + x - RICH_CURSOR_W, o.y + (int32_t)li * lh, o.x + x + RICH_CURSOR_W, o.y + (int32_t)(li + 1) * lh };
	lv_obj_invalidate_area(&e->obj, &a);
}

/* Shown from now on for a blink, as after each key */
static void blink_restart(rich_edit_t *e)
{
	e->blink_start = lv_tick_get();
	if (!e->blink_on)
		cursor_invalidate(e);
}

void ui_rich_edit_show_cursor(lv_obj_t *edit)
{
	rich_edit_t *e = (rich_edit_t *)edit;
	int32_t x, lh = line_height(e);
	int32_t y = (int32_t)cursor_line(e, &x) * lh;
	int32_t top = lv_obj_get_scroll_y(edit), h = lv_obj_get_content_height(edit);
	if (y < top)
		lv_obj_scroll_to_y(edit, y, LV_ANIM_OFF);
	else if (y + lh > top + h)
		lv_obj_scroll_to_y(edit, y + lh - h, LV_ANIM_OFF);
}

static void toolbar_update(rich_edit_t *e)
{
	if (!e->toolbar)
		return;
	for (uint32_t i = 0; i < lv_obj_get_child_count(e->toolbar); i++) {
		lv_obj_t *btn = lv_obj_get_child(e->toolbar, (int32_t)i);
		unsigned marks = (unsigned)(uintptr_t)lv_obj_get_user_data(btn);
		lv_obj_set_state(btn, LV_STATE_CHECKED, (e->typing & marks) != 0);
	}
}

/* The cursor moved without typing: what is typed takes the marks there */
static void cursor_moved(rich_edit_t *e, uint32_t pos)
{
	cursor_invalidate(e);
	e->cursor = pos;
	e->typing = rich_marks_at(e->doc, pos);
	toolbar_update(e);
	ui_rich_edit_show_cursor(&e->obj);
	blink_restart(e);
	cursor_invalidate(e);
}

static uint32_t ta_cursor(rich_edit_t *e)
{
	return lv_text_encoded_get_byte_id(lv_textarea_get_text(e->ta), lv_textarea_get_cursor_pos(e->ta));
}

static void poll_cb(lv_timer_t *t)
{
	rich_edit_t *e = lv_timer_get_user_data(t);
	uint32_t pos = ta_cursor(e);
	if (pos != e->cursor)
		cursor_moved(e, pos);
	bool on = lv_tick_elaps(e->blink_start) / RICH_BLINK_MS % 2 == 0;
	if (on != e->blink_on)
		cursor_invalidate(e);
}

/* Polled while typing: the keyboard moves the cursor by itself (the space bar) */
static void ta_state_cb(lv_event_t *ev)
{
	rich_edit_t *e = lv_event_get_user_data(ev);
	if (focused(e)) {
		lv_timer_resume(e->timer);
		blink_restart(e);
	} else {
		lv_timer_pause(e->timer);
	}
	cursor_invalidate(e);
}

/* Edits */

/* Where a UTF-8 character starts, at or before i */
static size_t char_start(const char *s, size_t i)
{
	while (i && ((unsigned char)s[i] & 0xc0) == 0x80)
		i--;
	return i;
}

/* The text area's text changed: the same change to the document */
static void ta_changed_cb(lv_event_t *ev)
{
	rich_edit_t *e = lv_event_get_user_data(ev);
	const char *now = lv_textarea_get_text(e->ta), *was = e->text;
	size_t nl = strlen(now), ol = strlen(was), cur = ta_cursor(e);
	size_t start, old_end, new_end;
	if (nl > ol && cur >= nl - ol && memcmp(now, was, cur - (nl - ol)) == 0 &&
	    strcmp(now + cur, was + cur - (nl - ol)) == 0) {
		/* Typed: what is before the cursor */
		start = cur - (nl - ol);
		old_end = start;
		new_end = cur;
	} else if (ol > nl && cur <= nl && memcmp(now, was, cur) == 0 && strcmp(now + cur, was + cur + (ol - nl)) == 0) {
		/* Deleted at the cursor */
		start = new_end = cur;
		old_end = cur + (ol - nl);
	} else {
		/* Set: what differs between what they have in common at the start and end */
		start = 0;
		while (start < ol && start < nl && was[start] == now[start])
			start++;
		start = char_start(now, start);
		size_t same = 0;
		while (same < ol - start && same < nl - start && was[ol - 1 - same] == now[nl - 1 - same])
			same++;
		while (same && ((unsigned char)was[ol - same] & 0xc0) == 0x80)
			same--;
		old_end = ol - same;
		new_end = nl - same;
	}
	cursor_invalidate(e);
	rich_delete(e->doc, (uint32_t)start, (uint32_t)old_end);
	rich_insert(e->doc, (uint32_t)start, now + start, (uint32_t)(new_end - start), e->typing);
	free(e->text);
	e->text = strdup(now);
	e->cursor = (uint32_t)cur;

	layout(e);
	lv_obj_refresh_self_size(&e->obj);
	lv_obj_invalidate(&e->obj);
	ui_rich_edit_show_cursor(&e->obj);
	blink_restart(e);
}

/* The view */

static void rich_edit_constructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
	LV_UNUSED(cls);
	rich_edit_t *e = (rich_edit_t *)obj;
	e->doc = rich_doc_new("");
	e->text = strdup("");
	e->layout_w = -1;
	e->timer = lv_timer_create(poll_cb, RICH_POLL_MS, e);
	lv_timer_pause(e->timer);
}

static void rich_edit_destructor(const lv_obj_class_t *cls, lv_obj_t *obj)
{
	LV_UNUSED(cls);
	rich_edit_t *e = (rich_edit_t *)obj;
	if (e->toolbar)
		lv_obj_set_user_data(e->toolbar, NULL);
	lv_timer_delete(e->timer);
	rich_doc_free(e->doc);
	free(e->text);
	free(e->placeholder);
	free(e->lines);
	free(e->frags);
}

static void draw(rich_edit_t *e, lv_layer_t *layer)
{
	ensure_layout(e);
	lv_point_t o = origin(e);
	int32_t lh = line_height(e), w = lv_obj_get_content_width(&e->obj);
	lv_draw_label_dsc_t dsc;
	lv_draw_label_dsc_init(&dsc);
	lv_obj_init_draw_label_dsc(&e->obj, LV_PART_MAIN, &dsc);
	dsc.flag |= LV_TEXT_FLAG_EXPAND;
	dsc.text_local = 1;

	if (e->text[0] == '\0' && e->placeholder) {
		dsc.text = e->placeholder;
		dsc.color = lv_color_hex(UI_COLOR_DIM);
		lv_area_t a = { o.x, o.y, o.x + w - 1, o.y + lh - 1 };
		lv_draw_label(layer, &dsc, &a);
		dsc.color = lv_obj_get_style_text_color(&e->obj, LV_PART_MAIN);
	}

	/* The lines in view */
	lv_area_t view;
	lv_obj_get_coords(&e->obj, &view);
	uint32_t first = view.y1 > o.y ? (uint32_t)((view.y1 - o.y) / lh) : 0;
	for (uint32_t li = first; li < e->line_count && o.y + (int32_t)li * lh <= view.y2; li++) {
		const struct line *l = &e->lines[li];
		const char *text = e->doc->blocks[l->block].text;
		int32_t y = o.y + (int32_t)li * lh;
		for (uint32_t k = 0; k < l->frag_count; k++) {
			const struct frag *f = &e->frags[l->frag + k];
			dsc.font = mark_font(e, f->marks);
			dsc.decor = f->marks & RICH_UNDERLINE ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE;
			dsc.text = text + f->start;
			dsc.text_length = f->end - f->start;
			lv_area_t a = { o.x + f->x, y, o.x + f->x + text_width(dsc.font, text, f->start, f->end), y + lh - 1 };
			lv_draw_label(layer, &dsc, &a);
		}
	}

	e->blink_on = focused(e) && lv_tick_elaps(e->blink_start) / RICH_BLINK_MS % 2 == 0;
	if (e->blink_on) {
		int32_t x;
		uint32_t li = cursor_line(e, &x);
		lv_draw_rect_dsc_t r;
		lv_draw_rect_dsc_init(&r);
		r.bg_color = lv_color_hex(UI_COLOR_ACCENT);
		r.bg_opa = LV_OPA_COVER;
		lv_area_t a = { o.x + x - RICH_CURSOR_W / 2, o.y + (int32_t)li * lh,
		                o.x + x - RICH_CURSOR_W / 2 + RICH_CURSOR_W - 1, o.y + (int32_t)(li + 1) * lh - 1 };
		/* Only within what is redrawn: the GLES draw unit fills a rectangle outside it
		 * over all of it (the old cursor's place, redrawn after it moved) */
		lv_area_t in;
		if (lv_area_intersect(&in, &a, &layer->_clip_area))
			lv_draw_rect(layer, &r, &a);
	}
}

static void rich_edit_event(const lv_obj_class_t *cls, lv_event_t *ev)
{
	LV_UNUSED(cls);
	if (lv_obj_event_base(&ui_rich_edit_class, ev) != LV_RESULT_OK)
		return;
	rich_edit_t *e = (rich_edit_t *)lv_event_get_current_target(ev);
	switch (lv_event_get_code(ev)) {
	case LV_EVENT_GET_SELF_SIZE: {
		lv_point_t *p = lv_event_get_param(ev);
		ensure_layout(e);
		p->y = LV_MAX(p->y, (int32_t)e->line_count * line_height(e));
		break;
	}
	case LV_EVENT_SIZE_CHANGED:
	case LV_EVENT_STYLE_CHANGED:
		e->layout_w = -1;
		lv_obj_refresh_self_size(&e->obj);
		break;
	case LV_EVENT_DRAW_MAIN:
		draw(e, lv_event_get_layer(ev));
		break;
	case LV_EVENT_CLICKED: {
		lv_point_t p;
		lv_indev_get_point(lv_indev_active(), &p);
		uint32_t pos = pos_at(e, p);
		lv_textarea_set_cursor_pos(e->ta, (int32_t)lv_text_encoded_get_char_id(e->text, pos));
		cursor_moved(e, pos);
		break;
	}
	default:
		break;
	}
}

const lv_obj_class_t ui_rich_edit_class = {
	.base_class = &lv_obj_class,
	.constructor_cb = rich_edit_constructor,
	.destructor_cb = rich_edit_destructor,
	.event_cb = rich_edit_event,
	.width_def = LV_PCT(100),
	.height_def = LV_SIZE_CONTENT,
	.instance_size = sizeof(rich_edit_t),
	.name = "rich_edit",
};

lv_obj_t *ui_rich_edit_create(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *obj = lv_obj_class_create_obj(&ui_rich_edit_class, parent);
	lv_obj_class_init_obj(obj);
	rich_edit_t *e = (rich_edit_t *)obj;
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_scroll_dir(obj, LV_DIR_VER);
	lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);

	/* Typed into, out of sight: no larger than a line, its own text not drawn (nor found
	 * by the tests: hidden), not tapped */
	e->ta = lv_textarea_create(obj);
	lv_obj_set_floating(e->ta, true);
	lv_obj_set_clickable(e->ta, false);
	lv_obj_set_scroll_on_focus(e->ta, false);
	lv_obj_set_size(e->ta, LV_PCT(100), 1);
	lv_obj_set_style_opa(e->ta, LV_OPA_TRANSP, 0);
	lv_obj_set_hidden(lv_textarea_get_label(e->ta), true);
	lv_obj_add_event_cb(e->ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, e);
	lv_obj_add_event_cb(e->ta, ta_state_cb, LV_EVENT_STATE_CHANGED, e);
	lv_obj_add_event_cb(e->ta, cb, LV_EVENT_READY, user_data);
	lv_obj_add_event_cb(e->ta, cb, LV_EVENT_CANCEL, user_data);
	return obj;
}

lv_obj_t *ui_rich_edit_textarea(lv_obj_t *edit)
{
	return ((rich_edit_t *)edit)->ta;
}

struct rich_doc *ui_rich_edit_doc(lv_obj_t *edit)
{
	return ((rich_edit_t *)edit)->doc;
}

const char *ui_rich_edit_text(lv_obj_t *edit)
{
	return ((rich_edit_t *)edit)->text;
}

void ui_rich_edit_set_placeholder(lv_obj_t *edit, const char *text)
{
	rich_edit_t *e = (rich_edit_t *)edit;
	free(e->placeholder);
	e->placeholder = strdup(text);
	lv_obj_invalidate(edit);
}

/* The toolbar */

static void toolbar_button_cb(lv_event_t *ev)
{
	lv_obj_t *btn = lv_event_get_current_target(ev);
	rich_edit_t *e = lv_obj_get_user_data(lv_obj_get_parent(btn));
	if (!e)
		return;
	e->typing ^= (unsigned)(uintptr_t)lv_obj_get_user_data(btn);
	toolbar_update(e);
}

static void toolbar_delete_cb(lv_event_t *ev)
{
	rich_edit_t *e = lv_obj_get_user_data(lv_event_get_current_target(ev));
	if (e)
		e->toolbar = NULL;
}

lv_obj_t *ui_rich_toolbar_create(lv_obj_t *parent, lv_obj_t *edit)
{
	static const struct {
		const char *symbol;
		unsigned marks;
	} buttons[] = {
		{ UI_SYMBOL_BOLD, RICH_BOLD },
		{ UI_SYMBOL_ITALIC, RICH_ITALIC },
		{ UI_SYMBOL_UNDERLINE, RICH_UNDERLINE },
	};
	rich_edit_t *e = (rich_edit_t *)edit;
	lv_obj_t *bar = lv_obj_create(parent);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), RICH_TOOLBAR_H);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_style_pad_hor(bar, 10, 0);
	lv_obj_set_style_pad_ver(bar, 8, 0);
	lv_obj_set_style_pad_column(bar, 10, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollable(bar, false);

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		lv_obj_t *btn = lv_obj_create(bar);
		lv_obj_remove_style_all(btn);
		lv_obj_set_size(btn, RICH_BUTTON_W, LV_PCT(100));
		lv_obj_set_style_radius(btn, 12, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT_PRESSED), LV_STATE_CHECKED | LV_STATE_PRESSED);
		/* The symbol's color, which the label takes */
		lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_CHECKED);
		lv_obj_set_user_data(btn, (void *)(uintptr_t)buttons[i].marks);
		lv_obj_add_event_cb(btn, toolbar_button_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *label = lv_label_create(btn);
		lv_label_set_text_static(label, buttons[i].symbol);
		lv_obj_set_style_text_font(label, &ui_font_26, 0);
		lv_obj_center(label);
	}

	lv_obj_set_user_data(bar, e);
	lv_obj_add_event_cb(bar, toolbar_delete_cb, LV_EVENT_DELETE, NULL);
	e->toolbar = bar;
	toolbar_update(e);
	return bar;
}
