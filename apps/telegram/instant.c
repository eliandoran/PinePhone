/*
 * Telegram's Instant View: a page Telegram made an article of (a link preview's
 * instant_view_version), asked for with getWebPageInstantView and shown in the app's
 * widgets, one per page block, in a scrolling column under the site's name:
 * - titles, headers, paragraphs, captions: a label, or a span group when styles mix
 *   (links underlined in the accent color, fixed-width text in monospace, struck);
 *   bold and italic are plain, the app has no such fonts
 * - lists: each item's label ("•", "1.") before its blocks; quotes: a bar on their
 *   left; preformatted text: monospace on a card; dividers: a line
 * - photos: the smallest size at least as wide as the screen, downloaded by TDLib and
 *   decoded into a texture one per poll, as the chat's; their caption under them
 * - covers, collages, slideshows, embedded posts, details: their blocks one after the
 *   other; tables: a row of cells per row
 * - what it cannot play or show (videos, audio, maps, embedded pages): their kind and
 *   caption; unsupported blocks: nothing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define PAD 24          /* around the article */
#define DEPTH 8         /* nested blocks drawn; deeper ones are left out */
#define PHOTOS 64       /* drawn per page */
#define RUNS 128        /* styled runs of a text; more go in the last */
#define MAX_TEXT 3000   /* bytes of a block's text: cut there (a label is a texture as large) */
#define LIST_INDENT 40  /* a list item's label and the gap after it */

/* Styles of a run of rich text */
enum { LINK = 1, FIXED = 2, STRIKE = 4, UNDERLINE = 8 };

struct run {
	char *text;
	int style;
};

struct runs {
	struct run run[RUNS];
	unsigned count;
	size_t len;
};

static struct {
	lv_obj_t *scr, *column, *status;
	char *url;
	int32_t width; /* of the column's content */
	struct photo {
		int file;   /* TDLib file id */
		char *path; /* once downloaded */
		int w;      /* on screen */
		unsigned tex;
		bool bad;   /* could not be decoded */
		lv_obj_t *box;
	} photos[PHOTOS];
	unsigned photo_count;
} iv;

/* Rich text */

static void run_add(struct runs *rs, const char *text, int style)
{
	size_t n = strlen(text);
	if (!n || rs->len >= MAX_TEXT)
		return;
	if (rs->len + n > MAX_TEXT) {
		n = MAX_TEXT - rs->len;
		while (n && ((unsigned char)text[n] & 0xc0) == 0x80)
			n--;
	}
	struct run *last = rs->count ? &rs->run[rs->count - 1] : NULL;
	if (!last || (last->style != style && rs->count < RUNS)) {
		last = &rs->run[rs->count++];
		last->text = strndup(text, n);
		last->style = style;
	} else {
		/* The same style, or no room for another run */
		size_t had = strlen(last->text);
		char *t = realloc(last->text, had + n + 1);
		if (!t)
			return;
		memcpy(t + had, text, n);
		t[had + n] = '\0';
		last->text = t;
	}
	rs->len += n;
}

