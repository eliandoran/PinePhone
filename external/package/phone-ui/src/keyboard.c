/*
 * Keyboard: LVGL's keyboard widget with a phone layout and its own key handler
 * (LVGL's only knows its own mode keys). Keys type on release, except backspace,
 * which repeats while held and deletes whole emoji. Shift lasts one character,
 * twice quickly locks it; sentences start in upper case. The emoji key opens a
 * panel of emoji over the keys (below). In a field of prose (ui_textarea_set_prose()) a
 * strip over the keys offers corrections of the word being typed (correct.c), one of
 * which a space or punctuation applies (further below).
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "correct.h"
#include "i18n.h"
#include "ui.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h" /* its keys' areas */

enum kb_mode { KB_LOWER, KB_UPPER, KB_LOCK, KB_SYMBOLS, KB_MORE, KB_PHONE };

#define KB_SHIFT LV_SYMBOL_UP
#define KB_LOCKED LV_SYMBOL_UPLOAD /* an arrow on a bar: caps lock */
#define KB_TO_SYMBOLS "?123"
#define KB_TO_LETTERS "ABC"
#define KB_TO_MORE "=\\<"
#define KB_TO_EMOJI "\xf0\x9f\x98\x80" /* 😀 */
#define KB_DOUBLE_MS 400           /* shift twice within this: caps lock */
#define KB_HEIGHT 440              /* the keys; the strip of corrections comes on top */
#define KB_STRIP_H 72
#define KB_MARGIN 30               /* tenths of a bit a correction must beat a known word by */
#define KB_OTHER_LANGUAGE 20       /* what a word of another language than the UI's costs more: 2 bits, past
                                    * the names and English that leak into the Romanian list */
#define KB_DICTS 8                 /* dictionaries used at once: every language's */
#define KB_SEPARATORS " \n.,!?;)\"" /* the keys that end a word, applying its correction */
/* A text area of prose (ui_textarea_set_prose()) is in LV_STATE_USER_1, one whose
 * Enter sends (ui_textarea_set_enter_sends()) in LV_STATE_USER_2 */
#define KB_GAP_PX 10               /* around and between the keys */
#define KB_CURSOR_STEP 24          /* px along the space bar: the cursor moves a character */
#define KB_ROWS 4
/* The bottom row, as the button matrix divides its height (the rest goes to it) */
#define KB_KEYS_H (KB_HEIGHT - 2 * KB_GAP_PX - (KB_ROWS - 1) * KB_GAP_PX)
#define KB_BOTTOM_H (KB_KEYS_H - KB_KEYS_H * (KB_ROWS - 1) / KB_ROWS)

#define K(w) (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))
#define KC(w) (K(w) | LV_BUTTONMATRIX_CTRL_CHECKED) /* control keys, gray */
#define KB_GAP (LV_BUTTONMATRIX_CTRL_HIDDEN | 1)   /* half a key, row 2 */
#define KB_BACKSPACE (LV_BUTTONMATRIX_CTRL_CHECKED | 3)

/* 20 units per row; the bottom row is the same in every mode except its first key */
#define KB_BOTTOM(mode_key) mode_key, LV_SYMBOL_KEYBOARD, KB_TO_EMOJI, ",", " ", ".", LV_SYMBOL_NEW_LINE, ""
#define KB_BOTTOM_CTRL KC(3), KC(2), KC(2), K(2), K(6), K(2), KC(3)

