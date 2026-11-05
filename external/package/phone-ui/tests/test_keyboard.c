/*
 * The on-screen keyboard's emoji (keyboard.c): the panel over the keys, its categories and
 * recent emoji, and backspace deleting whole emoji. Typed in a new note.
 */

#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define TO_EMOJI "\xf0\x9f\x98\x80"  /* 😀: the key, and the smileys' tab */
#define RECENT "\xf0\x9f\x95\x98"    /* 🕘 */
#define ANIMALS "\xf0\x9f\x90\xbb"   /* 🐻 */
#define FLAGS "\xf0\x9f\x8f\x81"     /* 🏁 */

extern const char *const emoji_list[];
extern const unsigned emoji_category_end[];

TEST_SETUP
{
	ui_test_reset();
}

static bool new_note(void)
{
	return ui_test_open("Notes") && ui_test_tap_text(LV_SYMBOL_PLUS) && ui_test_textarea() != NULL;
}

/* The grid's button of an emoji: its text composed for the fonts */
static bool tap_emoji(const char *emoji)
{
	char *composed = ui_emoji_compose(emoji);
	bool ok = composed && ui_test_tap_button(composed);
	free(composed);
	return ok;
}

TEST(panel_typed_into)
{
	CHECK(new_note());
	CHECK(ui_test_type("Hi "));
	CHECK(ui_test_tap_button(TO_EMOJI));

	/* No recent ones yet: the smileys */
	CHECK(tap_emoji(emoji_list[0]));
	CHECK(tap_emoji(emoji_list[1]));
	char want[64];
	snprintf(want, sizeof(want), "Hi %s%s", emoji_list[0], emoji_list[1]);
	CHECK_STR(ui_test_typed(), want);

	/* Back to the letters, which the panel covered */
	CHECK(!ui_test_tap_button("q"));
	CHECK(ui_test_tap_button("ABC"));
	CHECK(!tap_emoji(emoji_list[1])); /* the panel gone */
	CHECK(ui_test_tap_button("q"));
	snprintf(want, sizeof(want), "Hi %s%sq", emoji_list[0], emoji_list[1]);
	CHECK_STR(ui_test_typed(), want);
}

TEST(recent_first_and_kept)
{
	CHECK(new_note());
	CHECK(ui_test_tap_button(TO_EMOJI));
	CHECK(tap_emoji(emoji_list[0]));
	CHECK(ui_test_tap_button(ANIMALS));
	const char *bear = emoji_list[emoji_category_end[1]]; /* the first animal */
	CHECK(tap_emoji(bear));
	CHECK(tap_emoji(emoji_list[emoji_category_end[1] + 1]));
	CHECK(tap_emoji(bear)); /* again: first, once */
	char want[256];
	snprintf(want, sizeof(want), "emoji_recent=%s %s %s\n", bear, emoji_list[emoji_category_end[1] + 1],
	         emoji_list[0]);
	FILE *f = fopen(SETTINGS, "r");
	char got[256] = "";
	if (f) {
		got[fread(got, 1, sizeof(got) - 1, f)] = '\0';
		fclose(f);
	}
	CHECK(strstr(got, want) != NULL);

	/* Opened again: the recent ones, newest first */
	CHECK(ui_test_tap_button("ABC"));
	CHECK(ui_test_tap_button(TO_EMOJI));
	lv_obj_t *ta = ui_test_textarea();
	lv_textarea_set_text(ta, "");
	CHECK(tap_emoji(bear));
	CHECK(tap_emoji(emoji_list[0]));
	snprintf(want, sizeof(want), "%s%s", bear, emoji_list[0]);
	CHECK_STR(ui_test_typed(), want);
}

/* At most 32, the oldest dropped */
TEST(recent_ones_capped)
{
	CHECK(new_note());
	CHECK(ui_test_tap_button(TO_EMOJI));
	for (int i = 0; i < 34; i++)
		CHECK(tap_emoji(emoji_list[i]));
	CHECK(ui_test_tap_button(RECENT));
	char *oldest = ui_emoji_compose(emoji_list[2]), *gone = ui_emoji_compose(emoji_list[1]);
	CHECK(ui_test_tap_button(oldest));
	CHECK(!ui_test_tap_button(gone));
	free(oldest);
	free(gone);
}

/* Saved before: shown first, unknown ones skipped */
TEST(recent_ones_from_the_settings)
{
	char settings[256];
	snprintf(settings, sizeof(settings), "emoji_recent=%s nope %s\n", emoji_list[5], emoji_list[7]);
	ui_test_write(SETTINGS, settings);
	CHECK(new_note());
	CHECK(ui_test_tap_button(TO_EMOJI));
	CHECK(tap_emoji(emoji_list[7]));
	CHECK(!tap_emoji(emoji_list[6])); /* not a recent one */
	CHECK_STR(ui_test_typed(), emoji_list[7]);
}