/* A RichText's runs: links, fixed-width and struck text styled, the rest plain */
static void rich(struct runs *rs, const struct json *t, int style)
{
	const char *type = json_type(t);
	if (!strcmp(type, "richTextPlain")) {
		run_add(rs, json_str(json_get(t, "text"), ""), style);
	} else if (!strcmp(type, "richTexts")) {
		const struct json *texts = json_get(t, "texts");
		for (const struct json *c = texts ? texts->child : NULL; c; c = c->next)
			rich(rs, c, style);
	} else if (!strcmp(type, "richTextUrl") || !strcmp(type, "richTextEmailAddress") ||
	           !strcmp(type, "richTextPhoneNumber") || !strcmp(type, "richTextAnchorLink") ||
	           !strcmp(type, "richTextReferenceLink") || !strcmp(type, "richTextMention") ||
	           !strcmp(type, "richTextMentionName")) {
		rich(rs, json_get(t, "text"), style | LINK);
	} else if (!strcmp(type, "richTextFixed")) {
		rich(rs, json_get(t, "text"), style | FIXED);
	} else if (!strcmp(type, "richTextStrikethrough")) {
		rich(rs, json_get(t, "text"), style | STRIKE);
	} else if (!strcmp(type, "richTextUnderline")) {
		rich(rs, json_get(t, "text"), style | UNDERLINE);
	} else if (!strcmp(type, "richTextCustomEmoji")) {
		run_add(rs, json_str(json_get(t, "alternative_text"), ""), style);
	} else if (!strcmp(type, "richTextMathematicalExpression")) {
		run_add(rs, json_str(json_get(t, "expression"), ""), style | FIXED);
	} else {
		/* Bold, italic, marked, a hashtag...: their text; icons, anchors: nothing */
		const struct json *inner = json_get(t, "text");
		if (inner && inner->kind == JSON_OBJECT)
			rich(rs, inner, style);
	}
}

static void runs_free(struct runs *rs)
{
	for (unsigned i = 0; i < rs->count; i++)
		free(rs->run[i].text);
	rs->count = 0;
	rs->len = 0;
}

/* Emoji composed: a copy for the widget to copy again */
static void set_text(lv_obj_t *label, lv_obj_t *group, lv_span_t *span, const char *text)
{
	char *composed = ui_emoji_compose(text);
	if (label)
		lv_label_set_text(label, composed ? composed : text);
	else
		lv_spangroup_set_span_text(group, span, composed ? composed : text);
	free(composed);
}

/* A rich text as a label if it has one style, a span group otherwise; NULL if empty */
static lv_obj_t *text_add(lv_obj_t *parent, const struct json *t, const lv_font_t *font, uint32_t color,
                          int32_t width)
{
	struct runs rs = { 0 };
	rich(&rs, t, 0);
	if (!rs.count)
		return NULL;
	lv_obj_t *obj;
	if (rs.count == 1 && !rs.run[0].style) {
		obj = lv_label_create(parent);
		set_text(obj, NULL, NULL, rs.run[0].text);
		lv_obj_set_width(obj, width);
	} else {
		obj = lv_spangroup_create(parent);
		for (unsigned i = 0; i < rs.count; i++) {
			lv_span_t *span = lv_spangroup_add_span(obj);
			set_text(NULL, obj, span, rs.run[i].text);
			lv_style_t *style = lv_span_get_style(span);
			int s = rs.run[i].style;
			if (s & FIXED)
				lv_style_set_text_font(style, &ui_font_mono_20);
			if (s & LINK)
				lv_style_set_text_color(style, lv_color_hex(UI_COLOR_ACCENT));
			if (s & (LINK | UNDERLINE))
				lv_style_set_text_decor(style, LV_TEXT_DECOR_UNDERLINE);
			if (s & STRIKE)
				lv_style_set_text_decor(style, LV_TEXT_DECOR_STRIKETHROUGH);
		}
		lv_spangroup_refresh(obj);
		lv_obj_set_size(obj, width, LV_SIZE_CONTENT);
	}
	lv_obj_set_style_text_font(obj, font, 0);
	lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
	runs_free(&rs);
	return obj;
}

/* Blocks */

static lv_obj_t *column_create(lv_obj_t *parent, int32_t width, int32_t gap)
{
	lv_obj_t *col = ui_box(parent);
	lv_obj_set_size(col, width, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, gap, 0);
	return col;
}

/* A caption (pageBlockCaption): its text, then its credit, small and dim */
static void caption_add(lv_obj_t *parent, const struct json *caption, int32_t width)
{
	text_add(parent, json_get(caption, "text"), UI_FONT_CAPTION, UI_COLOR_DIM, width);
	lv_obj_t *credit = text_add(parent, json_get(caption, "credit"), UI_FONT_CAPTION, UI_COLOR_DIM, width);
	if (credit)
		lv_obj_set_style_text_opa(credit, LV_OPA_70, 0);
}

