/*
 * SVG images (browser_image_decode()): parsed and drawn by nanosvg, nanosvg.h and
 * nanosvgrast.h beside this file, unchanged from github.com/memononen/nanosvg at
 * 239e102e (zlib license, in each). Paths, shapes, fills, strokes and gradients;
 * not text, <style> sheets, clip paths, masks or filters.
 */

#define _GNU_SOURCE /* memmem */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
/* Kept as upstream wrote them, without our warnings (NON-NLS-BEGIN: the options) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "nanosvg.h"
#include "nanosvgrast.h"
#pragma GCC diagnostic pop
/* NON-NLS-END */

#include "browser.h"

#define SNIFF 4096 /* <svg within the first bytes: after an XML declaration, comments */

bool svg_is(const char *data, size_t size)
{
	if (size >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) {
		data += 3;
		size -= 3;
	}
	while (size && strchr(" \t\r\n", *data)) {
		data++;
		size--;
	}
	return size && *data == '<' && memmem(data, size < SNIFF ? size : SNIFF, "<svg", 4);
}

uint8_t *svg_decode(const char *data, size_t size, int max_w, int max_pixels, int *w, int *h,
                    int *natural_w, int *natural_h)
{
	/* nanosvg parses in place, up to a NUL */
	char *text = malloc(size + 1);
	if (!text)
		return NULL;
	memcpy(text, data, size);
	text[size] = 0;
	NSVGimage *img = nsvgParse(text, "px", 96);
	free(text);
	if (!img)
		return NULL;
	uint8_t *px = NULL;
	/* Its width and height (the viewBox's without them), in CSS pixels, drawn at the
	 * page's scale */
	float scale = BROWSER_SCALE;
	if (img->width * scale > max_w)
		scale = max_w / img->width;
	int ow = (int)lroundf(img->width * scale), oh = (int)lroundf(img->height * scale);
	if (img->width < 1 || img->height < 1 || ow < 1 || oh < 1 || (int64_t)ow * oh > max_pixels)
		goto done;
	px = calloc((size_t)ow * oh, 4);
	NSVGrasterizer *r = px ? nsvgCreateRasterizer() : NULL;
	if (!r) {
		free(px);
		px = NULL;
		goto done;
	}
	nsvgRasterize(r, img, 0, 0, scale, px, ow, oh, ow * 4);
	nsvgDeleteRasterizer(r);
	/* RGBA to BGRA, as the other decoders */
	for (size_t i = 0; i < (size_t)ow * oh * 4; i += 4) {
		uint8_t t = px[i];
		px[i] = px[i + 2];
		px[i + 2] = t;
	}
	*w = ow;
	*h = oh;
	*natural_w = (int)ceilf(img->width);
	*natural_h = (int)ceilf(img->height);
done:
	nsvgDelete(img);
	return px;
}