/* A category's end: scrolled to */
TEST(far_down_a_category)
{
	CHECK(new_note());
	CHECK(ui_test_tap_button(TO_EMOJI));
	CHECK(ui_test_tap_button(FLAGS));
	const char *last = emoji_list[emoji_category_end[8] - 1];
	CHECK(tap_emoji(last));
	CHECK_STR(ui_test_typed(), last);
}

/* Backspace: the whole of an emoji, from the keys and from the panel */
static void deletes_to(const char *text, const char *left, bool from_panel)
{
	lv_obj_t *ta = ui_test_textarea();
	if (!ta)
		return;
	lv_textarea_set_text(ta, text);
	lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
	if (from_panel)
		CHECK(ui_test_tap_button(LV_SYMBOL_BACKSPACE));
	else
		CHECK(ui_test_type("\b"));
	if (strcmp(ui_test_typed(), left) != 0)
		fprintf(stderr, "  from \"%s\"\n", text);
	CHECK_STR(ui_test_typed(), left);
}

TEST(backspace_whole_emoji)
{
	CHECK(new_note());
	deletes_to("ab", "a", false);
	deletes_to("a\xc3\xa9", "a", false);                                            /* é */
	deletes_to("a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", "a", false);                     /* 👍🏽 */
	deletes_to("a1\xef\xb8\x8f\xe2\x83\xa3", "a", false);                            /* 1️⃣ */
	deletes_to("a\xe2\x9d\xa4\xef\xb8\x8f", "a", false);                             /* ❤️ */
	/* 👨‍👩‍👧 */
	deletes_to("a\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", "a", false);
	/* 🇫🇷🇩🇪: one flag; 🇫🇷🇩: the odd one */
	deletes_to("\xf0\x9f\x87\xab\xf0\x9f\x87\xb7\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa",
	           "\xf0\x9f\x87\xab\xf0\x9f\x87\xb7", false);
	deletes_to("\xf0\x9f\x87\xab\xf0\x9f\x87\xb7\xf0\x9f\x87\xa9", "\xf0\x9f\x87\xab\xf0\x9f\x87\xb7", false);
	/* 🏴󠁧󠁢󠁳󠁣󠁴󠁿 Scotland: a tag sequence */
	deletes_to("a\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xb3\xf3\xa0\x81\xa3"
	           "\xf3\xa0\x81\xb4\xf3\xa0\x81\xbf", "a", false);
	deletes_to("", "", false);

	CHECK(ui_test_tap_button(TO_EMOJI));
	deletes_to("a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", "a", true);
}

/* Hidden with the keyboard key: the next showing is letters again */
TEST(keyboard_hidden_and_shown)
{
	CHECK(new_note());
	CHECK(ui_test_tap_button(TO_EMOJI));
	CHECK(ui_test_tap_button("ABC"));
	CHECK(ui_test_key(LV_SYMBOL_KEYBOARD));
	CHECK(ui_test_textarea() == NULL);
}

/* The keys at the left edge type, though the back gesture's strip is there: a tap on
 * the left half of shift and of q, then the back gesture still works above the keys */
TEST(left_edge_keys)
{
	CHECK(new_note());
	lv_obj_t *kb = ui_test_find_widget(&lv_keyboard_class);
	CHECK(kb != NULL);
	lv_area_t a;
	lv_obj_get_coords(kb, &a);
	int32_t row = (lv_area_get_height(&a) - 20) / 4; /* with its gap */

	ui_test_tap_at(14, a.y1 + 10 + 2 * row + row / 2); /* shift: the sentence's Q lower */
	ui_test_tap_at(14, a.y1 + 10 + row / 2);            /* q */
	CHECK_STR(ui_test_typed(), "q");

	ui_test_swipe(5, a.y1 - 200, 600, a.y1 - 200, 200);
	CHECK(ui_test_textarea() == NULL);
}

/* The middle row's indents (half a key at each end) type the letter beside them */
TEST(middle_row_indents)
{
	CHECK(new_note());
	lv_obj_t *kb = ui_test_find_widget(&lv_keyboard_class);
	CHECK(kb != NULL);
	lv_area_t a;
	lv_obj_get_coords(kb, &a);
	int32_t row = (lv_area_get_height(&a) - 20) / 4;
	int32_t y = a.y1 + 10 + row + row / 2;

	ui_test_tap_at(a.x1 + 20, y); /* A: the sentence starts */
	ui_test_tap_at(a.x2 - 20, y);
	CHECK_STR(ui_test_typed(), "Al");
}
