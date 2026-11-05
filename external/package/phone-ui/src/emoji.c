/*
 * Color emoji: fonts whose glyphs are Noto's emoji images, between our ui_font_*
 * and LVGL's Montserrat in the fallback chain (gen-fonts.sh). The images are
 * 64 x 64 PNGs in /usr/share/emoji/<code point>.png (package noto-emoji),
 * decoded and scaled down to the font's size the first time they are drawn, and
 * kept. Labels are rendered on the CPU, which draws image glyphs in their colors.
 *
 * Sequences (flags, keycaps, joined emoji like 👩‍💻) are several code points:
 * ui_emoji_compose() replaces those that have an image with one character of
 * Supplementary Private Use Area-A, which these fonts map to the image. The
 * files are named after the sequence without U+FE0F. Skin tones have no images
 * (7 MB): they are dropped and the emoji shown in the default color. Joiners and
 * modifiers left in the text take no space.
 *
 * The browser's build (../wasm/emoji.c) fetches the images as they are first drawn:
 * ui_emoji_reload() looks again for those that were missing.
 */

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"

/* The tests' build sets it to the same images in the Nix store */
#ifndef EMOJI_DIR
#define EMOJI_DIR "/usr/share/emoji"
#endif
#define BUCKETS 512
#define SEQ_FIRST 0xf0000 /* first character standing for a sequence */
#define SEQ_MAX 8         /* code points in a sequence */

/* Sequences seen so far, by file name; never freed */
static char **seqs;
static unsigned seq_count;

struct emoji {
	uint32_t cp;
	uint16_t size;
	lv_draw_buf_t *buf; /* NULL: no image for this code point */
	struct emoji *next;
};

static struct emoji *table[BUCKETS];

/* Box filter in premultiplied alpha (no dark fringes), then straight alpha */
static lv_draw_buf_t *scale(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t size)
{
	lv_draw_buf_t *buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_ARGB8888, 0);
	if (!buf)
		return NULL;
	for (uint32_t y = 0; y < size; y++) {
		uint8_t *out = buf->data + y * buf->header.stride;
		uint32_t y0 = y * sh / size, y1 = (y + 1) * sh / size;
		for (uint32_t x = 0; x < size; x++) {
			uint32_t x0 = x * sw / size, x1 = (x + 1) * sw / size;
			uint32_t sum[4] = { 0 }, n = 0;
			for (uint32_t sy = y0; sy < y1; sy++)
				for (uint32_t sx = x0; sx < x1; sx++, n++) {
					const uint8_t *p = src + (sy * sw + sx) * 4;
					for (int i = 0; i < 3; i++)
						sum[i] += p[i] * p[3];
					sum[3] += p[3];
				}
			for (int i = 0; i < 3; i++)
				out[x * 4 + i] = sum[3] ? sum[i] / sum[3] : 0;
			out[x * 4 + 3] = n ? sum[3] / n : 0;
		}
	}
	return buf;
}

static lv_draw_buf_t *load(uint32_t cp, uint32_t size)
{
	char path[256];
	if (cp >= SEQ_FIRST && cp - SEQ_FIRST < seq_count)
		snprintf(path, sizeof(path), EMOJI_DIR "/%s.png", seqs[cp - SEQ_FIRST]);
	else
		snprintf(path, sizeof(path), EMOJI_DIR "/%x.png", (unsigned)cp);
	png_image img = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_file(&img, path))
		return NULL;
	/* LVGL's ARGB8888 is B, G, R, A in memory */
	img.format = PNG_FORMAT_BGRA;
	uint8_t *pixels = malloc(PNG_IMAGE_SIZE(img));
	lv_draw_buf_t *buf = NULL;
	if (pixels && png_image_finish_read(&img, NULL, pixels, 0, NULL))
		buf = scale(pixels, img.width, img.height, size);
	else
		png_image_free(&img);
	free(pixels);
	/* Never changes: the GPU draw unit can cache it */
	if (buf)
		lv_draw_buf_clear_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
	return buf;
}

