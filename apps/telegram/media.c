/*
 * Telegram's photos and avatars: JPEGs decoded (libjpeg-turbo, scaled down
 * while decoding) into GL textures, shown by lv_3dtexture like the map tiles:
 * the draw unit's cache knows images by their address. Photos sent: their
 * size, screenshots (PPM) turned into JPEG.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "telegram.h"

/* Transparent outside the inscribed circle, with a one pixel soft edge (the
 * draw unit blends textures with their alpha) */
static void round_alpha(unsigned char *pixels, int w, int h)
{
	float cx = w / 2.0f, cy = h / 2.0f, r = (w < h ? w : h) / 2.0f;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			float a = r - sqrtf(dx * dx + dy * dy) + 0.5f;
			unsigned char *p = pixels + (y * w + x) * 4;
			p[3] = a >= 1 ? 255 : a <= 0 ? 0 : (unsigned char)(a * 255);
		}
	}
}

/* Symbols drawn here: `inside` gets a point as its distance from the middle (x,
 * signed) and its height (y), in fractions of the size. How much of pixel x, y
 * they cover, supersampled 4 x 4 for the edges. */
typedef bool symbol_fn(float x, float y);

static float coverage(symbol_fn *inside, int size, int x, int y)
{
	int in = 0;
	for (int sy = 0; sy < 4; sy++)
		for (int sx = 0; sx < 4; sx++)
			in += inside((x + (sx + 0.5f) / 4) / size - 0.5f, (y + (sy + 0.5f) / 4) / size);
	return in / 16.0f;
}

/* An avatar: a white symbol on a vertical gradient from `top` to `bottom`
 * (0xRRGGBB), BGRA, round */
static unsigned symbol_texture(int size, uint32_t top, uint32_t bottom, symbol_fn *inside)
{
	unsigned char *pixels = malloc((size_t)size * size * 4);
	if (!pixels)
		return 0;
	for (int y = 0; y < size; y++) {
		float t = (y + 0.5f) / size, color[3];
		for (int i = 0; i < 3; i++) {
			int from = top >> (16 - 8 * i) & 0xff, to = bottom >> (16 - 8 * i) & 0xff;
			color[i] = from + (to - from) * t;
		}
		for (int x = 0; x < size; x++) {
			float a = coverage(inside, size, x, y);
			unsigned char *p = pixels + (y * size + x) * 4;
			for (int i = 0; i < 3; i++)
				p[2 - i] = (unsigned char)(color[i] + (255 - color[i]) * a);
		}
	}
	round_alpha(pixels, size, size);
	unsigned tex = ui_texture_create(pixels, size, size);
	free(pixels);
	return tex;
}

/* A bookmark: half width, top, bottom, depth of the notch */
static bool bookmark(float x, float y)
{
	const float hw = 0.16f, top = 0.27f, bottom = 0.73f, notch = 0.13f;
	x = fabsf(x);
	return x <= hw && y >= top && y <= bottom - notch * (1 - x / hw);
}

/* A box: its lid, then its body with a slot */
static bool box(float x, float y)
{
	x = fabsf(x);
	bool lid = x <= 0.23f && y >= 0.29f && y <= 0.39f;
	bool body = x <= 0.19f && y >= 0.42f && y <= 0.71f;
	bool slot = x <= 0.07f && y >= 0.49f && y <= 0.54f;
	return lid || (body && !slot);
}

/* A push pin leaning right: upright around the middle (u across, v down), its
 * head, body widening to the collar, collar and needle, turned by 45 degrees */
static bool pin(float x, float y)
{
	const float k = 0.70710678f;
	y -= 0.5f;
	float u = fabsf(x * k + y * k), v = y * k - x * k;
	return (v >= -0.44f && v < -0.36f && u <= 0.17f) ||
	       (v >= -0.36f && v < 0.02f && u <= 0.09f + (v + 0.36f) * 0.15f) ||
	       (v >= 0.02f && v < 0.10f && u <= 0.26f) ||
	       (v >= 0.10f && v <= 0.44f && u <= 0.04f);
}

