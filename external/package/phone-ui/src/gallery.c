/*
 * Gallery: the photos taken by the camera (/var/lib/photos, JPEG or PPM), newest
 * first, as a grid of square thumbnails, and a viewer for one photo: swipe left
 * or right for the next or previous one, delete it from the header.
 *
 * Photos are decoded scaled down while reading (libjpeg's n/8 scaling, every
 * n-th pixel of a PPM) into GL textures shown by lv_3dtexture, like the map
 * tiles. Thumbnails are decoded one per timer tick, newest first; the UI waits
 * for each.
 */

#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>

#include "ui.h"

#if LV_USE_3DTEXTURE

#include <GLES2/gl2.h>

#define PHOTOS "/var/lib/photos"
#define COLUMNS 3
#define GAP 4
#define SWIPE 120 /* px sideways for the next or previous photo */
#define COLOR_RED 0xff453a

struct photo {
	char name[64]; /* file name in PHOTOS */
	time_t mtime;
	lv_obj_t *cell;
	unsigned tex;  /* thumbnail, 0: not decoded yet */
	bool bad;      /* could not be decoded */
};

static struct {
	struct photo *photos;
	unsigned count;
	lv_obj_t *body;
	lv_timer_t *timer;
} gal;

static struct {
	lv_obj_t *scr, *stage, *quad, *date, *pos, *confirm;
	unsigned index, tex;
	lv_point_t press;
} view;

/* Decoding */

/* The scale of a w x h image fitted into a box of bw x bh (cover: filling it) */
static float scale_for(int w, int h, int bw, int bh, bool cover)
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
	printf("gallery: %s\n", text);
	longjmp(((struct jpeg_error *)cinfo->err)->jmp, 1);
}

/* At the smallest scale (n/8) still as large as fitted into (cover: filling) bw x bh */
static uint8_t *jpeg_read(FILE *f, int bw, int bh, bool cover, int *w, int *h)
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
	float scale = scale_for(cinfo.image_width, cinfo.image_height, bw, bh, cover);
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

/* Binary PPM (P6, 8 bits) as the camera wrote it: every n-th pixel of every n-th
 * row, with the largest n that still keeps it as large as fitted into bw x bh */
