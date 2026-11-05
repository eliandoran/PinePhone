/* docs/apps/gallery.md's screenshots (included by test_docs.c) */

#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>

#define GALLERY_PHOTOS "/var/lib/photos"

/* A made-up landscape: a sky, a sun or moon, two lines of hills, maybe the sea */
struct gallery_scene {
	uint32_t sky_top, sky_low, far, near, sun;
	float sun_x, sun_y, sun_r; /* of the width, the height, the width */
	bool sea, stars;
};

static uint32_t gallery_mix(uint32_t a, uint32_t b, float t)
{
	if (t < 0)
		t = 0;
	if (t > 1)
		t = 1;
	uint32_t c = 0;
	for (int s = 0; s < 24; s += 8) {
		float x = ((a >> s) & 0xff) * (1 - t) + ((b >> s) & 0xff) * t;
		c |= (uint32_t)(x + 0.5f) << s;
	}
	return c;
}

/* A w x h JPEG in the photos' folder, taken at `when` */
static void gallery_photo(const char *name, int w, int h, const struct gallery_scene *s, time_t when)
{
	uint8_t *px = malloc((size_t)w * h * 4);
	float sx = s->sun_x * w, sy = s->sun_y * h, sr = s->sun_r * w;
	float horizon = 0.62f * h;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float fx = (float)x / w;
			float far = h * (0.56f + 0.05f * sinf(fx * 7.1f + s->sun_x * 9) + 0.025f * sinf(fx * 17.3f));
			float near = h * (0.72f + 0.06f * sinf(fx * 4.3f + 1.7f) + 0.02f * sinf(fx * 23.0f));
			uint32_t c;
			if (s->sea && y >= horizon) {
				/* The sea, darker towards the shore, the sun's path on it */
				float t = (y - horizon) / (h - horizon);
				c = gallery_mix(gallery_mix(s->sky_low, s->far, 0.5f), s->near, t);
				float dx = fabsf(x - sx) / (sr * (1.2f + 2 * t));
				if (dx < 1 && ((y / 5) % 3))
					c = gallery_mix(c, s->sun, 0.55f * (1 - dx) * (1 - t));
			} else if (!s->sea && y >= near) {
				c = gallery_mix(s->near, 0x000000, 0.4f * (y - near) / (h - near));
			} else if (!s->sea && y >= far) {
				c = gallery_mix(s->far, s->near, 0.3f * (y - far) / (h * 0.2f));
			} else {
				c = gallery_mix(s->sky_top, s->sky_low, (float)y / horizon);
				float d = hypotf(x - sx, y - sy);
				if (d < sr * 2.5f)
					c = gallery_mix(c, s->sun, 0.25f * (1 - d / (sr * 2.5f)));
				if (d < sr)
					c = gallery_mix(s->sun, c, d / sr > 0.9f ? (d / sr - 0.9f) / 0.1f : 0);
				uint32_t hash = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u);
				hash = hash * 2654435761u;
				if (s->stars && (hash >> 22) == 0 && y < horizon * 0.8f)
					c = 0xf0f0ff;
			}
			uint8_t *p = px + ((size_t)y * w + x) * 4;
			p[0] = c & 0xff;
			p[1] = (c >> 8) & 0xff;
			p[2] = (c >> 16) & 0xff;
			p[3] = 0xff;
		}
	}
	char path[128];
	mkdir(GALLERY_PHOTOS, 0755);
	snprintf(path, sizeof(path), GALLERY_PHOTOS "/%s", name);
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		CHECK(ui_jpeg_write(f, px, w, h, (long)w * 4, 90, false));
		fclose(f);
		struct timeval tv[2] = { { when, 0 }, { when, 0 } };
		utimes(path, tv);
	}
	free(px);
}

