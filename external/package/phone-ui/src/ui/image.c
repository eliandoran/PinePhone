/*
 * Images for lv_3dtexture: JPEG decoding (libjpeg-turbo, scaled down while
 * decoding) and GL textures. The draw unit's cache knows images by their
 * address, textures it draws as they are. Photos (JPEG or the camera's PPM) read
 * scaled down; JPEG encoding for photos and screenshots.
 */

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>

#include "ui.h"

#if LV_USE_3DTEXTURE
#include <GLES2/gl2.h>
#endif

float ui_fit_scale(int w, int h, int bw, int bh, bool cover)
{
	float sx = (float)bw / w, sy = (float)bh / h;
	return cover ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
}

struct jpeg_error {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

/* libjpeg's default exits the program */
static void jpeg_error_exit(j_common_ptr cinfo)
{
	char text[JMSG_LENGTH_MAX];
	cinfo->err->format_message(cinfo, text);
	printf("jpeg: %s\n", text);
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
}

uint8_t *ui_jpeg_read(FILE *f, int bw, int bh, bool cover, int *w, int *h)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error err;
	uint8_t *volatile pixels = NULL;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_decompress(&cinfo);
		free(pixels);
		return NULL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	jpeg_read_header(&cinfo, TRUE);
	/* LVGL's layout: its texture shader swaps red and blue back (c.bgra) */
	cinfo.out_color_space = JCS_EXT_BGRA;
	float scale = ui_fit_scale(cinfo.image_width, cinfo.image_height, bw, bh, cover);
	cinfo.scale_denom = 8;
	cinfo.scale_num = (unsigned)(scale * 8 + 0.999f);
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
		uint8_t *row = pixels + cinfo.output_scanline * stride;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	*w = cinfo.output_width;
	*h = cinfo.output_height;
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return pixels;
}

/* Binary PPM (P6, 8 bits) as the camera writes it: every n-th pixel of every
 * n-th row, with the largest n that still keeps it as large as fitted into bw x bh */
static uint8_t *ppm_read(FILE *f, int bw, int bh, bool cover, int *w, int *h)
{
	int iw, ih, max;
	if (fscanf(f, "P6 %d %d %d", &iw, &ih, &max) != 3 || max != 255 ||
	    iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192 || fgetc(f) == EOF)
		return NULL;
	long start = ftell(f);
	int step = (int)(1 / ui_fit_scale(iw, ih, bw, bh, cover));
	if (step < 1)
		step = 1;
	int ow = iw / step, oh = ih / step;

	uint8_t *pixels = malloc((size_t)ow * oh * 4);
	uint8_t *row = malloc((size_t)iw * 3);
	for (int y = 0; pixels && row && y < oh; y++) {
		if (fseek(f, start + (long)y * step * iw * 3, SEEK_SET) != 0 ||
		    fread(row, 3, iw, f) != (size_t)iw) {
			free(pixels);
			pixels = NULL;
			break;
		}
		uint8_t *p = pixels + (size_t)y * ow * 4;
		for (int x = 0; x < ow; x++, p += 4) {
			const uint8_t *s = row + (size_t)x * step * 3;
			p[0] = s[2];
			p[1] = s[1];
			p[2] = s[0];
			p[3] = 255;
		}
	}
	free(row);
	*w = ow;
	*h = oh;
	return pixels;
}

uint8_t *ui_image_read(const char *path, int bw, int bh, bool cover, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	/* By content: downloaded files may have no extension */
	int c = fgetc(f);
	rewind(f);
	uint8_t *pixels = c == 'P' ? ppm_read(f, bw, bh, cover, w, h) :
	                  ui_jpeg_read(f, bw, bh, cover, w, h);
	fclose(f);
	if (!pixels)
		printf("image: cannot read %s\n", path);
	return pixels;
}

bool ui_jpeg_write(FILE *f, const uint8_t *px, int w, int h, long stride, int quality, bool full_color)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error err;
	cinfo.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jpeg_error_exit;
	if (setjmp(err.jmp)) {
		jpeg_destroy_compress(&cinfo);
		return false;
	}
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, f);
	cinfo.image_width = w;
	cinfo.image_height = h;
	cinfo.in_color_space = JCS_EXT_BGRX;
	cinfo.input_components = 4;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	if (full_color)
		for (int i = 0; i < cinfo.num_components; i++)
			cinfo.comp_info[i].h_samp_factor = cinfo.comp_info[i].v_samp_factor = 1;
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = (JSAMPROW)px + (long)cinfo.next_scanline * stride;
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	return true;
}

unsigned ui_texture_create(const void *pixels, int w, int h)
{
#if LV_USE_3DTEXTURE
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
#else
	LV_UNUSED(pixels);
	LV_UNUSED(w);
	LV_UNUSED(h);
	return 0;
#endif
}

void ui_texture_delete(unsigned tex)
{
#if LV_USE_3DTEXTURE
	if (tex)
		glDeleteTextures(1, &tex);
#else
	LV_UNUSED(tex);
#endif
}

unsigned ui_tile_texture(uint8_t *px, int w, int h, int32_t shown_w, uint32_t color)
{
	uint8_t *own = NULL;
	if (!px) {
		px = own = malloc((size_t)w * h * 4);
		if (!px)
			return 0;
		for (int i = 0; i < w * h; i++) {
			px[i * 4] = color & 0xff;
			px[i * 4 + 1] = color >> 8 & 0xff;
			px[i * 4 + 2] = color >> 16 & 0xff;
			px[i * 4 + 3] = 0xff;
		}
	}
	/* The corners' alpha, antialiased, in the texture's pixels */
	float r = (float)UI_TILE_RADIUS * w / shown_w;
	for (int y = 0; y < r && y < h; y++)
		for (int x = 0; x < r && x < w / 2; x++) {
			float dx = r - x - 0.5f, dy = r - y - 0.5f;
			float a = r - sqrtf(dx * dx + dy * dy) + 0.5f;
			if (a >= 1)
				continue;
			if (a < 0)
				a = 0;
			px[((size_t)y * w + x) * 4 + 3] *= a;
			px[((size_t)y * w + w - 1 - x) * 4 + 3] *= a;
		}
	unsigned tex = ui_texture_create(px, w, h);
	free(own);
	return tex;
}
