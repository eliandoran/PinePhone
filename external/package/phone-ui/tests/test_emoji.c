/*
 * emoji.c, included for its fonts, with its images in the test's /var/lib/emoji: the
 * browser's (../wasm) arrive after they were first drawn.
 */

#include <stdio.h>
#include <sys/stat.h>

#include "test.h"

#define EMOJI_DIR "/var/lib/emoji"
#include "../src/emoji.c"

TEST_SETUP
{
	lv_init();
	lv_display_create(720, 1440);
	mkdir(EMOJI_DIR, 0755);
}

/* The flake's image of `name` into EMOJI_DIR */
static void arrive(const char *name)
{
	char from[256], to[256], buf[4096];
	snprintf(from, sizeof(from), EMOJI_IMAGES "/%s.png", name);
	snprintf(to, sizeof(to), EMOJI_DIR "/%s.png", name);
	FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
	CHECK(in && out);
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
}

static int32_t width(lv_obj_t *label)
{
	lv_obj_update_layout(label);
	return lv_obj_get_width(label);
}

TEST(reload_draws_emoji_that_arrived)
{
	lv_obj_t *label = lv_label_create(lv_screen_active());
	lv_obj_set_style_text_font(label, &ui_emoji_24, 0);
	lv_label_set_text(label, "\xf0\x9f\x98\x80"); /* U+1F600 */
	lv_font_glyph_dsc_t dsc;
	CHECK(!lv_font_get_glyph_dsc(&ui_emoji_24, &dsc, 0x1f600, 0) ||
	      dsc.format != LV_FONT_GLYPH_FORMAT_IMAGE);
	int32_t before = width(label);

	arrive("1f600");
	ui_emoji_reload();
	CHECK(lv_font_get_glyph_dsc(&ui_emoji_24, &dsc, 0x1f600, 0));
	CHECK_INT(dsc.format, LV_FONT_GLYPH_FORMAT_IMAGE);
	/* the label laid out again with it: the image 24 + 24 / 8 wide, and its spacing */
	CHECK(width(label) != before);
	CHECK_INT(width(label), 27 + 27 / 8);
}