static void label_add(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color, int32_t width)
{
	lv_obj_t *label = lv_label_create(parent);
	set_text(label, NULL, NULL, text);
	lv_obj_set_width(label, width);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

/* A photo: the smallest size at least as wide as shown (the largest otherwise),
 * downloaded, then decoded by instant_poll() */
static void photo_add(lv_obj_t *parent, const struct json *photo, int32_t width)
{
	const struct json *sizes = json_get(photo, "sizes"), *best = NULL, *largest = NULL;
	for (const struct json *s = sizes ? sizes->child : NULL; s; s = s->next) {
		int w = json_int(json_get(s, "width"), 0);
		if (!largest || w > json_int(json_get(largest, "width"), 0))
			largest = s;
		if (w >= width && (!best || w < json_int(json_get(best, "width"), 0)))
			best = s;
	}
	if (!best)
		best = largest;
	int w = json_int(json_get(best, "width"), 0), h = json_int(json_get(best, "height"), 0);
	if (w <= 0 || h <= 0 || iv.photo_count == PHOTOS)
		return;
	struct photo *p = &iv.photos[iv.photo_count++];
	memset(p, 0, sizeof(*p));
	p->w = w < width ? w : width;
	p->box = ui_box(parent);
	lv_obj_set_size(p->box, p->w, (int32_t)((long long)h * p->w / w));
	lv_obj_set_style_bg_opa(p->box, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->box, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_t *icon = lv_label_create(p->box);
	lv_label_set_text_static(icon, LV_SYMBOL_IMAGE);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_center(icon);

	const struct json *file = json_get(best, "photo");
	p->file = json_int(json_get(file, "id"), 0);
	const struct json *local = json_get(file, "local");
	const char *path = json_str(json_get(local, "path"), "");
	if (json_bool(json_get(local, "is_downloading_completed")) && path[0]) {
		p->path = strdup(path);
		return;
	}
	char buf[160];
	snprintf(buf, sizeof(buf), "{\"@type\":\"downloadFile\",\"file_id\":%d,\"priority\":1,"
		"\"offset\":0,\"limit\":0,\"synchronous\":false,\"@extra\":\"download\"}", p->file);
	td_request(buf);
}

/* What is not shown here: its kind, dim, then its caption */
static void media_add(lv_obj_t *parent, const char *symbol, const char *kind, const struct json *caption,
                      int32_t width)
{
	char text[96];
	snprintf(text, sizeof(text), "%s  %s", symbol, kind);
	label_add(parent, text, UI_FONT_BODY, UI_COLOR_DIM, width);
	caption_add(parent, caption, width);
}

static void blocks_add(lv_obj_t *parent, const struct json *blocks, int32_t width, int depth);

static void block_add(lv_obj_t *parent, const struct json *b, int32_t width, int depth)
{
	const char *type = json_type(b);
	lv_obj_t *obj = NULL;
	if (!strcmp(type, "pageBlockTitle")) {
		text_add(parent, json_get(b, "title"), UI_FONT_HEADLINE, UI_COLOR_TEXT, width);
	} else if (!strcmp(type, "pageBlockSubtitle")) {
		text_add(parent, json_get(b, "subtitle"), UI_FONT_TITLE, UI_COLOR_DIM, width);
	} else if (!strcmp(type, "pageBlockKicker")) {
		text_add(parent, json_get(b, "kicker"), UI_FONT_CAPTION, UI_COLOR_ACCENT, width);
	} else if (!strcmp(type, "pageBlockAuthorDate")) {
		/* "Author · 15 March 2021" */
		struct runs rs = { 0 };
		rich(&rs, json_get(b, "author"), 0);
		char line[256] = "";
		size_t n = 0;
		for (unsigned i = 0; i < rs.count && n < sizeof(line); i++)
			n += snprintf(line + n, sizeof(line) - n, "%s", rs.run[i].text);
		runs_free(&rs);
		time_t t = json_int(json_get(b, "publish_date"), 0);
		if (t) {
			struct tm tm;
			localtime_r(&t, &tm);
			if (n < sizeof(line))
				snprintf(line + n, sizeof(line) - n, "%s%d %s %d", n ? " \xc2\xb7 " : "", tm.tm_mday,
				         ui_month_name(tm.tm_mon, false), tm.tm_year + 1900);
		}
		if (line[0])
			label_add(parent, line, UI_FONT_CAPTION, UI_COLOR_DIM, width);
	} else if (!strcmp(type, "pageBlockHeader")) {
		obj = text_add(parent, json_get(b, "header"), UI_FONT_TITLE, UI_COLOR_TEXT, width);
		if (obj)
			lv_obj_set_style_margin_top(obj, 16, 0);
	} else if (!strcmp(type, "pageBlockSubheader")) {
		obj = text_add(parent, json_get(b, "subheader"), UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width);
		if (obj)
			lv_obj_set_style_margin_top(obj, 8, 0);
	} else if (!strcmp(type, "pageBlockSectionHeading")) {
		int size = json_int(json_get(b, "size"), 3);
		obj = text_add(parent, json_get(b, "text"), size <= 1 ? UI_FONT_HEADLINE : size == 2 ? UI_FONT_TITLE :
		               UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width);
		if (obj)
			lv_obj_set_style_margin_top(obj, 8, 0);
	} else if (!strcmp(type, "pageBlockParagraph")) {
		text_add(parent, json_get(b, "text"), UI_FONT_BODY, UI_COLOR_TEXT, width);
	} else if (!strcmp(type, "pageBlockFooter")) {
		text_add(parent, json_get(b, "footer"), UI_FONT_CAPTION, UI_COLOR_DIM, width);
	} else if (!strcmp(type, "pageBlockPreformatted")) {
		lv_obj_t *card = column_create(parent, width, 0);
		lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_style_radius(card, 12, 0);
		lv_obj_set_style_pad_all(card, 12, 0);
		text_add(card, json_get(b, "text"), &ui_font_mono_20, UI_COLOR_TEXT, width - 24);
	} else if (!strcmp(type, "pageBlockDivider")) {
		lv_obj_t *line = ui_box(parent);
		lv_obj_set_size(line, width, 2);
		lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(line, lv_color_hex(UI_COLOR_LINE), 0);
		lv_obj_set_style_margin_ver(line, 8, 0);
	} else if (!strcmp(type, "pageBlockList")) {
		const struct json *items = json_get(b, "items");
		for (const struct json *it = items ? items->child : NULL; it; it = it->next) {
			lv_obj_t *row = ui_box(parent);
			lv_obj_set_size(row, width, LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
			lv_obj_t *mark = lv_label_create(row);
			set_text(mark, NULL, NULL, json_str(json_get(it, "label"), ""));
			lv_obj_set_width(mark, LIST_INDENT);
			lv_obj_set_style_text_font(mark, UI_FONT_BODY, 0);
			lv_obj_set_style_text_color(mark, lv_color_hex(UI_COLOR_DIM), 0);
			lv_obj_t *col = column_create(row, width - LIST_INDENT, 8);
			blocks_add(col, json_get(it, "blocks"), width - LIST_INDENT, depth + 1);
		}
	} else if (!strcmp(type, "pageBlockBlockQuote") || !strcmp(type, "pageBlockExpandableBlockQuote") ||
	           !strcmp(type, "pageBlockPullQuote")) {
		lv_obj_t *quote = column_create(parent, width, 8);
		lv_obj_set_style_border_side(quote, LV_BORDER_SIDE_LEFT, 0);
		lv_obj_set_style_border_width(quote, 4, 0);
		lv_obj_set_style_border_color(quote, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_border_opa(quote, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_left(quote, 20, 0);
		if (json_get(b, "blocks"))
			blocks_add(quote, json_get(b, "blocks"), width - 24, depth + 1);
		else
			text_add(quote, json_get(b, "text"), UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width - 24);
		text_add(quote, json_get(b, "credit"), UI_FONT_CAPTION, UI_COLOR_DIM, width - 24);
	} else if (!strcmp(type, "pageBlockPhoto")) {
		photo_add(parent, json_get(b, "photo"), width);
		caption_add(parent, json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockCover")) {
		block_add(parent, json_get(b, "cover"), width, depth + 1);
	} else if (!strcmp(type, "pageBlockCollage") || !strcmp(type, "pageBlockSlideshow")) {
		blocks_add(parent, json_get(b, "blocks"), width, depth + 1);
		caption_add(parent, json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockEmbeddedPost")) {
		label_add(parent, json_str(json_get(b, "author"), ""), UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width);
		blocks_add(parent, json_get(b, "blocks"), width, depth + 1);
		caption_add(parent, json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockDetails")) {
		text_add(parent, json_get(b, "header"), UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width);
		blocks_add(parent, json_get(b, "blocks"), width, depth + 1);
	} else if (!strcmp(type, "pageBlockTable")) {
		text_add(parent, json_get(b, "caption"), UI_FONT_CAPTION, UI_COLOR_DIM, width);
		const struct json *rows = json_get(b, "cells");
		for (const struct json *r = rows ? rows->child : NULL; r; r = r->next) {
			unsigned cells = 0;
			for (const struct json *c = r->child; c; c = c->next)
				cells++;
			if (!cells)
				continue;
			lv_obj_t *row = ui_box(parent);
			lv_obj_set_size(row, width, LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
			lv_obj_set_style_pad_column(row, 12, 0);
			int32_t cw = (width - 12 * ((int32_t)cells - 1)) / (int32_t)cells;
			for (const struct json *c = r->child; c; c = c->next) {
				lv_obj_t *cell = column_create(row, cw, 0);
				text_add(cell, json_get(c, "text"), UI_FONT_CAPTION,
				         json_bool(json_get(c, "is_header")) ? UI_COLOR_DIM : UI_COLOR_TEXT, cw);
			}
		}
	} else if (!strcmp(type, "pageBlockRelatedArticles")) {
		text_add(parent, json_get(b, "header"), UI_FONT_BODY_LARGE, UI_COLOR_TEXT, width);
		const struct json *articles = json_get(b, "articles");
		for (const struct json *a = articles ? articles->child : NULL; a; a = a->next)
			label_add(parent, json_str(json_get(a, "title"), json_str(json_get(a, "url"), "")),
			          UI_FONT_BODY, UI_COLOR_ACCENT, width);
	} else if (!strcmp(type, "pageBlockVideo") || !strcmp(type, "pageBlockAnimation")) {
		media_add(parent, LV_SYMBOL_VIDEO, _("Video"), json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockAudio") || !strcmp(type, "pageBlockVoiceNote")) {
		media_add(parent, LV_SYMBOL_AUDIO, _("Audio"), json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockDocument")) {
		media_add(parent, LV_SYMBOL_FILE, _("File"), json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockMap")) {
		media_add(parent, LV_SYMBOL_GPS, _("Map"), json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockEmbedded")) {
		media_add(parent, LV_SYMBOL_IMAGE, _("Embedded content"), json_get(b, "caption"), width);
	} else if (!strcmp(type, "pageBlockMathematicalExpression")) {
		label_add(parent, json_str(json_get(b, "expression"), ""), &ui_font_mono_20, UI_COLOR_TEXT, width);
	} else if (!strcmp(type, "pageBlockChatLink")) {
		label_add(parent, json_str(json_get(b, "title"), ""), UI_FONT_BODY, UI_COLOR_ACCENT, width);
	}
	/* Anchors, button rows, unsupported blocks: nothing */
	LV_UNUSED(depth);
}

static void blocks_add(lv_obj_t *parent, const struct json *blocks, int32_t width, int depth)
{
	if (depth >= DEPTH)
		return;
	for (const struct json *b = blocks ? blocks->child : NULL; b; b = b->next)
		block_add(parent, b, width, depth);
}

/* The answer: the article, or why there is none */
void instant_answer(const struct json *u)
{
	const char *url = json_str(json_get(u, "@extra"), "") + strlen("instant view ");
	if (!iv.scr || !iv.url || strcmp(url, iv.url))
		return;
	if (strcmp(json_type(u), "webPageInstantView")) {
		printf("telegram: getWebPageInstantView: %s\n", json_str(json_get(u, "message"), ""));
		lv_label_set_text_static(iv.status, _("Cannot load this page"));
		return;
	}
	lv_obj_delete(iv.status);
	iv.status = NULL;
	blocks_add(iv.column, json_get(u, "blocks"), iv.width, 0);
}

/* A downloaded file: its path to the photos showing it */
void instant_file(const struct json *file)
{
	const struct json *local = json_get(file, "local");
	const char *path = json_str(json_get(local, "path"), "");
	if (!iv.scr || !json_bool(json_get(local, "is_downloading_completed")) || !path[0])
		return;
	int id = json_int(json_get(file, "id"), 0);
	for (unsigned i = 0; i < iv.photo_count; i++)
		if (iv.photos[i].file == id && !iv.photos[i].path)
			iv.photos[i].path = strdup(path);
}

/* The first downloaded photo not decoded yet: one per poll, the UI waits for it */
void instant_poll(void)
{
	for (unsigned i = 0; iv.scr && i < iv.photo_count; i++) {
		struct photo *p = &iv.photos[i];
		if (!p->path || p->tex || p->bad)
			continue;
		p->tex = jpeg_texture(p->path, p->w, false);
		p->bad = !p->tex;
#if LV_USE_3DTEXTURE
		if (p->tex) {
			lv_obj_t *quad = lv_3dtexture_create(p->box);
			lv_obj_set_size(quad, LV_PCT(100), LV_PCT(100));
			lv_3dtexture_set_src(quad, p->tex);
			lv_obj_set_hidden(lv_obj_get_child(p->box, 0), true);
		}
#endif
		return;
	}
}

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	for (unsigned i = 0; i < iv.photo_count; i++) {
		ui_texture_delete(iv.photos[i].tex);
		free(iv.photos[i].path);
	}
	free(iv.url);
	memset(&iv, 0, sizeof(iv));
}

void instant_open(const char *url, const char *site)
{
	if (iv.scr)
		return;
	lv_obj_t *body;
	/* The title must stay valid: the message's can go */
	iv.scr = telegram_screen(ui_intern(site && site[0] ? site : _("Instant View")), NULL, &body);
	lv_obj_add_event_cb(iv.scr, view_delete_cb, LV_EVENT_DELETE, NULL);
	iv.url = strdup(url);
	iv.width = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * PAD;
	iv.column = column_create(body, LV_PCT(100), 16);
	lv_obj_set_style_pad_hor(iv.column, PAD, 0);
	lv_obj_set_style_pad_ver(iv.column, PAD, 0);
	iv.status = lv_label_create(iv.column);
	lv_label_set_text_static(iv.status, _("Loading..."));
	lv_obj_set_style_text_color(iv.status, lv_color_hex(UI_COLOR_DIM), 0);

	size_t n = strlen(url);
	char *escaped = malloc(n * 6 + 1), *buf = malloc(2 * n * 6 + 160);
	if (escaped && buf) {
		json_escape(escaped, n * 6 + 1, url);
		/* NON-NLS-BEGIN: TDLib's JSON */
		snprintf(buf, 2 * n * 6 + 160, "{\"@type\":\"getWebPageInstantView\",\"url\":\"%s\",\"only_local\":false,"
			 "\"@extra\":\"instant view %s\"}", escaped, escaped);
		/* NON-NLS-END */
		td_request(buf);
	}
	free(escaped);
	free(buf);
	ui_push(iv.scr);
}
