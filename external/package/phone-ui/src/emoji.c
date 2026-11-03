/*
 * Color emoji: fonts whose glyphs are Noto's emoji images, between our ui_font_*
 * and LVGL's Montserrat in the fallback chain (gen-fonts.sh). The images are
 * 64 x 64 PNGs in /usr/share/emoji/<code point>.png (package noto-emoji),
 * decoded and scaled down to the font's size the first time they are drawn, and
 * kept. Labels are rendered on the CPU, which draws image glyphs in their colors.
 *
 * One code point per emoji: sequences (flags, skin tones, joined emoji) show as
 * their parts, their joiners and modifiers take no space.
 */

#include <png.h>
#include <stdio.h>
#include <stdlib.h>

#include "ui.h"

#define DIR "/usr/share/emoji"
#define BUCKETS 512

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
	char path[64];
	snprintf(path, sizeof(path), DIR "/%x.png", (unsigned)cp);
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
