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