/* Kept, also when missing (never freed: labels may still show them) */
static lv_draw_buf_t *emoji_get(uint32_t cp, uint32_t size)
{
	struct emoji **bucket = &table[(cp * 31 + size) % BUCKETS];
	for (struct emoji *e = *bucket; e; e = e->next)
		if (e->cp == cp && e->size == size)
			return e->buf;
	struct emoji *e = malloc(sizeof(*e));
	if (!e)
		return NULL;
	e->cp = cp;
	e->size = size;
	e->buf = load(cp, size);
	e->next = *bucket;
	*bucket = e;
	return e->buf;
}

/* Parts of sequences that have no image of their own: no space */
static bool invisible(uint32_t cp)
{
	return cp == 0x200d ||                    /* zero width joiner */
	       cp == 0xfe0e || cp == 0xfe0f ||    /* text or emoji presentation */
	       cp == 0x20e3 ||                    /* combining keycap */
	       (cp >= 0x1f3fb && cp <= 0x1f3ff) || /* skin tones */
	       (cp >= 0xe0020 && cp <= 0xe007f);  /* tags (subdivision flags) */
}

/* Sequences */

static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	c &= n ? 0x3f >> n : 0xff;
	for (int i = 1; i <= n; i++) {
		if ((p[i] & 0xc0) != 0x80) { /* broken: one byte */
			(*s)++;
			return 0xfffd;
		}
		c = c << 6 | (p[i] & 0x3f);
	}
	*s += n + 1;
	return c;
}

static bool regional(uint32_t c) { return c >= 0x1f1e6 && c <= 0x1f1ff; }
static bool skin_tone(uint32_t c) { return c >= 0x1f3fb && c <= 0x1f3ff; }
static bool keycap_base(uint32_t c) { return (c >= '0' && c <= '9') || c == '#' || c == '*'; }

/* The character standing for the sequence named `name`, 0 if it has no image */
static uint32_t seq_char(const char *name)
{
	for (unsigned i = 0; i < seq_count; i++)
		if (!strcmp(seqs[i], name))
			return SEQ_FIRST + i;
	char path[256];
	snprintf(path, sizeof(path), EMOJI_DIR "/%s.png", name);
	if (access(path, R_OK))
		return 0;
	char **grown = realloc(seqs, (seq_count + 1) * sizeof(*seqs));
	if (!grown)
		return 0;
	seqs = grown;
	if (!(seqs[seq_count] = strdup(name)))
		return 0;
	return SEQ_FIRST + seq_count++;
}

/* At an emoji: reads the sequence starting there (without U+FE0F and skin
 * tones) into cps; returns their count, 0 if this is not a sequence */
static unsigned seq_read(const char **s, uint32_t *cps)
{
	const char *p = *s;
	uint32_t c = utf8_next(&p);
	unsigned n = 0;
	cps[n++] = c;

	if (regional(c)) {
		const char *q = p;
		uint32_t d = utf8_next(&q);
		if (!regional(d))
			return 0;
		cps[n++] = d;
		*s = q;
		return n;
	}
	for (;;) {
		const char *q = p;
		uint32_t d = *q ? utf8_next(&q) : 0;
		if (d == 0xfe0f || skin_tone(d)) {
			/* not in the names */
		} else if (d == 0x20e3 || (d >= 0xe0020 && d <= 0xe007f)) {
			/* keycap, tag (subdivision flags) */
			if (n == SEQ_MAX)
				return 0;
			cps[n++] = d;
		} else if (d == 0x200d && *q) {
			uint32_t e = utf8_next(&q);
			if (n + 2 > SEQ_MAX)
				return 0;
			cps[n++] = d;
			cps[n++] = e;
		} else {
			break;
		}
		p = q;
	}
	if (n < 2)
		return 0;
	*s = p;
	return n;
}