static const struct gallery_scene gallery_scenes[] = {
	/* sunset over the sea */
	{ 0x2b1a4a, 0xff8c42, 0x24304f, 0x0d1424, 0xffd27a, 0.62f, 0.52f, 0.09f, true, false },
	/* morning hills */
	{ 0x6fb7ff, 0xd8ecff, 0x6f9f6a, 0x2f5d34, 0xfffbe0, 0.25f, 0.2f, 0.07f, false, false },
	/* night */
	{ 0x050a1e, 0x1c2b5a, 0x141a30, 0x07090f, 0xe8ecff, 0.75f, 0.18f, 0.06f, false, true },
	/* noon at the beach */
	{ 0x1f7ae0, 0xa8dcff, 0x2a9fd0, 0x0f5f8f, 0xffffff, 0.8f, 0.12f, 0.06f, true, false },
	/* autumn */
	{ 0x8fb4d9, 0xf3d9b1, 0xc0703a, 0x6b3418, 0xfff1c9, 0.4f, 0.35f, 0.08f, false, false },
	/* dusk in the mountains */
	{ 0x3a2f6b, 0xe86f7c, 0x5a4a7a, 0x2a2140, 0xffc3a0, 0.5f, 0.5f, 0.1f, false, false },
};

/* Twelve photos over two weeks, the last ones this morning */
static void gallery_photos(void)
{
	static const struct {
		int scene, w, h;
		int ago; /* minutes before NOW */
	} photos[] = {
		{ 0, 480, 640, 29 },              /* 09:12 */
		{ 1, 480, 640, 54 },              /* 08:47 */
		{ 3, 640, 480, 15 * 60 + 11 },    /* yesterday 18:30 */
		{ 5, 480, 640, 15 * 60 + 13 },
		{ 2, 480, 640, 2 * 1440 + 600 },
		{ 4, 480, 640, 3 * 1440 + 120 },
		{ 1, 640, 480, 3 * 1440 + 125 },
		{ 0, 480, 640, 6 * 1440 + 50 },
		{ 5, 480, 640, 7 * 1440 + 400 },
		{ 3, 480, 640, 9 * 1440 + 30 },
		{ 4, 640, 480, 11 * 1440 + 200 },
		{ 2, 480, 640, 13 * 1440 + 700 },
	};
	for (size_t i = 0; i < sizeof(photos) / sizeof(photos[0]); i++) {
		time_t when = NOW - photos[i].ago * 60;
		struct tm tm;
		char name[64];
		gmtime_r(&when, &tm);
		strftime(name, sizeof(name), "IMG_%Y%m%d_%H%M%S.jpg", &tm);
		gallery_photo(name, photos[i].w, photos[i].h, &gallery_scenes[photos[i].scene], when);
	}
}

/* gallery.md: the grid of photos, newest first */
TEST(gallery_grid)
{
	gallery_photos();
	CHECK(ui_test_open("Gallery"));
	ui_test_wait(800); /* the thumbnails, one at a time */
	lv_obj_t *thumbs[16];
	CHECK_INT(ui_test_find_widgets(&lv_3dtexture_class, thumbs, 16), 12);
	/* Three to a row */
	lv_area_t a, b, c;
	lv_obj_get_coords(thumbs[0], &a);
	lv_obj_get_coords(thumbs[2], &b);
	lv_obj_get_coords(thumbs[3], &c);
	CHECK_INT(a.y1, b.y1);
	CHECK(c.y1 > a.y1);
	shot("gallery-grid");
}

/* gallery.md: a photo opened, its date and place in the list under it */
TEST(gallery_photo)
{
	gallery_photos();
	CHECK(ui_test_open("Gallery"));
	ui_test_wait(800);
	lv_obj_t *thumbs[16];
	CHECK(ui_test_find_widgets(&lv_3dtexture_class, thumbs, 16) == 12);
	ui_test_tap(thumbs[0]);
	CHECK(ui_test_shows("Photo"));
	CHECK(ui_test_shows("14 March 2026, 09:12"));
	CHECK(ui_test_shows("1 of 12"));
	CHECK(ui_test_shows(LV_SYMBOL_TRASH));
	/* A swipe to the left: the one before */
	ui_test_swipe(560, 700, 160, 700, 200);
	CHECK(ui_test_shows("2 of 12"));
	CHECK(ui_test_shows("14 March 2026, 08:47"));
	ui_test_swipe(160, 700, 560, 700, 200);
	CHECK(ui_test_shows("1 of 12"));
	/* The trash asks first */
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_shows(LV_SYMBOL_TRASH "  Delete photo"));
	CHECK(ui_test_shows("Cancel"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH "  Delete photo"));
	shot("gallery-photo");
}
