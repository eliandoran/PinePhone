/*
 * Wallpaper: a blue to teal gradient with three soft glows (dark or pastel),
 * computed once per theme. A placeholder until wallpapers are image files (no
 * image decoder is built yet). Screens that dim it get a dimmed copy: drawing a
 * full screen translucent layer over it costs the GPU another full screen pass
 * on every frame. They are never freed: the GPU draw unit caches textures by
 * buffer address, a new buffer at a freed one's address would show the old one.
 */

#include <stdlib.h>

#include "ui_internal.h"

#define WALL_STEP 8 /* the colors are computed every 8 pixels, interpolated in between */

/* By orientation (the display turned sideways: the home screen), then ui_light */
static lv_draw_buf_t *wallpaper[2][2];                       /* as computed */
static lv_draw_buf_t *wallpapers[2][2][LV_OPA_COVER + 1]; /* by dim level, made when needed */

static void wallpaper_color(int32_t x, int32_t y, int32_t w, int32_t h, bool light, float c[3])
{
	static const struct { float x, y, r, s; float c[3]; } glows[] = {
		{ 0.15f, 0.22f, 0.45f, 0.55f, { 255, 110, 170 } },
		{ 0.95f, 0.55f, 0.50f, 0.45f, {  70, 190, 255 } },
		{ 0.35f, 0.95f, 0.40f, 0.40f, { 110, 255, 200 } },
	};
	static const float base[2][3][3] = {
		{ { 16, 18, 52 }, { 52, 26, 96 }, { 8, 70, 96 } },
		{ { 196, 208, 248 }, { 226, 206, 246 }, { 192, 232, 236 } },
	};
	const float *top = base[light][0], *mid = base[light][1], *bot = base[light][2];

	float nx = (float)x / w, ny = (float)y / w;
	float t = (float)y / h * 0.85f + nx * 0.15f;
	for (int i = 0; i < 3; i++)
		c[i] = t < 0.5f ? top[i] + (mid[i] - top[i]) * t * 2
		                : mid[i] + (bot[i] - mid[i]) * (t - 0.5f) * 2;

	for (size_t g = 0; g < sizeof(glows) / sizeof(glows[0]); g++) {
		float dx = nx - glows[g].x, dy = ny - glows[g].y * h / w;
		float q = 1 + (dx * dx + dy * dy) / (glows[g].r * glows[g].r) * 6;
		float f = glows[g].s / (q * q);
		/* Light: tinted towards the glow's color, dark: lightened by it */
		for (int i = 0; i < 3; i++)
			c[i] += light ? (glows[g].c[i] - c[i]) * f * 0.5f
			              : (255 - c[i]) * f * glows[g].c[i] / 255;
	}
}

static lv_draw_buf_t *wallpaper_make(int32_t w, int32_t h, bool light)
{
	int32_t gw = w / WALL_STEP + 2, gh = h / WALL_STEP + 2;
	float (*grid)[3] = malloc(sizeof(*grid) * gw * gh);
	for (int32_t gy = 0; gy < gh; gy++)
		for (int32_t gx = 0; gx < gw; gx++)
			wallpaper_color(gx * WALL_STEP, gy * WALL_STEP, w, h, light, grid[gy * gw + gx]);

	lv_draw_buf_t *buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
	float (*line)[3] = malloc(sizeof(*line) * gw); /* the grid's columns at this row */
	uint32_t seed = 1;

	for (int32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(buf->data + y * buf->header.stride);
		const float (*g0)[3] = &grid[y / WALL_STEP * gw], (*g1)[3] = g0 + gw;
		float fy = (float)(y % WALL_STEP) / WALL_STEP;
		for (int32_t gx = 0; gx < gw; gx++)
			for (int i = 0; i < 3; i++)
				line[gx][i] = g0[gx][i] + (g1[gx][i] - g0[gx][i]) * fy;
		/* Between two columns, the same step from pixel to pixel */
		for (int32_t x0 = 0; x0 < w; x0 += WALL_STEP) {
			const float *a = line[x0 / WALL_STEP], *b = line[x0 / WALL_STEP + 1];
			float v[3], step[3];
			for (int i = 0; i < 3; i++) {
				v[i] = a[i];
				step[i] = (b[i] - a[i]) / WALL_STEP;
			}
			for (int32_t x = x0; x < x0 + WALL_STEP && x < w; x++) {
				/* A little noise against banding */
				seed = seed * 1103515245 + 12345;
				float n = (float)((seed >> 16) & 3) - 1.5f;
				uint32_t px = 0xff000000;
				for (int i = 0; i < 3; i++) {
					float c = v[i] + n;
					px |= (uint32_t)(c < 0 ? 0 : c > 255 ? 255 : c) << (16 - 8 * i);
					v[i] += step[i];
				}
				row[x] = px;
			}
		}
	}
	free(line);
	free(grid);
	return buf;
}

/* The status bar has no background: its texts stand on the screen's. The top of
 * every wallpaper is shaded for them as the bar was (40%), fading out over twice
 * its height; screens dimmed more than that are left as they are, the bar takes
 * their background. */
#define SCRIM LV_OPA_40
#define SCRIM_H (UI_STATUS_H * 2)

static lv_opa_t row_dim(lv_opa_t dim, int32_t y)
{
	if (y >= SCRIM_H)
		return dim;
	float t = (float)y / SCRIM_H, s = SCRIM * (1 - t * t * (3 - 2 * t));
	return s > dim ? (lv_opa_t)(s + 0.5f) : dim;
}

const lv_draw_buf_t *ui_wallpaper(lv_opa_t dim)
{
	int32_t screen_w = ui_screen_w, screen_h = ui_screen_h;
	bool side = screen_w > screen_h;
	lv_draw_buf_t **cached = &wallpapers[side][ui_light][dim];
	if (!*cached) {
		if (!wallpaper[side][ui_light])
			wallpaper[side][ui_light] = wallpaper_make(screen_w, screen_h, ui_light);
		/* What a fill of the shade color (black or white) with opacity `dim` (more
		 * at the top) over it would look like */
		lv_draw_buf_t *buf = lv_draw_buf_dup(wallpaper[side][ui_light]);
		uint8_t to[256]; /* each value dimmed, for the rows dimmed by `made` */
		int made = -1;
		for (int32_t y = 0; y < screen_h; y++) {
			lv_opa_t d = row_dim(dim, y);
			if (!d)
				continue;
			if (d != made) {
				uint32_t keep = LV_OPA_COVER - d, add = (ui_light ? 255 : 0) * d + 127;
				for (uint32_t v = 0; v < 256; v++)
					to[v] = (uint8_t)((v * keep + add) / LV_OPA_COVER);
				made = d;
			}
			uint8_t *p = buf->data + y * buf->header.stride;
			for (int32_t x = 0; x < screen_w * 4; x += 4) {
				p[x] = to[p[x]];
				p[x + 1] = to[p[x + 1]];
				p[x + 2] = to[p[x + 2]];
			}
		}
		/* Never changes: the GPU draw unit only caches the texture of unmodifiable
		 * images, it renders and uploads modifiable ones again on every frame */
		lv_draw_buf_clear_flag(buf, LV_IMAGE_FLAGS_MODIFIABLE);
		*cached = buf;
	}
	return *cached;
}

void wallpaper_refit(lv_obj_t *scr)
{
	lv_obj_t *img = scr ? lv_obj_get_child(scr, 0) : NULL;
	if (img && lv_obj_check_type(img, &lv_image_class))
		lv_image_set_src(img, ui_wallpaper((lv_opa_t)(uintptr_t)lv_obj_get_user_data(img)));
}