static const char *const kb_lower[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	" ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
	KB_SHIFT, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

static const char *const kb_upper[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
	KB_SHIFT, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

static const char *const kb_lock[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	" ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
	KB_LOCKED, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_SYMBOLS),
};

/* Shift unchecked (as light as the letters) while it is on */
static const lv_buttonmatrix_ctrl_t kb_letters_ctrl[2][37] = {
	{
		K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
		KB_GAP, K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_GAP,
		KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
		KB_BOTTOM_CTRL,
	},
	{
		K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
		KB_GAP, K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_GAP,
		K(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
		KB_BOTTOM_CTRL,
	},
};

/* Symbols as on Android (Gboard): ?123, then =\< for the second page. The pages
 * have the same key sizes: switching does not move them. */

static const char *const kb_symbols[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"@", "#", "$", "_", "&", "-", "+", "(", ")", "/", "\n",
	KB_TO_MORE, "*", "\"", "'", ":", ";", "!", "?", LV_SYMBOL_BACKSPACE, "\n",
	KB_BOTTOM(KB_TO_LETTERS),
};

static const lv_buttonmatrix_ctrl_t kb_symbols_ctrl[] = {
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
	KB_BOTTOM_CTRL,
};

static const char *const kb_more[] = {
	"~", "`", "|", "•", "√", "π", "÷", "×", "¶", "∆", "\n",
	"£", "¢", "€", "¥", "^", "°", "=", "{", "}", "\\", "\n",
	KB_TO_SYMBOLS, "%", "©", "®", "™", "✓", "[", "]", LV_SYMBOL_BACKSPACE, "\n",
	KB_TO_LETTERS, LV_SYMBOL_KEYBOARD, "<", " ", ">", LV_SYMBOL_NEW_LINE, "",
};

/* As the symbols, with its own bottom row */
static const lv_buttonmatrix_ctrl_t kb_more_ctrl[] = {
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2),
	KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
	KC(3), KC(2), K(2), K(8), K(2), KC(3),
};

/* What phone numbers are written with: also what marks a text area as one */
static const char kb_phone_chars[] = "0123456789+*#()- ";

/* Phone numbers (ui_textarea_set_phone()): a dial pad with what numbers are written
 * with on the sides */
static const char *const kb_phone[] = {
	"+", "1", "2", "3", "-", "\n",
	"(", "4", "5", "6", " ", "\n",
	")", "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
	LV_SYMBOL_KEYBOARD, "*", "0", "#", LV_SYMBOL_NEW_LINE, "",
};

static const lv_buttonmatrix_ctrl_t kb_phone_ctrl[] = {
	KC(2), K(3), K(3), K(3), KC(2),
	KC(2), K(3), K(3), K(3), KC(2),
	KC(2), K(3), K(3), K(3), LV_BUTTONMATRIX_CTRL_CHECKED | 2, /* repeats */
	KC(2), K(3), K(3), K(3), KC(2),
};

static uint32_t kb_shift_time;

/* A keyboard's own (its user data): its mode, the emoji panel, and the strip of
 * corrections with the word being typed */
struct kb {
	enum kb_mode mode;
	lv_obj_t *panel;                     /* the emoji panel, once opened */
	lv_obj_t *strip, *slot[CORRECT_MAX]; /* a box per word offered, a label in each */
	const struct correct_dict *dicts[KB_DICTS]; /* every language's, the UI's first; none: no corrections */
	int dict_count;
	bool english;                        /* one of them is English: its i is I */
	lv_point_t press;                    /* where the finger landed last, on the screen */
	struct correct_touch touches[CORRECT_LETTERS_MAX]; /* the word's letters as pressed */
	int touch_count;
	char text[CORRECT_MAX][CORRECT_WORD_MAX]; /* the slots' words, in the typed word's case */
	int apply;                                /* the slot a separator puts in, -1: none */
	bool kept;                                /* the word put back by backspace: left so */
	bool undo;                                /* a correction was applied just now */
	char undo_typed[CORRECT_WORD_MAX];        /* what backspace puts back */
	char log[320]; /* the word's touches and candidates, logged when it ends */
	uint32_t undo_pos;                        /* the cursor then, after the separator */
};

static struct kb *kb_of(lv_obj_t *kb)
{
	return lv_obj_get_user_data(kb);
}

static void kb_set_mode(lv_obj_t *kb, enum kb_mode mode)
{
	static const char *const *const maps[] = { kb_lower, kb_upper, kb_lock, kb_symbols, kb_more, kb_phone };
	static const lv_buttonmatrix_ctrl_t *const ctrls[] = {
		kb_letters_ctrl[0], kb_letters_ctrl[1], kb_letters_ctrl[1], kb_symbols_ctrl, kb_more_ctrl,
		kb_phone_ctrl,
	};
	kb_of(kb)->mode = mode;
	lv_buttonmatrix_set_map(kb, maps[mode]);
	lv_buttonmatrix_set_ctrl_map(kb, ctrls[mode]);
}

static enum kb_mode kb_get_mode(lv_obj_t *kb)
{
	return kb_of(kb)->mode;
}

/* The cursor is at the start of the text, of a line, or after ". " (or ! ?) */
static bool kb_sentence_start(lv_obj_t *ta)
{
	const char *text = lv_textarea_get_text(ta);
	/* The cursor position counts characters, not bytes */
	size_t i = 0;
	for (uint32_t n = lv_textarea_get_cursor_pos(ta); n && text[i]; n--)
		for (i++; ((unsigned char)text[i] & 0xc0) == 0x80; i++)
			;
	size_t end = i;
	while (i && text[i - 1] == ' ')
		i--;
	return i == 0 || text[i - 1] == '\n' ||
	       (i < end && (text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?'));
}

/* Letters in the case for the cursor position, unless caps lock or symbols are on */
static void kb_auto_shift(lv_obj_t *kb)
{
	enum kb_mode mode = kb_get_mode(kb);
	if (mode != KB_LOWER && mode != KB_UPPER)
		return;
	enum kb_mode want = kb_sentence_start(lv_keyboard_get_textarea(kb)) ? KB_UPPER : KB_LOWER;
	if (want != mode)
		kb_set_mode(kb, want);
}

/* Backspace: a whole emoji, whatever it is made of */

/* The byte offset of the cursor; the code point before it, 0 at the start */
static uint32_t kb_char_before(const char *text, size_t *end)
{
	size_t i = *end;
	if (!i)
		return 0;
	while (--i && ((unsigned char)text[i] & 0xc0) == 0x80)
		;
	*end = i;
	const unsigned char *s = (const unsigned char *)text + i;
	return s[0] < 0x80 ? s[0] :
	       s[0] < 0xe0 ? (s[0] & 0x1fu) << 6 | (s[1] & 0x3f) :
	       s[0] < 0xf0 ? (s[0] & 0x0fu) << 12 | (s[1] & 0x3fu) << 6 | (s[2] & 0x3f) :
	       (s[0] & 0x07u) << 18 | (s[1] & 0x3fu) << 12 | (s[2] & 0x3fu) << 6 | (s[3] & 0x3f);
}

static size_t kb_cursor_byte(lv_obj_t *ta)
{
	const char *text = lv_textarea_get_text(ta);
	size_t i = 0;
	for (uint32_t n = lv_textarea_get_cursor_pos(ta); n && text[i]; n--)
		for (i++; ((unsigned char)text[i] & 0xc0) == 0x80; i++)
			;
	return i;
}

/* What attaches to the character before it: emoji presentation, skin tones,
 * keycap, tags (subdivision flags) */
static bool kb_modifier(uint32_t c)
{
	return c == 0xfe0f || (c >= 0x1f3fb && c <= 0x1f3ff) || c == 0x20e3 || (c >= 0xe0020 && c <= 0xe007f);
}

static bool kb_regional(uint32_t c)
{
	return c >= 0x1f1e6 && c <= 0x1f1ff;
}

/* The character before the cursor, and the rest of its emoji: the characters
 * its modifiers attach to, those joined to it (U+200D), the first half of a flag */
static void kb_delete(lv_obj_t *ta)
{
	size_t end = kb_cursor_byte(ta);
	uint32_t c = kb_char_before(lv_textarea_get_text(ta), &end);
	lv_textarea_delete_char(ta);
	for (;;) {
		const char *text = lv_textarea_get_text(ta);
		end = kb_cursor_byte(ta);
		size_t at = end;
		uint32_t p = kb_char_before(text, &at);
		if (!p)
			return;
		if (kb_modifier(c)) {
			lv_textarea_delete_char(ta);
			c = p;
		} else if (p == 0x200d) {
			lv_textarea_delete_char(ta);
			end = kb_cursor_byte(ta);
			c = kb_char_before(lv_textarea_get_text(ta), &end);
			if (!c)
				return;
			lv_textarea_delete_char(ta);
		} else if (kb_regional(c) && kb_regional(p)) {
			/* Flags are pairs from the first: c ended one if an odd number is left */
			unsigned n = 0;
			for (at = end; kb_regional(kb_char_before(text, &at)); n++)
				;
			if (n % 2)
				lv_textarea_delete_char(ta);
			return;
		} else {
			return;
		}
	}
}

/*
 * Emoji: a panel over the keyboard (its child, as large as its keys), a grid of a
 * category's emoji scrolled vertically, and tabs: back to letters, the recent
 * ones, the categories (emoji_list.c, gen-emoji.sh), backspace. The grids are
 * button matrices whose maps (composed for the fonts) are made once and kept;
 * the recent ones (setting "emoji_recent") are mapped again when shown.
 */

extern const char *const emoji_list[];       /* emoji_list.c */
extern const unsigned emoji_category_end[];

#define EMOJI_CATEGORIES 9
#define EMOJI_COLUMNS 8
#define EMOJI_ROW_H 88
#define EMOJI_RECENT 32
#define EMOJI_TAB_RECENT 1 /* tabs: letters, recent, the categories, backspace */

static const char *const kb_emoji_tabs[] = {
	KB_TO_LETTERS, "\xf0\x9f\x95\x98", /* 🕘 */
	"\xf0\x9f\x98\x80", "\xf0\x9f\x91\x8b", "\xf0\x9f\x90\xbb", "\xf0\x9f\x8d\x94", /* 😀 👋 🐻 🍔 */
	"\xe2\x9a\xbd", "\xf0\x9f\x9a\x97", "\xf0\x9f\x92\xa1", "\xf0\x9f\x94\xa3",     /* ⚽ 🚗 💡 🔣 */
	"\xf0\x9f\x8f\x81", LV_SYMBOL_BACKSPACE, "",                                    /* 🏁 */
};

static const lv_buttonmatrix_ctrl_t kb_emoji_tabs_ctrl[] = {
	KC(3), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), K(2), KB_BACKSPACE,
};

static struct {
	const char **composed;        /* emoji_list for the fonts, made when first shown */
	const char **maps[EMOJI_CATEGORIES + 2]; /* by tab */
	/* The recent ones', remade in place: other keyboards' panels may show it */
	const char *recent_map[EMOJI_RECENT + EMOJI_RECENT / EMOJI_COLUMNS + 1];
	int recent[EMOJI_RECENT];     /* indexes in emoji_list, most recent first */
	int recent_count;
	bool recent_loaded;
} emoji;

/* The panel of a keyboard: its first child, made when first opened */
struct kb_emoji_panel {
	lv_obj_t *kb, *scroll, *grid, *tabs;
	int tab;
};

static int emoji_count(void)
{
	return (int)emoji_category_end[EMOJI_CATEGORIES - 1];
}

static int emoji_category_start(int category)
{
	return category ? (int)emoji_category_end[category - 1] : 0;
}

static void emoji_recent_load(void)
{
	if (emoji.recent_loaded)
		return;
	emoji.recent_loaded = true;
	char *list = strdup(ui_setting_get("emoji_recent", ""));
	char *save = NULL;
	for (char *e = list ? strtok_r(list, " ", &save) : NULL; e && emoji.recent_count < EMOJI_RECENT;
	     e = strtok_r(NULL, " ", &save))
		for (int i = 0; i < emoji_count(); i++)
			if (!strcmp(emoji_list[i], e)) {
				emoji.recent[emoji.recent_count++] = i;
				break;
			}
	free(list);
}

/* Typed: first in the recent ones, saved */
static void emoji_recent_add(int index)
{
	int n = 0;
	while (n < emoji.recent_count && emoji.recent[n] != index)
		n++;
	if (n == emoji.recent_count && n < EMOJI_RECENT)
		emoji.recent_count++;
	if (n == EMOJI_RECENT)
		n--;
	memmove(emoji.recent + 1, emoji.recent, n * sizeof(*emoji.recent));
	emoji.recent[0] = index;

	char buf[EMOJI_RECENT * 40] = "";
	size_t len = 0;
	for (int i = 0; i < emoji.recent_count && len < sizeof(buf); i++)
		len += snprintf(buf + len, sizeof(buf) - len, "%s%s", i ? " " : "", emoji_list[emoji.recent[i]]);
	if (len < sizeof(buf))
		ui_setting_set("emoji_recent", buf);
}

/* The emoji of a tab, indexes in emoji_list */
static int emoji_tab_count(int tab)
{
	return tab == EMOJI_TAB_RECENT ? emoji.recent_count :
	       (int)emoji_category_end[tab - 2] - emoji_category_start(tab - 2);
}

static int emoji_tab_index(int tab, int i)
{
	return tab == EMOJI_TAB_RECENT ? emoji.recent[i] : emoji_category_start(tab - 2) + i;
}

/* A grid's map: the emoji in rows of EMOJI_COLUMNS */
static const char **emoji_map(int tab)
{
	if (!emoji.composed) {
		emoji.composed = malloc(emoji_count() * sizeof(*emoji.composed));
		if (!emoji.composed)
			return NULL;
		for (int i = 0; i < emoji_count(); i++) {
			char *c = ui_emoji_compose(emoji_list[i]);
			emoji.composed[i] = c ? c : "";
		}
	}
	if (emoji.maps[tab])
		return emoji.maps[tab];
	int count = emoji_tab_count(tab);
	const char **map = tab == EMOJI_TAB_RECENT ? emoji.recent_map :
	                   malloc((count + count / EMOJI_COLUMNS + 1) * sizeof(*map));
	if (!map)
		return NULL;
	if (tab != EMOJI_TAB_RECENT)
		emoji.maps[tab] = map;
	int n = 0;
	for (int i = 0; i < count; i++) {
		if (i && i % EMOJI_COLUMNS == 0)
			map[n++] = "\n";
		map[n++] = emoji.composed[emoji_tab_index(tab, i)];
	}
	map[n] = "";
	return map;
}

static void emoji_tab_show(struct kb_emoji_panel *p, int tab)
{
	const char **map = emoji_map(tab);
	if (!map)
		return;
	int count = emoji_tab_count(tab);
	p->tab = tab;
	/* The tab shown is gray */
	lv_buttonmatrix_set_ctrl_map(p->tabs, kb_emoji_tabs_ctrl);
	lv_buttonmatrix_set_button_ctrl(p->tabs, tab, LV_BUTTONMATRIX_CTRL_CHECKED);
	lv_obj_set_hidden(p->grid, !count);
	if (count) {
		lv_buttonmatrix_set_map(p->grid, map);
		lv_buttonmatrix_set_button_ctrl_all(p->grid, LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
		                                    LV_BUTTONMATRIX_CTRL_NO_REPEAT);
		/* Full rows: the last one's emoji as wide as the others */
		int rows = (count + EMOJI_COLUMNS - 1) / EMOJI_COLUMNS;
		lv_obj_set_size(p->grid, LV_PCT(100 * LV_MIN(count, EMOJI_COLUMNS) / EMOJI_COLUMNS),
		                rows * EMOJI_ROW_H);
	}
	lv_obj_scroll_to_y(p->scroll, 0, LV_ANIM_OFF);
}

static void emoji_grid_cb(lv_event_t *e)
{
	struct kb_emoji_panel *p = lv_event_get_user_data(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(p->kb);
	uint32_t id = lv_buttonmatrix_get_selected_button(p->grid);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || !ta || (int)id >= emoji_tab_count(p->tab))
		return;
	int index = emoji_tab_index(p->tab, id);
	ui_vibrate(UI_VIBRATE_TAP);
	lv_textarea_add_text(ta, emoji_list[index]);
	emoji_recent_add(index);
}

static void emoji_tabs_cb(lv_event_t *e)
{
	struct kb_emoji_panel *p = lv_event_get_user_data(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(p->kb);
	uint32_t id = lv_buttonmatrix_get_selected_button(p->tabs);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || !ta)
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	const char *key = lv_buttonmatrix_get_button_text(p->tabs, id);
	if (strcmp(key, KB_TO_LETTERS) == 0) {
		lv_obj_set_hidden(lv_obj_get_parent(p->tabs), true);
		kb_set_mode(p->kb, KB_LOWER);
		kb_auto_shift(p->kb);
	} else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
		kb_delete(ta);
	} else if ((int)id != p->tab) {
		emoji_tab_show(p, id);
	}
}

static void emoji_panel_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* Pressed, not taking the focus from the text area (its cursor), as LVGL's keyboard: a
 * press on a focusable object defocuses the last focusable one pressed */
static void kb_unfocusable(lv_obj_t *obj)
{
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

/* Keys as the keyboard's: transparent ones in the grid */
static void emoji_keys_style(lv_obj_t *bm, bool keys)
{
	kb_unfocusable(bm);
	lv_obj_set_style_pad_all(bm, 0, 0);
	lv_obj_set_style_pad_gap(bm, keys ? KB_GAP_PX : 0, 0);
	lv_obj_set_style_border_width(bm, 0, 0);
	lv_obj_set_style_radius(bm, 0, 0);
	lv_obj_set_style_shadow_width(bm, 0, 0);
	lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(bm, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_radius(bm, 12, LV_PART_ITEMS);
	lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(bm, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_color_filter_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(bm, keys ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(bm, lv_color_hex(UI_COLOR_CARD), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(bm, lv_color_hex(UI_COLOR_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(bm, lv_color_hex(UI_COLOR_BUTTON), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(bm, lv_color_hex(UI_COLOR_BUTTON_PRESSED),
	                          LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(bm, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
}

/* Over the keys, the recent ones first if any */
static void emoji_panel_open(lv_obj_t *kb)
{
	lv_obj_t *panel = kb_of(kb)->panel;
	struct kb_emoji_panel *p;
	if (panel) {
		p = lv_obj_get_user_data(panel);
	} else {
		p = calloc(1, sizeof(*p));
		if (!p)
			return;
		p->kb = kb;
		panel = kb_of(kb)->panel = ui_box(kb);
		lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
		lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_BAR), 0);
		/* Presses on it do not reach the keys below */
		lv_obj_set_clickable(panel, true);
		kb_unfocusable(panel);
		lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(panel, KB_GAP_PX, 0);
		lv_obj_set_user_data(panel, p);
		lv_obj_add_event_cb(panel, emoji_panel_delete_cb, LV_EVENT_DELETE, p);

		p->scroll = lv_obj_create(panel);
		lv_obj_remove_style_all(p->scroll);
		kb_unfocusable(p->scroll);
		lv_obj_set_width(p->scroll, LV_PCT(100));
		lv_obj_set_flex_grow(p->scroll, 1);
		lv_obj_set_scroll_dir(p->scroll, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(p->scroll, LV_SCROLLBAR_MODE_OFF);

		p->grid = lv_buttonmatrix_create(p->scroll);
		emoji_keys_style(p->grid, false);
		lv_obj_set_style_text_font(p->grid, &ui_font_48, LV_PART_ITEMS);
		lv_obj_add_event_cb(p->grid, emoji_grid_cb, LV_EVENT_VALUE_CHANGED, p);

		p->tabs = lv_buttonmatrix_create(panel);
		emoji_keys_style(p->tabs, true);
		/* Where the keyboard's bottom row is, as high */
		lv_obj_set_size(p->tabs, LV_PCT(100), KB_BOTTOM_H);
		lv_obj_set_style_text_font(p->tabs, &ui_font_32, LV_PART_ITEMS);
		lv_buttonmatrix_set_map(p->tabs, kb_emoji_tabs);
		lv_obj_add_event_cb(p->tabs, emoji_tabs_cb, LV_EVENT_VALUE_CHANGED, p);
	}
	emoji_recent_load();
	lv_obj_set_hidden(panel, false);
	emoji_tab_show(p, emoji.recent_count ? EMOJI_TAB_RECENT : EMOJI_TAB_RECENT + 1);
}

/*
 * Corrections: the word before the cursor (back to a space or a line break), when made
 * of the language's letters, is given to correct.c with where each key was pressed (the
 * letters typed here one after the other; dead centre when the cursor was moved or the
 * word made otherwise). The strip shows the word as typed first, then the words meant,
 * the one a separator puts in (KB_SEPARATORS, Enter) in the text's color: the likeliest
 * when the typed word is unknown, or likelier by KB_MARGIN than it. Every language's dictionary is asked
 * (a Romanian types English too): a word known in any is known, one of another language
 * than the UI's costs KB_OTHER_LANGUAGE more. Tapping a slot puts
 * that word and a space in. Backspace right after a correction puts the typed word
 * back, kept so at the next separator. The typed word's case carries over: a capital
 * first letter, all capitals; English's i is I.
 */

/* Upper-case letters without a key (Romanian's), and their lower case */
static const struct {
	const char *upper, *lower;
} kb_cases[] = {
	{ "\xc4\x82", "\xc4\x83" }, /* Ă ă */
	{ "\xc3\x82", "\xc3\xa2" }, /* Â â */
	{ "\xc3\x8e", "\xc3\xae" }, /* Î î */
	{ "\xc8\x98", "\xc8\x99" }, /* Ș ș */
	{ "\xc8\x9a", "\xc8\x9b" }, /* Ț ț */
};

static bool kb_strip_on(lv_obj_t *kb)
{
	struct kb *k = kb_of(kb);
	return k->strip && !lv_obj_is_hidden(k->strip);
}

/* Characters in the bytes: those not continuing a previous one */
static uint32_t kb_chars(const char *s, size_t len)
{
	uint32_t n = 0;
	for (size_t i = 0; i < len; i++)
		n += ((unsigned char)s[i] & 0xc0) != 0x80;
	return n;
}

/* The word's start: before the cursor, back over letters (any character beyond ASCII:
 * kb_word_letters() tells the language's), an apostrophe or a hyphen */
static size_t kb_word_start(const char *text, size_t end)
{
	size_t i = end;
	while (i && (isalpha((unsigned char)text[i - 1]) || (unsigned char)text[i - 1] >= 0x80 ||
	             text[i - 1] == '\'' || text[i - 1] == '-'))
		i--;
	return i;
}

/* Whether the word is part of an address or a code, left as typed: between the spaces
 * around it a slash, an at sign, a colon, a digit or a dot inside */
static bool kb_address(const char *text, size_t end)
{
	size_t i = end;
	while (i && text[i - 1] != ' ' && text[i - 1] != '\n')
		i--;
	for (; i < end; i++)
		if (text[i] == '/' || text[i] == '@' || text[i] == ':' || isdigit((unsigned char)text[i]) ||
		    (text[i] == '.' && i + 1 < end))
			return true;
	return false;
}

/* The typed word in lower case, as the dictionary's letters, its keys in `keys`
 * (what a letter without a key, ș, is typed on): how many letters, -1 when a character
 * is not a letter of the language (a digit, an address) or there are too many */
static int kb_word_letters(const struct correct_dict *d, const char *word, size_t len, char *lower,
                           char *keys)
{
	int n = 0;
	size_t out = 0;
	for (size_t i = 0; i < len;) {
		if (n == CORRECT_LETTERS_MAX)
			return -1;
		unsigned char c = (unsigned char)word[i];
		if (c < 0x80) {
			bool letter = isalpha(c), inside = (c == '\'' || c == '-') && i > 0 && i + 1 < len;
			if (!letter && !inside)
				return -1;
			lower[out++] = (char)tolower(c);
			keys[n++] = (char)tolower(c);
			i++;
			continue;
		}
		const char *l = word + i;
		for (unsigned u = 0; u < sizeof(kb_cases) / sizeof(*kb_cases); u++)
			if (strncmp(l, kb_cases[u].upper, strlen(kb_cases[u].upper)) == 0)
				l = kb_cases[u].lower;
		int index = -1;
		for (int j = 0; j < d->letter_count && index < 0; j++)
			if (d->letters[j][1] && strncmp(l, d->letters[j], strlen(d->letters[j])) == 0)
				index = j;
		if (index < 0)
			return -1;
		size_t bytes = strlen(d->letters[index]);
		memcpy(lower + out, d->letters[index], bytes);
		out += bytes;
		keys[n++] = d->base[index];
		i += l == word + i ? bytes : strlen(kb_cases[0].upper);
	}
	lower[out] = 0;
	return n;
}

static bool kb_upper_first(const char *word)
{
	if (isupper((unsigned char)*word))
		return true;
	for (unsigned u = 0; u < sizeof(kb_cases) / sizeof(*kb_cases); u++)
		if (strncmp(word, kb_cases[u].upper, strlen(kb_cases[u].upper)) == 0)
			return true;
	return false;
}

/* The word's letters in upper case into `out`, the first only or all */
static void kb_upper_case(const char *word, char *out, bool all)
{
	size_t o = 0;
	for (size_t i = 0; word[i] && o + 4 < CORRECT_WORD_MAX;) {
		bool first = i == 0;
		if ((unsigned char)word[i] < 0x80) {
			out[o++] = (first || all) ? (char)toupper((unsigned char)word[i]) : word[i];
			i++;
		} else {
			const char *l = word + i;
			size_t bytes = ((unsigned char)word[i] & 0xe0) == 0xc0 ? 2 : 3;
			if (first || all)
				for (unsigned u = 0; u < sizeof(kb_cases) / sizeof(*kb_cases); u++)
					if (strncmp(word + i, kb_cases[u].lower, bytes) == 0)
						l = kb_cases[u].upper;
			memcpy(out + o, l, bytes);
			o += bytes;
			i += bytes;
		}
	}
	out[o] = 0;
}

/* A word meant in the case of the typed one; English's i is I */
static void kb_cased(struct kb *k, const char *typed, int typed_letters, const char *word, char *out)
{
	bool all = typed_letters > 1;
	for (const char *p = typed; *p && all; p++)
		if (islower((unsigned char)*p))
			all = false;
	if (kb_upper_first(typed))
		kb_upper_case(word, out, all);
	else if (k->english && word[0] == 'i' && (!word[1] || word[1] == '\''))
		kb_upper_case(word, out, false);
	else
		strcpy(out, word);
}

static void kb_slot_show(struct kb *k, int i, const char *text, bool applied)
{
	strcpy(k->text[i], text);
	lv_obj_t *label = lv_obj_get_child(k->slot[i], 0);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(applied ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);
	lv_obj_set_hidden(k->slot[i], !*text);
}

/* The strip for the word before the cursor */
static void kb_suggest(lv_obj_t *kb)
{
	struct kb *k = kb_of(kb);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	k->apply = -1;
	if (!kb_strip_on(kb) || !ta)
		return;
	const char *text = lv_textarea_get_text(ta);
	size_t end = kb_cursor_byte(ta), start = kb_word_start(text, end);
	char typed[CORRECT_WORD_MAX], lower[CORRECT_WORD_MAX], keys[CORRECT_LETTERS_MAX];
	int n = -1;
	if (end - start < sizeof(typed) && !kb_address(text, end)) {
		memcpy(typed, text + start, end - start);
		typed[end - start] = 0;
		n = kb_word_letters(k->dicts[0], typed, end - start, lower, keys);
	}
	k->log[0] = 0;
	if (n <= 0) {
		for (int i = 0; i < CORRECT_MAX; i++)
			kb_slot_show(k, i, "", false);
		return;
	}

	/* Where each key was pressed, when these letters were typed here */
	struct correct_touch touches[CORRECT_LETTERS_MAX];
	bool pressed = k->touch_count == n;
	for (int i = 0; i < n && pressed; i++)
		pressed = k->touches[i].key == keys[i];
	for (int i = 0; i < n; i++)
		touches[i] = pressed ? k->touches[i] : (struct correct_touch){ keys[i], 0, 0 };
	/* Each dictionary's words, the likeliest first, once each */
	struct correct_word words[CORRECT_MAX * KB_DICTS];
	int found = 0, typed_cost = -1;
	for (int j = 0; j < k->dict_count; j++) {
		int extra = j ? KB_OTHER_LANGUAGE : 0;
		int cost = correct_cost(k->dicts[j], lower);
		if (cost >= 0 && (typed_cost < 0 || cost + extra < typed_cost))
			typed_cost = cost + extra;
		struct correct_word w[CORRECT_MAX];
		int m = n > 1 ? correct(k->dicts[j], touches, n, w) : 0;
		for (int i = 0; i < m; i++) {
			w[i].cost += extra;
			int at = found;
			for (int o = 0; o < found; o++)
				if (strcmp(words[o].text, w[i].text) == 0) { /* known there too */
					if (w[i].cost >= words[o].cost)
						at = -1;
					else
						at = o;
					break;
				}
			if (at < 0)
				continue;
			if (at == found)
				found++;
			while (at > 0 && words[at - 1].cost > w[i].cost) {
				words[at] = words[at - 1];
				at--;
			}
			words[at] = w[i];
		}
	}

	/* The word as typed, then the others, the likeliest first: applied when the typed
	 * word is unknown, or it is likelier by the margin */
	int best = -1;
	for (int i = 0; i < found && best < 0; i++)
		if (strcmp(words[i].text, lower) != 0)
			best = i;
	bool known = typed_cost >= 0; /* as typed, in any language: its frequency */
	bool replace = !k->kept && best >= 0 && (!known || words[best].cost + KB_MARGIN < typed_cost);

	/* For the log: the touches as given (dead centre when not pressed here), the words */
	int len = snprintf(k->log, sizeof(k->log), "keyboard: \"%s\"%s", typed, pressed ? "" : " (centre)");
	for (int i = 0; i < n && pressed && len < (int)sizeof(k->log); i++)
		len += snprintf(k->log + len, sizeof(k->log) - (size_t)len, " %c%+.2f%+.2f", touches[i].key,
		                (double)touches[i].dx, (double)touches[i].dy);
	if (len < (int)sizeof(k->log))
		len += snprintf(k->log + len, sizeof(k->log) - (size_t)len, ": typed %d", typed_cost);
	for (int i = 0; i < found && len < (int)sizeof(k->log); i++)
		len += snprintf(k->log + len, sizeof(k->log) - (size_t)len, ", %s %d", words[i].text, words[i].cost);
	char cased[CORRECT_WORD_MAX];
	kb_cased(k, typed, n, lower, cased);
	bool i_to_upper = strcmp(cased, typed) != 0 && !kb_upper_first(typed); /* English's i */
	kb_slot_show(k, 0, i_to_upper ? cased : typed, !replace);
	if (i_to_upper && !k->kept)
		k->apply = 0;
	int slot = 1;
	for (int i = 0; i < found && slot < CORRECT_MAX; i++) {
		if (strcmp(words[i].text, lower) == 0)
			continue;
		kb_cased(k, typed, n, words[i].text, cased);
		kb_slot_show(k, slot, cased, replace && i == best);
		if (replace && i == best)
			k->apply = slot;
		slot++;
	}
	for (; slot < CORRECT_MAX; slot++)
		kb_slot_show(k, slot, "", false);
}

/* The slot's word in place of the typed one */
static void kb_put(lv_obj_t *kb, int slot)
{
	struct kb *k = kb_of(kb);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	const char *text = lv_textarea_get_text(ta);
	size_t end = kb_cursor_byte(ta), start = kb_word_start(text, end);
	uint32_t chars = kb_chars(text + start, end - start);
	strcpy(k->undo_typed, k->text[0]);
	while (chars--)
		lv_textarea_delete_char(ta);
	lv_textarea_add_text(ta, k->text[slot]);
	k->touch_count = 0;
}

/* A separator typed: the correction first, remembered for backspace */
static void kb_separator(lv_obj_t *kb, const char *key)
{
	struct kb *k = kb_of(kb);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	bool applied = k->apply >= 0 && strcmp(k->text[k->apply], k->text[0]) != 0;
	if (k->log[0])
		printf("phone-ui: %s; %s%s\n", k->log, applied ? "put " : "kept", applied ? k->text[k->apply] : "");
	if (k->apply >= 0)
		kb_put(kb, k->apply);
	if (key)
		lv_textarea_add_text(ta, key);
	k->kept = false;
	k->touch_count = 0;
	if (applied && key) {
		k->undo = true;
		k->undo_pos = lv_textarea_get_cursor_pos(ta);
	}
}

/* Backspace right after a correction: the typed word back, kept */
static bool kb_undo(lv_obj_t *kb)
{
	struct kb *k = kb_of(kb);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	if (lv_textarea_get_cursor_pos(ta) != k->undo_pos)
		return false;
	const char *text = lv_textarea_get_text(ta);
	size_t end = kb_cursor_byte(ta);
	uint32_t sep = kb_char_before(text, &end); /* the separator */
	if (!sep || sep > 0x7f)
		return false;
	size_t start = kb_word_start(text, end);
	uint32_t chars = 1 + kb_chars(text + start, end - start);
	while (chars--)
		lv_textarea_delete_char(ta);
	lv_textarea_add_text(ta, k->undo_typed);
	k->kept = true;
	k->touch_count = 0;
	return true;
}

static void kb_slot_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target_obj(e)));
	int slot = (int)(intptr_t)lv_event_get_user_data(e);
	struct kb *k = kb_of(kb);
	if (!k->text[slot][0] || !lv_keyboard_get_textarea(kb))
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	k->apply = slot;
	kb_separator(kb, " ");
	k->undo = false;
	kb_auto_shift(kb);
	kb_suggest(kb);
}

/* A letter typed: where the finger landed on its key, in key widths and heights from
 * its centre, for the correction; dead centre when no press landed on it (keys
 * pressed otherwise: the tests') */
static void kb_touch(lv_obj_t *kb, uint32_t id, const char *key)
{
	struct kb *k = kb_of(kb);
	if (k->touch_count >= CORRECT_LETTERS_MAX)
		return;
	const lv_buttonmatrix_t *m = (const lv_buttonmatrix_t *)kb;
	lv_area_t c;
	lv_obj_get_coords(kb, &c);
	const lv_area_t *a = &m->button_areas[id];
	float w = (float)lv_area_get_width(a), h = (float)lv_area_get_height(a);
	lv_point_t p = { k->press.x - c.x1, k->press.y - c.y1 };
	lv_area_t on = *a;
	lv_area_increase(&on, KB_GAP_PX, KB_GAP_PX);
	bool landed = p.x >= on.x1 && p.x <= on.x2 && p.y >= on.y1 && p.y <= on.y2;
	k->touches[k->touch_count++] = (struct correct_touch){
		.key = (char)tolower((unsigned char)key[0]),
		.dx = landed ? ((float)p.x - ((float)a->x1 + w / 2)) / w : 0,
		.dy = landed ? ((float)p.y - ((float)a->y1 + h / 2)) / h : 0,
	};
}

static void kb_key_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target_obj(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	uint32_t id = lv_buttonmatrix_get_selected_button(kb);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || !ta)
		return;
	const char *key = lv_buttonmatrix_get_button_text(kb, id);
	struct kb *k = kb_of(kb);
	bool undo = k->undo;
	k->undo = false;
	ui_vibrate(UI_VIBRATE_TAP);

	if (strcmp(key, KB_SHIFT) == 0) {
		if (kb_get_mode(kb) == KB_LOWER) {
			kb_set_mode(kb, KB_UPPER);
			kb_shift_time = lv_tick_get();
		} else {
			kb_set_mode(kb, lv_tick_elaps(kb_shift_time) < KB_DOUBLE_MS ? KB_LOCK : KB_LOWER);
		}
		k->undo = undo;
		return;
	} else if (strcmp(key, KB_LOCKED) == 0 || strcmp(key, KB_TO_LETTERS) == 0) {
		kb_set_mode(kb, KB_LOWER);
		kb_auto_shift(kb);
		k->undo = undo;
		return;
	} else if (strcmp(key, KB_TO_SYMBOLS) == 0) {
		kb_set_mode(kb, KB_SYMBOLS);
		k->undo = undo;
		return;
	} else if (strcmp(key, KB_TO_MORE) == 0) {
		kb_set_mode(kb, KB_MORE);
		k->undo = undo;
		return;
	} else if (strcmp(key, KB_TO_EMOJI) == 0) {
		emoji_panel_open(kb);
		return;
	} else if (strcmp(key, LV_SYMBOL_KEYBOARD) == 0) {
		lv_obj_send_event(ta, LV_EVENT_CANCEL, NULL);
		return;
	} else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0 &&
	           (lv_textarea_get_one_line(ta) || lv_obj_has_state(ta, LV_STATE_USER_2))) {
		kb_separator(kb, NULL); /* the correction goes with the text */
		lv_obj_send_event(ta, LV_EVENT_READY, NULL);
		return;
	} else if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
		if (!(undo && kb_undo(kb))) {
			kb_delete(ta);
			if (k->touch_count > 0)
				k->touch_count--;
		}
	} else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0) {
		kb_separator(kb, "\n");
	} else if (key[0] && !key[1] && strchr(KB_SEPARATORS, key[0])) {
		kb_separator(kb, key);
	} else {
		lv_textarea_add_text(ta, key);
		if (key[0] && !key[1] && isalpha((unsigned char)key[0]))
			kb_touch(kb, id, key);
		else
			k->touch_count = 0;
		k->kept = false;
	}
	kb_auto_shift(kb);
	kb_suggest(kb);
}

/* The middle row's indents are hidden keys, which LVGL does not press: the letter
 * beside the one under the finger, as far as the keyboard's edge, or none */
static uint32_t kb_indent_key(lv_obj_t *kb)
{
	const lv_buttonmatrix_t *m = (const lv_buttonmatrix_t *)kb;
	lv_area_t c;
	lv_obj_get_coords(kb, &c);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	p.x -= c.x1;
	p.y -= c.y1;
	for (uint32_t i = 0; i < m->btn_cnt; i++) {
		lv_area_t a = m->button_areas[i];
		lv_area_increase(&a, KB_GAP_PX, KB_GAP_PX / 2);
		if (lv_buttonmatrix_has_button_ctrl(kb, i, LV_BUTTONMATRIX_CTRL_HIDDEN) &&
		    p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2)
			return a.x1 < lv_obj_get_width(kb) / 2 ? i + 1 : i - 1;
	}
	return LV_BUTTONMATRIX_BUTTON_NONE;
}

/* A press in an indent presses the letter beside it, until the finger leaves it (after
 * the button matrix's handler, which let go of it) */
static void kb_indent_cb(lv_event_t *e)
{
	static bool indent; /* the press landed in an indent */
	lv_obj_t *kb = lv_event_get_target_obj(e);
	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		indent = lv_buttonmatrix_get_selected_button(kb) == LV_BUTTONMATRIX_BUTTON_NONE;
		lv_indev_get_point(lv_indev_active(), &kb_of(kb)->press);
	}
	if (!indent || lv_buttonmatrix_get_selected_button(kb) != LV_BUTTONMATRIX_BUTTON_NONE)
		return;
	uint32_t id = kb_indent_key(kb);
	indent = id != LV_BUTTONMATRIX_BUTTON_NONE;
	lv_buttonmatrix_set_selected_button(kb, id);
}

/* Sliding along the space bar moves the cursor, as on Android: a character per
 * KB_CURSOR_STEP from where the finger landed, as far as the text's ends. The space
 * key is let go of then, so the space is not typed (after kb_indent_cb, which
 * would press a letter for it) */
static struct {
	int32_t x0;   /* where the finger landed */
	uint32_t pos; /* the cursor then */
	bool on_space, moving;
} kb_slide;

static void kb_space_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target_obj(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		uint32_t id = lv_buttonmatrix_get_selected_button(kb);
		kb_slide.on_space = ta && id != LV_BUTTONMATRIX_BUTTON_NONE &&
		                    strcmp(lv_buttonmatrix_get_button_text(kb, id), " ") == 0;
		kb_slide.moving = false;
		kb_slide.x0 = p.x;
		if (ta)
			kb_slide.pos = lv_textarea_get_cursor_pos(ta);
		return;
	}
	int32_t steps = (p.x - kb_slide.x0) / KB_CURSOR_STEP;
	if (!kb_slide.on_space || (!kb_slide.moving && steps == 0))
		return;
	kb_slide.moving = true;
	lv_buttonmatrix_set_selected_button(kb, LV_BUTTONMATRIX_BUTTON_NONE);
	/* Past the end: at the end (lv_textarea_set_cursor_pos() limits it) */
	uint32_t to = (uint32_t)LV_MAX(0, (int32_t)kb_slide.pos + steps);
	if (to != lv_textarea_get_cursor_pos(ta)) {
		lv_textarea_set_cursor_pos(ta, (int32_t)to);
		kb_auto_shift(kb);
		kb_suggest(kb);
	}
}

/* While a finger is on the keyboard, the next one types the key under it as it lands
 * (fast typing: fingers overlap), elsewhere a second finger is not the pointer (pinch) */
static void kb_rollover_cb(lv_event_t *e)
{
	/* Touchscreens, with evdev's data (not the tests' emulated pointer: LV_CHECK_ARG
	 * does not check it in lv_evdev_set_rollover(), LV_USE_ASSERT_NULL is off) */
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
		if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_driver_data(indev))
			lv_evdev_set_rollover(indev, lv_event_get_code(e) == LV_EVENT_PRESSED);
}