static uint8_t *ppm_read(FILE *f, int bw, int bh, bool cover, int *w, int *h)
{
	int iw, ih, max;
	if (fscanf(f, "P6 %d %d %d", &iw, &ih, &max) != 3 || max != 255 ||
	    iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192 || fgetc(f) == EOF)
		return NULL;
	long start = ftell(f);
	int step = (int)(1 / scale_for(iw, ih, bw, bh, cover));
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

/* Pixels (BGRA) of a photo, at least as large as fitted into (cover: filling)
 * bw x bh; NULL if it cannot be read */
static uint8_t *photo_read(const char *name, int bw, int bh, bool cover, int *w, int *h)
{
	char path[sizeof(PHOTOS) + 64];
	snprintf(path, sizeof(path), PHOTOS "/%s", name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	const char *ext = strrchr(name, '.');
	uint8_t *pixels = !strcmp(ext, ".ppm") ? ppm_read(f, bw, bh, cover, w, h) :
	                  jpeg_read(f, bw, bh, cover, w, h);
	fclose(f);
	if (!pixels)
		printf("gallery: cannot read %s\n", name);
	return pixels;
}

static unsigned texture_create(const uint8_t *pixels, int w, int h)
{
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
}

static void texture_delete(unsigned tex)
{
	if (tex)
		glDeleteTextures(1, &tex);
}

/* Files */

static bool photo_file(const char *name)
{
	const char *ext = strrchr(name, '.');
	return ext && (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg") || !strcmp(ext, ".ppm"));
}

static int photo_cmp(const void *a, const void *b)
{
	const struct photo *x = a, *y = b;
	if (x->mtime != y->mtime)
		return (x->mtime < y->mtime) - (x->mtime > y->mtime);
	return -strcmp(x->name, y->name);
}

/* Photos newest first into gal.photos */
static void photos_load(void)
{
	unsigned size = 0;
	DIR *dir = opendir(PHOTOS);
	if (!dir)
		return;
	struct dirent *d;
	while ((d = readdir(dir))) {
		size_t len = strlen(d->d_name);
		if (len >= sizeof(gal.photos->name) || !photo_file(d->d_name))
			continue;
		char path[sizeof(PHOTOS) + sizeof(gal.photos->name)];
		snprintf(path, sizeof(path), PHOTOS "/%s", d->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
			continue;
		if (gal.count == size) {
			size = size ? size * 2 : 32;
			gal.photos = realloc(gal.photos, size * sizeof(*gal.photos));
		}
		struct photo *p = &gal.photos[gal.count++];
		memset(p, 0, sizeof(*p));
		memcpy(p->name, d->d_name, len + 1);
		p->mtime = st.st_mtime;
	}
	closedir(dir);
	qsort(gal.photos, gal.count, sizeof(*gal.photos), photo_cmp);
}

static void empty_show(void)
{
	lv_obj_t *label = lv_label_create(gal.body);
	lv_label_set_text_static(label, "No photos");
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(label, 200, 0);
}

/* Viewer */

static void view_show(unsigned index)
{
	const struct photo *p = &gal.photos[index];
	view.index = index;
	lv_obj_set_hidden(view.confirm, true);

	int32_t bw = lv_obj_get_content_width(view.stage);
	int32_t bh = lv_obj_get_content_height(view.stage);
	int w, h;
	uint8_t *pixels = photo_read(p->name, bw, bh, false, &w, &h);
	texture_delete(view.tex);
	view.tex = 0;
	if (pixels) {
		view.tex = texture_create(pixels, w, h);
		free(pixels);
		float scale = scale_for(w, h, bw, bh, false);
		lv_obj_set_size(view.quad, (int32_t)(w * scale), (int32_t)(h * scale));
		lv_3dtexture_set_src(view.quad, view.tex);
		lv_obj_invalidate(view.quad);
	}
	lv_obj_set_hidden(view.quad, !pixels);

	char buf[64];
	struct tm tm;
	localtime_r(&p->mtime, &tm);
	strftime(buf, sizeof(buf), "%-d %B %Y, %H:%M", &tm);
	ui_set_text(view.date, buf);
	ui_set_text_fmt(view.pos, "%u of %u", index + 1, gal.count);
}

static void stage_cb(lv_event_t *e)
{
	lv_point_t pt;
	lv_indev_get_point(lv_indev_active(), &pt);
	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		view.press = pt;
		return;
	}
	int32_t dx = pt.x - view.press.x, dy = pt.y - view.press.y;
	if (LV_ABS(dx) < SWIPE || LV_ABS(dy) > LV_ABS(dx))
		return;
	if (dx < 0 && view.index + 1 < gal.count)
		view_show(view.index + 1);
	else if (dx > 0 && view.index > 0)
		view_show(view.index - 1);
}

static void trash_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(view.confirm, !lv_obj_is_hidden(view.confirm));
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(view.confirm, true);
}

/* Deletes the photo and its cell; shows the next one (or the previous, the last
 * one was deleted), or goes back once there is none left */
static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	struct photo *p = &gal.photos[view.index];
	char path[sizeof(PHOTOS) + sizeof(p->name)];
	snprintf(path, sizeof(path), PHOTOS "/%s", p->name);
	if (unlink(path) != 0) {
		perror("gallery");
		return;
	}
	ui_vibrate(15);
	lv_obj_delete(p->cell);
	texture_delete(p->tex);
	memmove(p, p + 1, (gal.count - view.index - 1) * sizeof(*p));
	gal.count--;

	if (!gal.count) {
		empty_show();
		ui_pop();
		return;
	}
	view_show(view.index < gal.count ? view.index : gal.count - 1);
}

static void view_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	texture_delete(view.tex);
	memset(&view, 0, sizeof(view));
}

static void confirm_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t color,
                           lv_event_cb_t cb)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_height(button, 80);
	lv_obj_set_flex_grow(button, 1);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(bg), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_center(label);
}