/* A pinned chat's mark: the pin in `color` (0xRRGGBB) on nothing, BGRA */
unsigned pin_texture(int size, uint32_t color)
{
	unsigned char *pixels = malloc((size_t)size * size * 4);
	if (!pixels)
		return 0;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			unsigned char *p = pixels + (y * size + x) * 4;
			p[0] = color & 0xff;
			p[1] = color >> 8 & 0xff;
			p[2] = color >> 16 & 0xff;
			p[3] = (unsigned char)(coverage(pin, size, x, y) * 255);
		}
	}
	unsigned tex = ui_texture_create(pixels, size, size);
	free(pixels);
	return tex;
}

/* Telegram's colors */
unsigned saved_texture(int size)
{
	return symbol_texture(size, 0x72d5fd, 0x2a9ef1, bookmark);
}

unsigned archive_texture(int size)
{
	return symbol_texture(size, 0xb8c2cc, 0x9ea9b4, box);
}

/* Decoded at the smallest scale (n/8) still as wide as on screen; round: a
 * circle (avatars) */
unsigned jpeg_texture(const char *path, int width, bool round)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	/* Fitted by width only: as large as on screen at any height */
	int w, h;
	uint8_t *pixels = ui_jpeg_read(f, width, 0, true, &w, &h);
	fclose(f);
	if (!pixels) {
		printf("telegram: cannot read %s\n", path);
		return 0;
	}
	if (round)
		round_alpha(pixels, w, h);
	unsigned tex = ui_texture_create(pixels, w, h);
	free(pixels);
	return tex;
}

/* Photos sent */

/* A JPEG's size from its frame header (SOFn), false if it has none */
bool jpeg_size(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	bool found = false;
	if (fgetc(f) == 0xff && fgetc(f) == 0xd8) {
		for (;;) {
			int c = fgetc(f);
			if (c != 0xff)
				break;
			int marker;
			while ((marker = fgetc(f)) == 0xff)
				;
			if (marker == EOF || marker == 0xd9 || marker == 0xda)
				break;
			/* No length: restarts, TEM */
			if ((marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
				continue;
			int hi = fgetc(f), lo = fgetc(f);
			if (lo == EOF)
				break;
			int len = hi << 8 | lo;
			/* SOF0 to SOF15, not DHT, JPG or DAC */
			if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
				unsigned char b[5];
				if (fread(b, 1, 5, f) == 5) {
					*h = b[1] << 8 | b[2];
					*w = b[3] << 8 | b[4];
					found = *w > 0 && *h > 0;
				}
				break;
			}
			if (len < 2 || fseek(f, len - 2, SEEK_CUR) != 0)
				break;
		}
	}
	fclose(f);
	return found;
}

/* A binary PPM (P6, 8 bits: the Gallery's screenshots) written as JPEG to
 * `out`, which Telegram takes as a photo; false on error */
bool ppm_to_jpeg(const char *path, const char *out, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	int iw, ih, max;
	uint8_t *px = NULL;
	bool ok = fscanf(f, "P6 %d %d %d", &iw, &ih, &max) == 3 && max == 255 &&
	          iw > 0 && ih > 0 && iw <= 8192 && ih <= 8192 && fgetc(f) != EOF &&
	          (px = malloc((size_t)iw * ih * 4));
	/* RGB rows into BGRX, in place from the end */
	if (ok && fread(px, 3, (size_t)iw * ih, f) != (size_t)iw * ih)
		ok = false;
	fclose(f);
	if (ok) {
		for (size_t i = (size_t)iw * ih; i-- > 0;) {
			uint8_t r = px[i * 3], g = px[i * 3 + 1], b = px[i * 3 + 2];
			px[i * 4] = b;
			px[i * 4 + 1] = g;
			px[i * 4 + 2] = r;
			px[i * 4 + 3] = 255;
		}
		FILE *o = fopen(out, "wb");
		ok = o && ui_jpeg_write(o, px, iw, ih, (long)iw * 4, 92, true);
		if (o && fclose(o) != 0)
			ok = false;
		if (!ok)
			unlink(out);
	}
	free(px);
	*w = iw;
	*h = ih;
	return ok;
}