char *ui_emoji_compose(const char *text)
{
	/* A sequence takes at least as many bytes as its character */
	char *out = malloc(strlen(text) + 1), *o = out;
	if (!out)
		return NULL;
	while (*text) {
		const char *start = text, *p = text;
		uint32_t c = utf8_next(&p);
		uint32_t cps[SEQ_MAX];
		unsigned n = 0;
		/* Below U+2000 only keycaps start sequences */
		if ((c >= 0x2000 || (keycap_base(c) && (*p == '\xe2' || *p == '\xef'))) &&
		    (n = seq_read(&text, cps)) > 0) {
			char name[SEQ_MAX * 7], *e = name;
			for (unsigned i = 0; i < n; i++)
				e += sprintf(e, i ? "-%x" : "%x", (unsigned)cps[i]);
			if (keycap_base(cps[0]))
				memmove(name + 2, name, strlen(name) + 1), name[0] = name[1] = '0';
			uint32_t ch = seq_char(name);
			if (ch) {
				/* 4 bytes of UTF-8 */
				*o++ = 0xf0 | ch >> 18;
				*o++ = 0x80 | (ch >> 12 & 0x3f);
				*o++ = 0x80 | (ch >> 6 & 0x3f);
				*o++ = 0x80 | (ch & 0x3f);
				continue;
			}
			/* No image: the whole sequence as it is (a flag's second letter
			 * must not pair with the next flag) */
			p = text;
		}
		memcpy(o, start, p - start);
		o += p - start;
		text = p;
	}
	*o = '\0';
	return out;
}

/* Fonts */

static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t cp, uint32_t next)
{
	LV_UNUSED(next);
	/* Everything below is text: the common case, answered right away */
	if (cp < 0x2000)
		return false;

	if (invisible(cp)) {
		lv_memzero(dsc, sizeof(*dsc));
		dsc->format = LV_FONT_GLYPH_FORMAT_NONE;
		dsc->resolved_font = font;
		return true;
	}

	/* A bit larger than the letters, centered on the line */
	const lv_font_t *text = font->fallback;
	uint32_t size = (uint32_t)(uintptr_t)font->dsc;
	size += size / 8;
	lv_draw_buf_t *buf = emoji_get(cp, size);
	if (!buf)
		return false;

	lv_memzero(dsc, sizeof(*dsc));
	dsc->box_w = size;
	dsc->box_h = size;
	dsc->adv_w = size + size / 8;
	dsc->ofs_x = size / 16;
	dsc->ofs_y = (text->line_height - (int32_t)size) / 2 - text->base_line;
	dsc->format = LV_FONT_GLYPH_FORMAT_IMAGE;
	dsc->gid.src = buf;
	dsc->resolved_font = font;
	return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf)
{
	LV_UNUSED(draw_buf);
	return dsc->gid.src;
}

/* dsc: the font size in pixels */
#define EMOJI_FONT(px)                                         \
	const lv_font_t ui_emoji_##px = {                      \
		.get_glyph_dsc = glyph_dsc,                    \
		.get_glyph_bitmap = glyph_bitmap,              \
		.line_height = px,                             \
		.base_line = 0,                                \
		.dsc = (const void *)(uintptr_t)px,            \
		.fallback = &lv_font_montserrat_##px,          \
	}

EMOJI_FONT(16);
EMOJI_FONT(20);
EMOJI_FONT(24);
EMOJI_FONT(26);
EMOJI_FONT(32);
EMOJI_FONT(48);

void ui_emoji_reload(void)
{
	/* Only the missing ones: labels may still show the others */
	for (unsigned i = 0; i < BUCKETS; i++)
		for (struct emoji **e = &table[i]; *e;) {
			if ((*e)->buf) {
				e = &(*e)->next;
				continue;
			}
			struct emoji *missing = *e;
			*e = missing->next;
			free(missing);
		}
	/* Every label measures its text again */
	lv_obj_report_style_change(NULL);
}
