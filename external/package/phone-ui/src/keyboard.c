/*
 * Keyboard: LVGL's keyboard widget with a phone layout and its own key handler
 * (LVGL's only knows its own mode keys). Keys type on release, except backspace,
 * which repeats while held and deletes whole emoji. Shift lasts one character,
 * twice quickly locks it; sentences start in upper case. The emoji key opens a
 * panel of emoji over the keys (below).
 */

#include <stdlib.h>
#include <string.h>

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
#define KB_HEIGHT 440
#define KB_GAP_PX 10               /* around and between the keys */
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

static void kb_set_mode(lv_obj_t *kb, enum kb_mode mode)
{
	static const char *const *const maps[] = { kb_lower, kb_upper, kb_lock, kb_symbols, kb_more, kb_phone };
	static const lv_buttonmatrix_ctrl_t *const ctrls[] = {
		kb_letters_ctrl[0], kb_letters_ctrl[1], kb_letters_ctrl[1], kb_symbols_ctrl, kb_more_ctrl,
		kb_phone_ctrl,
	};
	lv_obj_set_user_data(kb, (void *)(uintptr_t)mode);
	lv_buttonmatrix_set_map(kb, maps[mode]);
	lv_buttonmatrix_set_ctrl_map(kb, ctrls[mode]);
}

static enum kb_mode kb_get_mode(lv_obj_t *kb)
{
	return (enum kb_mode)(uintptr_t)lv_obj_get_user_data(kb);
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

/* Keys as the keyboard's: transparent ones in the grid */
static void emoji_keys_style(lv_obj_t *bm, bool keys)
{
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
	lv_obj_t *panel = lv_obj_get_child(kb, 0);
	struct kb_emoji_panel *p;
	if (panel) {
		p = lv_obj_get_user_data(panel);
	} else {
		p = calloc(1, sizeof(*p));
		if (!p)
			return;
		p->kb = kb;
		panel = ui_box(kb);
		lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
		lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_BAR), 0);
		/* Presses on it do not reach the keys below */
		lv_obj_set_clickable(panel, true);
		lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(panel, KB_GAP_PX, 0);
		lv_obj_set_user_data(panel, p);
		lv_obj_add_event_cb(panel, emoji_panel_delete_cb, LV_EVENT_DELETE, p);

		p->scroll = lv_obj_create(panel);
		lv_obj_remove_style_all(p->scroll);
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

static void kb_key_cb(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target_obj(e);
	lv_obj_t *ta = lv_keyboard_get_textarea(kb);
	uint32_t id = lv_buttonmatrix_get_selected_button(kb);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE || !ta)
		return;
	const char *key = lv_buttonmatrix_get_button_text(kb, id);
	ui_vibrate(UI_VIBRATE_TAP);

	if (strcmp(key, KB_SHIFT) == 0) {
		if (kb_get_mode(kb) == KB_LOWER) {
			kb_set_mode(kb, KB_UPPER);
			kb_shift_time = lv_tick_get();
		} else {
			kb_set_mode(kb, lv_tick_elaps(kb_shift_time) < KB_DOUBLE_MS ? KB_LOCK : KB_LOWER);
		}
	} else if (strcmp(key, KB_LOCKED) == 0 || strcmp(key, KB_TO_LETTERS) == 0) {
		kb_set_mode(kb, KB_LOWER);
		kb_auto_shift(kb);
	} else if (strcmp(key, KB_TO_SYMBOLS) == 0) {
		kb_set_mode(kb, KB_SYMBOLS);
	} else if (strcmp(key, KB_TO_MORE) == 0) {
		kb_set_mode(kb, KB_MORE);
	} else if (strcmp(key, KB_TO_EMOJI) == 0) {
		emoji_panel_open(kb);
	} else if (strcmp(key, LV_SYMBOL_KEYBOARD) == 0) {
		lv_obj_send_event(ta, LV_EVENT_CANCEL, NULL);
	} else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0 && lv_textarea_get_one_line(ta)) {
		lv_obj_send_event(ta, LV_EVENT_READY, NULL);
	} else {
		if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0)
			kb_delete(ta);
		else if (strcmp(key, LV_SYMBOL_NEW_LINE) == 0)
			lv_textarea_add_char(ta, '\n');
		else
			lv_textarea_add_text(ta, key);
		kb_auto_shift(kb);
	}
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
	if (lv_event_get_code(e) == LV_EVENT_PRESSED)
		indent = true;
	if (!indent || lv_buttonmatrix_get_selected_button(kb) != LV_BUTTONMATRIX_BUTTON_NONE)
		return;
	uint32_t id = kb_indent_key(kb);
	indent = id != LV_BUTTONMATRIX_BUTTON_NONE;
	lv_buttonmatrix_set_selected_button(kb, id);
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

lv_obj_t *ui_keyboard_create(lv_obj_t *parent, lv_obj_t *ta)
{
	lv_obj_t *kb = lv_keyboard_create(parent);
	lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
	lv_obj_add_event_cb(kb, kb_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(kb, kb_indent_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_indent_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(kb, kb_rollover_cb, LV_EVENT_DELETE, NULL); /* while pressed */
	lv_keyboard_set_textarea(kb, ta);
	lv_obj_remove_state(ta, LV_STATE_FOCUSED); /* set by lv_keyboard_set_textarea() */
	kb_set_mode(kb, KB_LOWER);

	lv_obj_set_size(kb, LV_PCT(100), KB_HEIGHT);
	lv_obj_set_style_pad_all(kb, KB_GAP_PX, 0);
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

	lv_obj_set_hidden(kb, true);
	return kb;
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
		kb_set_mode(kb, lv_textarea_get_accepted_chars(ta) == kb_phone_chars ? KB_PHONE : KB_LOWER);
		kb_auto_shift(kb);
	}
}
