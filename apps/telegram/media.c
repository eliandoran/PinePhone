/*
 * Telegram's photos and avatars: JPEGs decoded (libjpeg-turbo, scaled down
 * while decoding) into GL textures, shown by lv_3dtexture like the map tiles:
 * the draw unit's cache knows images by their address.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

/* Saved messages' avatar: a white bookmark on Telegram's blue gradient,
 * supersampled 4 x 4 for its edges (BGRA, round) */
unsigned saved_texture(int size)
{
	unsigned char *pixels = malloc((size_t)size * size * 4);
	if (!pixels)
		return 0;
	/* Bookmark, in fractions of the size: half width, top, bottom, depth of the notch */
	const float hw = 0.16f, top = 0.27f, bottom = 0.73f, notch = 0.13f;
	for (int y = 0; y < size; y++) {
		/* From #72d5fd at the top to #2a9ef1 at the bottom */
		float t = (y + 0.5f) / size;
		float r = 0x72 + (0x2a - 0x72) * t, g = 0xd5 + (0x9e - 0xd5) * t, b = 0xfd + (0xf1 - 0xfd) * t;
		for (int x = 0; x < size; x++) {
			int in = 0;
			for (int sy = 0; sy < 4; sy++) {
				for (int sx = 0; sx < 4; sx++) {
					float fx = fabsf((x + (sx + 0.5f) / 4) / size - 0.5f);
					float fy = (y + (sy + 0.5f) / 4) / size;
					in += fx <= hw && fy >= top && fy <= bottom - notch * (1 - fx / hw);
				}
			}
			float a = in / 16.0f;
			unsigned char *p = pixels + (y * size + x) * 4;
			p[0] = (unsigned char)(b + (255 - b) * a);
			p[1] = (unsigned char)(g + (255 - g) * a);
			p[2] = (unsigned char)(r + (255 - r) * a);
		}
	}
	round_alpha(pixels, size, size);
	unsigned tex = ui_texture_create(pixels, size, size);
	free(pixels);
	return tex;
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