static void kb_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

lv_obj_t *ui_keyboard_create(lv_obj_t *parent, lv_obj_t *ta)
{
	lv_obj_t *kb = lv_keyboard_create(parent);
	struct kb *k = calloc(1, sizeof(*k));
	if (!k) {
		lv_obj_delete(kb);
		return NULL;
	}
	k->apply = -1;
	lv_obj_set_user_data(kb, k);
	lv_obj_add_event_cb(kb, kb_delete_cb, LV_EVENT_DELETE, k);
	lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
	lv_obj_add_event_cb(kb, kb_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(kb, kb_indent_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_indent_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(kb, kb_space_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_space_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_DELETE, NULL); /* while pressed */
	lv_keyboard_set_textarea(kb, ta);
	lv_obj_remove_state(ta, LV_STATE_FOCUSED); /* set by lv_keyboard_set_textarea() */
	kb_set_mode(kb, KB_LOWER);

	lv_obj_set_size(kb, LV_PCT(100), KB_HEIGHT + UI_HOME_EDGE_H);
	lv_obj_set_style_pad_all(kb, KB_GAP_PX, 0);
	lv_obj_set_style_pad_bottom(kb, KB_GAP_PX + UI_HOME_EDGE_H, 0); /* the home gesture's strip */
	lv_obj_set_style_pad_gap(kb, KB_GAP_PX, 0);
	lv_obj_set_style_radius(kb, 0, 0);
	lv_obj_set_style_border_width(kb, 0, 0);
	lv_obj_set_style_shadow_width(kb, 0, 0);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BAR), 0);

	lv_obj_set_style_text_font(kb, &ui_font_32, LV_PART_ITEMS);
	lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_radius(kb, 12, LV_PART_ITEMS);
	lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(kb, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_color_filter_opa(kb, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_CARD), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BUTTON), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(kb, lv_color_hex(UI_COLOR_BUTTON_PRESSED),
	                          LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(kb, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);

	/* The strip of corrections in the top padding, hidden until shown over prose */
	k->strip = ui_box(kb);
	lv_obj_set_size(k->strip, LV_PCT(100), KB_STRIP_H - KB_GAP_PX);
	lv_obj_set_pos(k->strip, 0, -KB_STRIP_H);
	lv_obj_set_flex_flow(k->strip, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(k->strip, KB_GAP_PX, 0);
	for (int i = 0; i < CORRECT_MAX; i++) {
		lv_obj_t *slot = k->slot[i] = ui_box(k->strip);
		lv_obj_set_height(slot, LV_PCT(100));
		lv_obj_set_flex_grow(slot, 1);
		lv_obj_set_clickable(slot, true);
		kb_unfocusable(slot);
		lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_radius(slot, 12, 0);
		lv_obj_set_style_bg_color(slot, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_add_event_cb(slot, kb_slot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_t *label = lv_label_create(slot);
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_max_width(label, LV_PCT(100), 0);
		lv_obj_set_style_text_font(label, UI_FONT_TITLE, 0);
		lv_obj_set_hidden(slot, true);
	}
	lv_obj_set_hidden(k->strip, true);

	lv_obj_set_hidden(kb, true);
	return kb;
}

void ui_textarea_set_prose(lv_obj_t *ta)
{
	lv_obj_set_state_user_1(ta, true);
}

void ui_textarea_set_enter_sends(lv_obj_t *ta)
{
	lv_obj_set_state_user_2(ta, true);
}

void ui_textarea_set_phone(lv_obj_t *ta)
{
	lv_textarea_set_accepted_chars_static(ta, kb_phone_chars);
}

void ui_keyboard_show(lv_obj_t *kb, bool show)
{
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	lv_obj_set_hidden(kb, !show);
	lv_obj_set_state(ta, LV_STATE_FOCUSED, show);
	if (show) {
		struct kb *k = kb_of(kb);
		kb_set_mode(kb, lv_textarea_get_accepted_chars(ta) == kb_phone_chars ? KB_PHONE : KB_LOWER);
		kb_auto_shift(kb);
		/* The strip over prose, in a language with a dictionary, unless turned off */
		/* Every language's dictionary, the UI's first */
		k->dict_count = 0;
		k->dicts[k->dict_count++] = correct_dict(ui_language()->code);
		for (int i = 0; i < correct_dict_count && k->dict_count < KB_DICTS; i++)
			if (&correct_dicts[i] != k->dicts[0])
				k->dicts[k->dict_count++] = &correct_dicts[i];
		if (!k->dicts[0])
			k->dicts[0] = k->dicts[--k->dict_count];
		k->english = correct_dict("en") != NULL;
		bool strip = k->dict_count && k->mode != KB_PHONE && lv_obj_is_state_user_1(ta) &&
		             !lv_textarea_get_password_mode(ta) && ui_setting_get_int("autocorrect", 1);
		lv_obj_set_hidden(k->strip, !strip);
		lv_obj_set_height(kb, KB_HEIGHT + UI_HOME_EDGE_H + (strip ? KB_STRIP_H : 0));
		lv_obj_set_style_pad_top(kb, strip ? KB_GAP_PX + KB_STRIP_H : KB_GAP_PX, 0);
		k->touch_count = 0;
		k->kept = false;
		k->undo = false;
		kb_suggest(kb);
	}
}
