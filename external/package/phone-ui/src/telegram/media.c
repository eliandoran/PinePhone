/*
 * Telegram's photos and avatars: JPEGs decoded (libjpeg-turbo, scaled down
 * while decoding) into GL textures, shown by lv_3dtexture like the map tiles:
 * the draw unit's cache knows images by their address.
 */

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

#include "telegram.h"

#if LV_USE_3DTEXTURE
#include <GLES2/gl2.h>
#endif

struct jpeg_error {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

/* libjpeg's default exits the program */
static void jpeg_error_exit(j_common_ptr cinfo)
{
	char text[JMSG_LENGTH_MAX];
	cinfo->err->format_message(cinfo, text);
	printf("telegram: photo: %s\n", text);
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
}

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
#if LV_USE_3DTEXTURE
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error err;
	unsigned char *volatile pixels = NULL;
	unsigned tex = 0;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_decompress(&cinfo);
		free(pixels);
		fclose(f);
		return 0;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	/* LVGL's layout: its texture shader swaps red and blue back (c.bgra) */
	cinfo.out_color_space = JCS_EXT_BGRA;
	cinfo.scale_denom = 8;
	cinfo.scale_num = (width * 8 + cinfo.image_width - 1) / cinfo.image_width;
	if (cinfo.scale_num < 1)
		cinfo.scale_num = 1;
	if (cinfo.scale_num > 8)
		cinfo.scale_num = 8;
	jpeg_start_decompress(&cinfo);

	size_t stride = cinfo.output_width * 4;
	pixels = malloc(stride * cinfo.output_height);
	if (!pixels)
		longjmp(err.jmp, 1);
	while (cinfo.output_scanline < cinfo.output_height) {
		unsigned char *row = pixels + cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	if (round)
		round_alpha(pixels, cinfo.output_width, cinfo.output_height);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cinfo.output_width, cinfo.output_height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(pixels);
	fclose(f);
	return tex;
#else
	LV_UNUSED(path);
	LV_UNUSED(width);
	LV_UNUSED(round);
	return 0;
#endif
}

void texture_delete(unsigned tex)
{
#if LV_USE_3DTEXTURE
	if (tex)
		glDeleteTextures(1, &tex);
#else
	LV_UNUSED(tex);
#endif
}
