/*
 * A pull request's files (its Files tab, pr.c): each changed file a card with its
 * name (a renamed one's old and new), how many lines it adds and removes, then its
 * diff, GitHub's patch (a unified diff without the files' headers), in monospace:
 * the added lines on green, the removed on red, the hunks' headers on the accent
 * color, its code highlighted by its language (highlight.c), scrolled sideways
 * rather than wrapped. A label each CHUNK lines over strips
 * of color, not a label per line: each label is a texture. A long diff is folded,
 * shown or hidden by tapping its file's name. What has no patch says why: a binary
 * file, a rename alone, one too large for GitHub to give.
 */

#define _GNU_SOURCE /* strchrnul */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "github.h"

#define CHUNK 80        /* lines in a label */
#define MAX_LINES 3000  /* of a file's diff shown, then how many more */
#define FOLD_LINES 150  /* a diff longer than that is folded */
#define OPEN_LINES 600  /* and the others once that many are shown */
#define TAB_SPACES 4

enum line { CONTEXT, ADDED, REMOVED, HUNK };

static enum line line_kind(const char *s)
{
	switch (*s) {
	case '+': return ADDED;
	case '-': return REMOVED;
	case '@': return HUNK;
	default: return CONTEXT;
	}
}

static lv_color_t line_color(enum line kind)
{
	static const uint32_t colors[] = {[ADDED] = UI_COLOR_GREEN, [REMOVED] = UI_COLOR_RED, [HUNK] = UI_COLOR_ACCENT};
	return lv_color_mix(lv_color_hex(colors[kind]), lv_color_hex(UI_COLOR_CARD), LV_OPA_30);
}

/* Characters in the line from s to its end, tabs as TAB_SPACES */
static int line_chars(const char *s, const char *end)
{
	int n = 0;
	for (; s < end; s++)
		if (*s == '\t')
			n += TAB_SPACES;
		else if (((unsigned char)*s & 0xc0) != 0x80)
			n++;
	return n;
}

/* A label of `count` lines from `text` over a strip for each run of lines of a kind
 * other than context, `w` wide */