static void view_open(unsigned index)
{
	lv_obj_t *header, *body;
	view.scr = ui_app_screen_create("Photo", &header, &body);
	lv_obj_add_event_cb(view.scr, view_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, NULL);
	lv_obj_set_scrollable(body, false);

	/* The photo, fitted and centered; swipes anywhere on it */
	view.stage = lv_obj_create(body);
	lv_obj_remove_style_all(view.stage);
	lv_obj_set_width(view.stage, LV_PCT(100));
	lv_obj_set_flex_grow(view.stage, 1);
	lv_obj_set_scrollable(view.stage, false);
	lv_obj_set_clickable(view.stage, true);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(view.stage, stage_cb, LV_EVENT_RELEASED, NULL);
	view.quad = lv_3dtexture_create(view.stage);
	lv_obj_set_clickable(view.quad, false);
	lv_obj_center(view.quad);

	lv_obj_t *info = lv_obj_create(body);
	lv_obj_remove_style_all(info);
	lv_obj_set_size(info, LV_PCT(100), 112);
	lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(info, 4, 0);
	lv_obj_set_clickable(info, false);
	lv_obj_set_scrollable(info, false);
	view.date = lv_label_create(info);
	lv_obj_set_style_text_color(view.date, lv_color_hex(UI_COLOR_TEXT), 0);
	view.pos = lv_label_create(info);
	lv_obj_set_style_text_font(view.pos, &ui_font_20, 0);
	lv_obj_set_style_text_color(view.pos, lv_color_hex(UI_COLOR_DIM), 0);

	/* Delete confirmation, shown by the trash button, in place of the info */
	view.confirm = lv_obj_create(body);
	lv_obj_remove_style_all(view.confirm);
	lv_obj_set_size(view.confirm, LV_PCT(100), 112);
	lv_obj_set_floating(view.confirm, true);
	lv_obj_align(view.confirm, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_flex_flow(view.confirm, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(view.confirm, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(view.confirm, 16, 0);
	lv_obj_set_style_pad_column(view.confirm, 12, 0);
	lv_obj_set_style_bg_opa(view.confirm, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(view.confirm, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_scrollable(view.confirm, false);
	confirm_button(view.confirm, "Cancel", UI_COLOR_BUTTON, UI_COLOR_TEXT, cancel_cb);
	confirm_button(view.confirm, LV_SYMBOL_TRASH "  Delete photo", COLOR_RED, 0xffffff, delete_cb);

	lv_obj_update_layout(view.scr);
	view_show(index);
	ui_push(view.scr);
}

/* Grid */

static void cell_cb(lv_event_t *e)
{
	lv_obj_t *cell = lv_event_get_target(e);
	for (unsigned i = 0; i < gal.count; i++) {
		if (gal.photos[i].cell == cell) {
			view_open(i);
			return;
		}
	}
}

/* The next thumbnail, newest first; stops once they are all done */
static void thumb_cb(lv_timer_t *timer)
{
	for (unsigned i = 0; i < gal.count; i++) {
		struct photo *p = &gal.photos[i];
		if (p->tex || p->bad)
			continue;
		int32_t size = lv_obj_get_width(p->cell);
		int w, h;
		uint8_t *pixels = photo_read(p->name, size, size, true, &w, &h);
		if (!pixels) {
			p->bad = true;
			return;
		}
		/* The middle square, moved to the start of the buffer row by row */
		int s = w < h ? w : h, x0 = (w - s) / 2, y0 = (h - s) / 2;
		for (int y = 0; y < s; y++)
			memmove(pixels + (size_t)y * s * 4, pixels + ((size_t)(y0 + y) * w + x0) * 4,
			        (size_t)s * 4);
		p->tex = texture_create(pixels, s, s);
		free(pixels);

		lv_obj_t *quad = lv_3dtexture_create(p->cell);
		lv_obj_set_size(quad, LV_PCT(100), LV_PCT(100));
		lv_obj_set_clickable(quad, false);
		lv_3dtexture_set_src(quad, p->tex);
		return;
	}
	lv_timer_pause(timer);
}

static void gallery_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(gal.timer);
	for (unsigned i = 0; i < gal.count; i++)
		texture_delete(gal.photos[i].tex);
	free(gal.photos);
	memset(&gal, 0, sizeof(gal));
}

void gallery_open(void)
{
	lv_obj_t *scr = ui_app_screen_create("Gallery", NULL, &gal.body);
	lv_obj_add_event_cb(scr, gallery_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_flow(gal.body, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(gal.body, GAP, 0);
	lv_obj_set_style_pad_bottom(gal.body, 32, 0);
	lv_obj_set_style_pad_gap(gal.body, GAP, 0);
	lv_obj_set_scroll_dir(gal.body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(gal.body, LV_SCROLLBAR_MODE_OFF);

	photos_load();
	if (!gal.count)
		empty_show();

	lv_obj_update_layout(scr);
	int32_t size = (lv_obj_get_content_width(gal.body) - (COLUMNS - 1) * GAP) / COLUMNS;
	for (unsigned i = 0; i < gal.count; i++) {
		lv_obj_t *cell = lv_obj_create(gal.body);
		lv_obj_remove_style_all(cell);
		lv_obj_set_size(cell, size, size);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(cell, lv_color_hex(UI_COLOR_CARD), 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_set_clickable(cell, true);
		lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_SHORT_CLICKED, NULL);
		gal.photos[i].cell = cell;
	}

	gal.timer = lv_timer_create(thumb_cb, 10, NULL);
	ui_push(scr);
}

#else

void gallery_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Gallery", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "The gallery needs the GPU renderer");
	ui_push(scr);
}

#endif