static void chunk_create(lv_obj_t *parent, const char *text, size_t len, const enum line *kinds, int count, int32_t w,
			 bool recolor)
{
	int32_t line_h = lv_font_get_line_height(&ui_font_mono_20);
	lv_obj_t *chunk = ui_box(parent);
	lv_obj_set_size(chunk, w, count * line_h);
	for (int i = 0; i < count;) {
		int j = i;
		while (j < count && kinds[j] == kinds[i])
			j++;
		if (kinds[i] != CONTEXT) {
			lv_obj_t *strip = ui_box(chunk);
			lv_obj_set_pos(strip, 0, i * line_h);
			lv_obj_set_size(strip, w, (j - i) * line_h);
			lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(strip, line_color(kinds[i]), 0);
		}
		i = j;
	}
	lv_obj_t *label = lv_label_create(chunk);
	lv_label_set_recolor(label, recolor);
	lv_label_set_text_fmt(label, "%.*s", (int)len, text);
	lv_obj_set_width(label, w);
	lv_obj_set_style_pad_left(label, 12, 0);
	lv_obj_set_style_text_font(label, &ui_font_mono_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
}

/* The patch's lines in `box`, at least `min_w` wide, highlighted as `lang` (NULL:
 * not); returns how many */
static int diff_create(lv_obj_t *box, const char *patch, int32_t min_w, const struct lang *lang)
{
	/* Tabs as spaces, then the lines and their kinds */
	size_t n = 0;
	for (const char *s = patch; *s; s++)
		n += *s == '\t' ? TAB_SPACES : 1;
	char *text = malloc(n + 1), *o = text;
	int lines = 1, chars = 0;
	for (const char *s = patch; *s; s++)
		lines += *s == '\n';
	enum line *kinds = malloc(lines * sizeof(*kinds));
	if (!text || !kinds) {
		free(text);
		free(kinds);
		return 0;
	}
	int count = 0;
	for (const char *s = patch; *s && count < MAX_LINES;) {
		const char *end = strchrnul(s, '\n');
		kinds[count++] = line_kind(s);
		int c = line_chars(s, end);
		chars = c > chars ? c : chars;
		for (; s < end; s++) {
			if (*s == '\t') {
				memset(o, ' ', TAB_SPACES);
				o += TAB_SPACES;
			} else if (*s != '\r') {
				*o++ = *s;
			}
		}
		*o++ = '\n';
		if (*s)
			s++;
	}
	if (o > text)
		o--; /* the last line's \n */
	*o = '\0';

	/* Recolor marks: the lines and their widths stay */
	char *marked = lang ? highlight(lang, text) : NULL;
	if (marked) {
		free(text);
		text = marked;
	}

	int32_t w = chars * lv_font_get_glyph_width(&ui_font_mono_20, 'm', 0) + 24;
	w = w > min_w ? w : min_w;
	/* CHUNK lines a label */
	const char *s = text;
	for (int i = 0; i < count; i += CHUNK) {
		int k = count - i < CHUNK ? count - i : CHUNK;
		const char *end = s;
		for (int l = 0; l < k; l++) {
			end = strchrnul(end, '\n');
			if (l + 1 < k && *end)
				end++;
		}
		chunk_create(box, s, end - s, kinds + i, k, w, marked != NULL);
		s = *end ? end + 1 : end;
	}
	free(text);
	free(kinds);
	return lines;
}

static void fold_cb(lv_event_t *e)
{
	lv_obj_t *diff = lv_event_get_user_data(e);
	lv_obj_set_hidden(diff, !lv_obj_is_hidden(diff));
}

/* A file's card in `col`: its name and counts, then its diff (folded if `fold`, or
 * when long); returns the diff's lines */
static int file_create(lv_obj_t *col, const struct json *f, bool fold)
{
	const char *name = json_str(json_get(f, "filename"), "");
	const char *previous = json_str(json_get(f, "previous_filename"), NULL);
	const char *status = json_str(json_get(f, "status"), "");
	const char *patch = json_str(json_get(f, "patch"), NULL);
	long long added = json_int(json_get(f, "additions"), 0), removed = json_int(json_get(f, "deletions"), 0);

	lv_obj_t *card = ui_card_create(col);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_set_style_pad_row(card, 0, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

	/* Its name, then the counts */
	lv_obj_t *head = ui_box(card);
	lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(head, 20, 0);
	lv_obj_set_style_pad_column(head, 12, 0);
	lv_obj_t *label = lv_label_create(head);
	if (previous && strcmp(previous, name))
		ui_set_text_fmt(label, "%s " LV_SYMBOL_RIGHT " %s", previous, name);
	else
		ui_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	if (added) {
		lv_obj_t *l = lv_label_create(head);
		ui_set_text_fmt(l, "+%lld", added);
		lv_obj_set_style_text_font(l, UI_FONT_CAPTION, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_GREEN), 0);
	}
	if (removed) {
		lv_obj_t *l = lv_label_create(head);
		ui_set_text_fmt(l, "-%lld", removed);
		lv_obj_set_style_text_font(l, UI_FONT_CAPTION, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(UI_COLOR_RED), 0);
	}

	if (!patch || !patch[0]) {
		const char *why = !strcmp(status, "renamed") && !added && !removed ? _("File renamed without changes")
				  : !added && !removed				     ? _("Binary file not shown")
										     : _("Diff too large to show");
		lv_obj_t *l = ui_dim_label(card, why, UI_FONT_CAPTION);
		lv_obj_set_style_pad_hor(l, 20, 0);
		lv_obj_set_style_pad_bottom(l, 20, 0);
		return 0;
	}

	/* The diff, scrolled sideways */
	lv_obj_t *diff = ui_box(card);
	lv_obj_set_size(diff, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_bottom(diff, 24, 0); /* clear of the card's rounded corners */
	lv_obj_set_flex_flow(diff, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(diff, true);
	lv_obj_set_clickable(diff, true); /* pressed, or the drag goes to the column */
	lv_obj_set_scroll_dir(diff, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(diff, LV_SCROLLBAR_MODE_OFF);
	lv_obj_update_layout(col);
	int lines = diff_create(diff, patch, lv_obj_get_content_width(col), highlight_lang(name));
	if (lines > MAX_LINES) {
		lv_obj_t *l = ui_dim_label(diff, "", UI_FONT_CAPTION);
		ui_set_text_fmt(l, P_("%d more line", "%d more lines", lines - MAX_LINES), lines - MAX_LINES);
		lv_obj_set_style_pad_all(l, 12, 0);
	}
	lv_obj_set_hidden(diff, fold || lines > FOLD_LINES);
	lv_obj_set_clickable(head, true);
	lv_obj_add_event_cb(head, fold_cb, LV_EVENT_CLICKED, diff);
	return lv_obj_is_hidden(diff) ? 0 : lines;
}

void files_show(lv_obj_t *col, const struct json *files)
{
	int shown = 0;
	for (const struct json *f = files ? files->child : NULL; f; f = f->next)
		shown += file_create(col, f, shown > OPEN_LINES);
}
