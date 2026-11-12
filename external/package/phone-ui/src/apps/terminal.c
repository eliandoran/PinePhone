/*
 * Terminal: the shell (root's, BusyBox ash, as a login shell) on a pseudo-terminal,
 * built in (it was a plugin app) so that every phone has it for debugging.
 *
 * The screen is a grid of character cells as an xterm shows it (TERM=xterm): the
 * cursor movements, erasing, scroll regions, the alternate screen and the line
 * drawing characters that BusyBox's programs and ncurses use. Each row is a label in a
 * monospace font (phone-ui's ui_font_mono_20, 12 x 25 px: 59 columns). Rows that scroll
 * off the top of the grid stay above it as the history (HISTORY_MAX lines), which the
 * view scrolls through; output and typing scroll back to the bottom.
 *
 * Colors and attributes (SGR): the 16 colors (a palette per theme, bold ones bright),
 * 256 and 24-bit ones, bold, dim, underline, strikethrough, reverse, hidden; italic and
 * blinking are not drawn. A row with any keeps them as runs in its label's user data
 * (struct style), the history's too, and draws its text itself (line_draw_cb()), the
 * label's own text transparent: its text stays what the tests and the history read.
 * Erasing fills with the background color, as xterm does ("bce").
 *
 * Typing goes through phone-ui's keyboard, on a hidden text area that keeps one
 * letter (so that it does not capitalize): what it would insert goes to the shell
 * instead. A row of keys above it has what the keyboard lacks: Esc, Tab, Ctrl (for
 * the next key, e.g. Ctrl then c) and the arrows, and |, /, - at hand. Tapping the
 * terminal shows the keyboard, its hide key hides it; the grid's rows follow its
 * height (SIGWINCH), like a resized xterm: the history gives rows back.
 *
 * Leaving the app ends the shell (SIGHUP, then SIGKILL), and the app closes when
 * the shell exits.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "i18n.h"
#include "ui.h"

#define HISTORY_MAX 1000
#define MAX_ROWS 128
#define MAX_PARAMS 16
#define TICK_MS 16
#define READ_MAX 262144 /* per tick, so that a flood of output still lets it draw */
#define KILL_MS 2000    /* after SIGHUP */
#define KEYS_H 88
#define FIRST 1         /* the view's first label: the cursor comes before them */

enum state { GROUND, ESC, CSI, CHARSET, STRING, SKIP };

/* A color: none (the default), one of the 256, or 24-bit */
#define INDEXED(i) (1u << 24 | (uint32_t)(i))
#define RGB(rgb) (2u << 24 | (uint32_t)(rgb))

enum { BOLD = 1, DIM = 2, UNDERLINE = 4, STRIKE = 8, REVERSE = 16, HIDDEN = 32 };

/* What a character is drawn with */
struct pen {
	uint32_t fg, bg;
	uint8_t flags;
};

struct cell {
	uint32_t ch;
	struct pen pen;
};

/* A styled row's runs of cells with the same pen, the whole row (its label's text) */
struct run {
	uint16_t x, n;      /* cells */
	uint16_t off, len;  /* bytes of the label's text */
	struct pen pen;
};

struct style {
	int n;
	struct run run[];
};

static struct {
	lv_obj_t *scr, *view, *cursor, *ta, *kb, *keys;
	lv_timer_t *timer;
	int fd;
	pid_t pid;

	int rows, cols, cw, lh;
	struct cell *cells, *other; /* the grid; the other one (main or alternate) */
	char *text;               /* a row's UTF-8, line_render()'s: 4 bytes a cell */
	lv_obj_t *lines[MAX_ROWS]; /* the rows' labels, after the history's */
	bool dirty[MAX_ROWS];
	int history;
	bool resized, follow;

	int x, y, saved_x, saved_y;
	bool wrap; /* the last column written: the next character goes on the next line */
	int top, bottom; /* scroll region */
	bool alt, app_cursor, autowrap, cursor_on;
	bool graphics[2]; /* G0, G1: DEC line drawing */
	int charset, charset_set;
	uint32_t last;
	struct pen pen, saved_pen;

	enum state state;
	int params[MAX_PARAMS], nparams;
	char private, inter;
	uint32_t utf8;
	int utf8_left;
} term;

/* DEC special graphics for 0x5f to 0x7e: ESC ( 0 */
static const uint16_t dec_graphics[32] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

/* Grid */

static struct cell *row(int y)
{
	return term.cells + (size_t)y * term.cols;
}

/* Spaces in the pen's background color (xterm's "bce") */
static void blank(struct cell *c, int n)
{
	for (int i = 0; i < n; i++)
		c[i] = (struct cell){ ' ', { 0, term.pen.bg, 0 } };
}

/* What shows of a cell besides its character: a background, a line, reversed */
static bool pen_shows(const struct pen *p)
{
	return p->bg || p->flags & (UNDERLINE | STRIKE | REVERSE);
}

/* Colors */

/* The 16 colors, readable on each theme's background */
static const uint32_t dark_palette[16] = {
	0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
	0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};
static const uint32_t light_palette[16] = {
	0x000000, 0xcd3131, 0x00bc00, 0x949800, 0x0451a5, 0xbc05bc, 0x0598bc, 0x555555,
	0x666666, 0xcd3131, 0x14ce14, 0xb5ba00, 0x0451a5, 0xbc05bc, 0x0598bc, 0xa5a5a5,
};

static uint32_t background(void)
{
	return ui_light ? 0xffffff : 0x000000;
}

/* One of the 256: the 16, a 6 x 6 x 6 cube, 24 grays */
static uint32_t indexed(uint32_t i)
{
	if (i < 16)
		return (ui_light ? light_palette : dark_palette)[i];
	if (i < 232) {
		static const uint8_t level[6] = { 0, 95, 135, 175, 215, 255 };
		i -= 16;
		return (uint32_t)level[i / 36] << 16 | (uint32_t)level[i / 6 % 6] << 8 | level[i % 6];
	}
	uint32_t g = 8 + 10 * (i - 232);
	return g << 16 | g << 8 | g;
}

/* A color as RGB, `fallback` for the default */
static uint32_t color(uint32_t c, uint32_t fallback)
{
	return !c ? fallback : c >> 24 == 1 ? indexed(c & 0xff) : c & 0xffffff;
}

static void mark(int a, int b)
{
	for (int y = a; y <= b; y++)
		term.dirty[y] = true;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static bool pen_equal(const struct pen *a, const struct pen *b)
{
	return a->fg == b->fg && a->bg == b->bg && a->flags == b->flags;
}

/* A styled row: its runs' backgrounds, the cursor over them, their text */
static void line_draw_cb(lv_event_t *e)
{
	lv_obj_t *label = lv_event_get_current_target(e);
	const struct style *s = lv_obj_get_user_data(label);
	if (!s)
		return;
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	const char *text = lv_label_get_text(label);
	bool cursor = !lv_obj_is_hidden(term.cursor) && term.y < term.rows && term.lines[term.y] == label;
	for (int i = 0; i < s->n; i++) {
		const struct run *r = &s->run[i];
		uint32_t fg = r->pen.fg;
		if (r->pen.flags & BOLD && fg >> 24 == 1 && (fg & 0xff) < 8)
			fg += 8; /* bold: the bright color, as xterm */
		uint32_t fg_rgb = color(fg, UI_COLOR_TEXT), bg_rgb = color(r->pen.bg, background());
		bool filled = r->pen.bg;
		if (r->pen.flags & REVERSE) {
			uint32_t t = fg_rgb;
			fg_rgb = bg_rgb;
			bg_rgb = t;
			filled = true;
		}
		lv_area_t cells = { a.x1 + r->x * term.cw, a.y1, a.x1 + (r->x + r->n) * term.cw - 1, a.y1 + term.lh - 1 };
		if (filled) {
			lv_draw_rect_dsc_t rect;
			lv_draw_rect_dsc_init(&rect);
			rect.bg_color = lv_color_hex(bg_rgb);
			lv_draw_rect(layer, &rect, &cells);
			if (cursor && term.x >= r->x && term.x < r->x + r->n) { /* drawn under the row: covered */
				lv_area_t c = { a.x1 + term.x * term.cw, a.y1, a.x1 + (term.x + 1) * term.cw - 1, cells.y2 };
				rect.bg_color = lv_color_hex(UI_COLOR_ACCENT);
				lv_draw_rect(layer, &rect, &c);
			}
		}
		if (r->pen.flags & HIDDEN || !r->len)
			continue;
		char buf[512];
		size_t len = LV_MIN(r->len, sizeof(buf) - 1);
		memcpy(buf, text + r->off, len);
		buf[len] = '\0';
		lv_draw_label_dsc_t d;
		lv_draw_label_dsc_init(&d);
		d.text = buf;
		d.text_local = 1;
		d.font = &ui_font_mono_20;
		d.color = lv_color_hex(fg_rgb);
		d.opa = r->pen.flags & DIM ? LV_OPA_60 : LV_OPA_COVER;
		d.flag = LV_TEXT_FLAG_EXPAND;
		d.decor = (r->pen.flags & UNDERLINE ? LV_TEXT_DECOR_UNDERLINE : 0) |
		          (r->pen.flags & STRIKE ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
		cells.x2 = a.x2;
		lv_draw_label(layer, &d, &cells);
	}
}

static void line_delete_cb(lv_event_t *e)
{
	free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

/* A row's runs (NULL: none, the label draws it), taken */
static void line_style_set(lv_obj_t *label, struct style *s)
{
	struct style *old = lv_obj_get_user_data(label);
	bool same = !old == !s;
	for (int i = 0; same && s && i < s->n; i++)
		same = i < old->n && old->n == s->n && old->run[i].x == s->run[i].x && old->run[i].n == s->run[i].n &&
		       old->run[i].off == s->run[i].off && old->run[i].len == s->run[i].len &&
		       pen_equal(&old->run[i].pen, &s->run[i].pen);
	if (same) {
		free(s);
		return;
	}
	free(old);
	lv_obj_set_user_data(label, s);
	if (s)
		lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
	else
		lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_OPA, 0);
	lv_obj_invalidate(label);
}

static lv_obj_t *line_create(void)
{
	lv_obj_t *label = lv_label_create(term.view);
	lv_label_set_text_static(label, "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_add_event_cb(label, line_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
	lv_obj_add_event_cb(label, line_delete_cb, LV_EVENT_DELETE, NULL);
	return label;
}

static size_t utf8_put(char *out, uint32_t c)
{
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | c >> 6);
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | c >> 12);
		out[1] = (char)(0x80 | (c >> 6 & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | c >> 18);
	out[1] = (char)(0x80 | (c >> 12 & 0x3f));
	out[2] = (char)(0x80 | (c >> 6 & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

/* A row's cells into its label, without the spaces at the end (unless something of them
 * shows), with its runs when any cell has a pen */
static void line_render(lv_obj_t *label, const struct cell *c)
{
	static const struct pen plain;
	char *text = term.text;
	size_t n = 0, end = 0;
	int cells = 0, runs = 0;
	for (int x = 0; x < term.cols; x++) {
		n += utf8_put(text + n, c[x].ch);
		if (c[x].ch != ' ' || pen_shows(&c[x].pen)) {
			end = n;
			cells = x + 1;
		}
	}
	text[end] = '\0';
	if (strcmp(lv_label_get_text(label), text) != 0)
		lv_label_set_text(label, text);

	bool styled = false;
	for (int x = 0; x < cells; x++) {
		styled |= !pen_equal(&c[x].pen, &plain);
		runs += !x || !pen_equal(&c[x].pen, &c[x - 1].pen);
	}
	struct style *s = styled ? malloc(sizeof(*s) + (size_t)runs * sizeof(*s->run)) : NULL;
	if (s) {
		char utf8[4];
		size_t off = 0;
		s->n = 0;
		for (int x = 0; x < cells; x++) {
			if (!x || !pen_equal(&c[x].pen, &c[x - 1].pen))
				s->run[s->n++] = (struct run){ (uint16_t)x, 0, (uint16_t)off, 0, c[x].pen };
			size_t len = utf8_put(utf8, c[x].ch);
			s->run[s->n - 1].n++;
			s->run[s->n - 1].len = (uint16_t)(s->run[s->n - 1].len + len);
			off += len;
		}
	}
	line_style_set(label, s);
}

/* A label's text and runs back into cells (a history line returning to the grid) */
static void line_parse(struct cell *c, lv_obj_t *label)
{
	const unsigned char *p = (const unsigned char *)lv_label_get_text(label);
	for (int x = 0; x < term.cols; x++)
		c[x] = (struct cell){ ' ', { 0 } };
	for (int x = 0; *p && x < term.cols; x++) {
		uint32_t v = *p++;
		int more = v >= 0xf0 ? 3 : v >= 0xe0 ? 2 : v >= 0xc0 ? 1 : 0;
		v &= more ? 0x3f >> more : 0x7f;
		for (; more && (*p & 0xc0) == 0x80; more--)
			v = v << 6 | (*p++ & 0x3f);
		c[x].ch = v;
	}
	const struct style *s = lv_obj_get_user_data(label);
	for (int i = 0; s && i < s->n; i++)
		for (int x = s->run[i].x; x < s->run[i].x + s->run[i].n && x < term.cols; x++)
			c[x].pen = s->run[i].pen;
}

/* A blank row at `at` in both grids, its label set by the caller */
static void rows_insert(int at)
{
	size_t len = (size_t)(term.rows - at) * term.cols * sizeof(struct cell);
	memmove(row(at + 1), row(at), len);
	memmove(term.other + (size_t)(at + 1) * term.cols, term.other + (size_t)at * term.cols, len);
	blank(row(at), term.cols);
	blank(term.other + (size_t)at * term.cols, term.cols);
	memmove(term.lines + at + 1, term.lines + at, (size_t)(term.rows - at) * sizeof(*term.lines));
	memmove(term.dirty + at + 1, term.dirty + at, (size_t)(term.rows - at) * sizeof(*term.dirty));
	term.dirty[at] = true;
	term.rows++;
}

/* Row `at` out of both grids, its label left to the caller */
static void rows_remove(int at)
{
	size_t len = (size_t)(term.rows - at - 1) * term.cols * sizeof(struct cell);
	memmove(row(at), row(at + 1), len);
	memmove(term.other + (size_t)at * term.cols, term.other + (size_t)(at + 1) * term.cols, len);
	memmove(term.lines + at, term.lines + at + 1, (size_t)(term.rows - at - 1) * sizeof(*term.lines));
	memmove(term.dirty + at, term.dirty + at + 1, (size_t)(term.rows - at - 1) * sizeof(*term.dirty));
	term.rows--;
}

/* The top row into the history (its label stays where it is), the oldest line out */
static void history_add(void)
{
	line_render(term.lines[0], row(0));
	rows_remove(0);
	if (++term.history > HISTORY_MAX) {
		lv_obj_delete(lv_obj_get_child(term.view, FIRST));
		term.history--;
	}
}

static void history_clear(void)
{
	for (; term.history; term.history--)
		lv_obj_delete(lv_obj_get_child(term.view, FIRST));
}

/* Scrolling up from the top of the screen puts the line in the history */
static void scroll_up(int a, int b, int n, bool keep)
{
	n = clamp(n, 0, b - a + 1);
	if (keep && a == 0 && !term.alt) {
		while (n--) {
			lv_obj_t *label;
			if (term.history == HISTORY_MAX) { /* the oldest line's label, reused */
				label = lv_obj_get_child(term.view, FIRST);
				lv_label_set_text_static(label, "");
				line_style_set(label, NULL);
				term.history--;
			} else {
				label = line_create();
			}
			history_add();
			lv_obj_move_to_index(label, FIRST + term.history + b);
			rows_insert(b);
			term.lines[b] = label;
		}
		return;
	}
	memmove(row(a), row(a + n), (size_t)(b - a + 1 - n) * term.cols * sizeof(struct cell));
	blank(row(b - n + 1), n * term.cols);
	mark(a, b);
}

static void scroll_down(int a, int b, int n)
{
	n = clamp(n, 0, b - a + 1);
	memmove(row(a + n), row(a), (size_t)(b - a + 1 - n) * term.cols * sizeof(struct cell));
	blank(row(a), n * term.cols);
	mark(a, b);
}

static void line_feed(void)
{
	term.wrap = false;
	if (term.y == term.bottom)
		scroll_up(term.top, term.bottom, 1, true);
	else if (term.y < term.rows - 1)
		term.y++;
}

static void reverse_line_feed(void)
{
	term.wrap = false;
	if (term.y == term.top)
		scroll_down(term.top, term.bottom, 1);
	else if (term.y > 0)
		term.y--;
}

static void move_to(int x, int y)
{
	term.x = clamp(x, 0, term.cols - 1);
	term.y = clamp(y, 0, term.rows - 1);
	term.wrap = false;
}

static void put(uint32_t c)
{
	if (term.graphics[term.charset] && c >= 0x5f && c <= 0x7e)
		c = dec_graphics[c - 0x5f];
	if (term.wrap) {
		term.x = 0;
		line_feed();
	}
	row(term.y)[term.x] = (struct cell){ c, term.pen };
	term.dirty[term.y] = true;
	term.last = c;
	if (term.x < term.cols - 1)
		term.x++;
	else
		term.wrap = term.autowrap;
}

static void erase_all(void)
{
	blank(term.cells, term.rows * term.cols);
	mark(0, term.rows - 1);
}

static void save_cursor(void)
{
	term.saved_x = term.x;
	term.saved_pen = term.pen;
	term.saved_y = term.y;
}

static void restore_cursor(void)
{
	move_to(term.saved_x, term.saved_y);
	term.pen = term.saved_pen;
}

static void alt_screen(bool on, bool cursor)
{
	if (on == term.alt)
		return;
	if (on && cursor)
		save_cursor();
	struct cell *cells = term.cells;
	term.cells = term.other;
	term.other = cells;
	term.alt = on;
	if (on)
		erase_all();
	mark(0, term.rows - 1);
	if (!on && cursor)
		restore_cursor();
}

static void reset(void)
{
	term.pen = (struct pen){ 0 };
	alt_screen(false, false);
	erase_all();
	move_to(0, 0);
	save_cursor();
	term.top = 0;
	term.bottom = term.rows - 1;
	term.autowrap = term.cursor_on = true;
	term.app_cursor = false;
	term.graphics[0] = term.graphics[1] = false;
	term.charset = 0;
}

/* Grid rows: fewer drop blank ones below the cursor, then send the top ones into the
 * history; more take them back from it (as xterm does), then add blank ones */
static void resize(int rows)
{
	rows = clamp(rows, 1, MAX_ROWS);
	if (rows == term.rows)
		return;
	while (term.rows > rows && term.y < term.rows - 1) {
		const struct cell *c = row(term.rows - 1);
		int x = 0;
		while (x < term.cols && c[x].ch == ' ' && !pen_shows(&c[x].pen))
			x++;
		if (x < term.cols)
			break;
		lv_obj_delete(term.lines[term.rows - 1]);
		rows_remove(term.rows - 1);
	}
	while (!term.alt && term.rows > rows) {
		history_add();
		term.y--;
		term.saved_y--;
	}
	while (!term.alt && term.rows < rows && term.history) {
		lv_obj_t *label = lv_obj_get_child(term.view, FIRST + --term.history);
		rows_insert(0);
		term.lines[0] = label;
		line_parse(row(0), label);
		term.dirty[0] = false;
		term.y++;
		term.saved_y++;
	}
	while (term.rows > rows) {
		lv_obj_delete(term.lines[term.rows - 1]);
		rows_remove(term.rows - 1);
	}
	while (term.rows < rows) {
		rows_insert(term.rows);
		term.lines[term.rows - 1] = line_create();
	}
	term.top = 0;
	term.bottom = rows - 1;
	term.x = clamp(term.x, 0, term.cols - 1);
	term.y = clamp(term.y, 0, rows - 1);
	term.saved_y = clamp(term.saved_y, 0, rows - 1);

	struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)term.cols };
	if (term.fd >= 0 && ioctl(term.fd, TIOCSWINSZ, &ws) < 0)
		perror("terminal: TIOCSWINSZ");
}

/* Escape sequences */

static void reply(const char *s)
{
	if (term.fd >= 0 && write(term.fd, s, strlen(s)) < 0)
		perror("terminal: write");
}

static int arg(int i, int fallback)
{
	return i < term.nparams && term.params[i] ? term.params[i] : fallback;
}

static void set_modes(bool on)
{
	for (int i = 0; i < (term.nparams ? term.nparams : 1); i++) {
		if (term.private != '?')
			continue; /* insert mode, newline mode: not done */
		switch (term.params[i]) {
		case 1: term.app_cursor = on; break;
		case 7: term.autowrap = on; break;
		case 25: term.cursor_on = on; break;
		case 47:
		case 1047: alt_screen(on, false); break;
		case 1049: alt_screen(on, true); break;
		}
	}
}

/* 38 or 48's color at params[*i]: 5;n (of the 256) or 2;r;g;b, the parameters taken; 0 if
 * neither */
static uint32_t extended_color(int *i)
{
	int n = term.nparams;
	if (*i + 2 < n && term.params[*i + 1] == 5) {
		*i += 2;
		return INDEXED(term.params[*i] & 0xff);
	}
	if (*i + 4 < n && term.params[*i + 1] == 2) {
		uint32_t rgb = (uint32_t)(term.params[*i + 2] & 0xff) << 16 |
		               (uint32_t)(term.params[*i + 3] & 0xff) << 8 | (uint32_t)(term.params[*i + 4] & 0xff);
		*i += 4;
		return RGB(rgb);
	}
	*i = n;
	return 0;
}

/* Select graphic rendition: the pen of what follows */
static void sgr(void)
{
	struct pen *p = &term.pen;
	for (int i = 0; i < (term.nparams ? term.nparams : 1); i++) {
		int v = term.params[i];
		if (v >= 30 && v <= 37)
			p->fg = INDEXED(v - 30);
		else if (v >= 40 && v <= 47)
			p->bg = INDEXED(v - 40);
		else if (v >= 90 && v <= 97)
			p->fg = INDEXED(v - 90 + 8);
		else if (v >= 100 && v <= 107)
			p->bg = INDEXED(v - 100 + 8);
		else switch (v) {
		case 0: *p = (struct pen){ 0 }; break;
		case 1: p->flags |= BOLD; break;
		case 2: p->flags |= DIM; break;
		case 4:
		case 21: p->flags |= UNDERLINE; break;
		case 7: p->flags |= REVERSE; break;
		case 8: p->flags |= HIDDEN; break;
		case 9: p->flags |= STRIKE; break;
		case 22: p->flags &= (uint8_t)~(BOLD | DIM); break;
		case 24: p->flags &= (uint8_t)~UNDERLINE; break;
		case 27: p->flags &= (uint8_t)~REVERSE; break;
		case 28: p->flags &= (uint8_t)~HIDDEN; break;
		case 29: p->flags &= (uint8_t)~STRIKE; break;
		case 38: p->fg = extended_color(&i); break;
		case 39: p->fg = 0; break;
		case 48: p->bg = extended_color(&i); break;
		case 49: p->bg = 0; break;
		}
		/* 3, 23 italic, 5, 25 blinking: not drawn */
	}
}

static void erase_display(int mode)
{
	switch (mode) {
	case 0:
		blank(row(term.y) + term.x, term.cols - term.x);
		blank(row(term.y + 1), (term.rows - term.y - 1) * term.cols);
		mark(term.y, term.rows - 1);
		break;
	case 1:
		blank(term.cells, term.y * term.cols + term.x + 1);
		mark(0, term.y);
		break;
	case 2:
		erase_all();
		break;
	case 3:
		history_clear();
		break;
	}
}

static void erase_line(int mode)
{
	struct cell *c = row(term.y);
	if (mode == 0)
		blank(c + term.x, term.cols - term.x);
	else if (mode == 1)
		blank(c, term.x + 1);
	else
		blank(c, term.cols);
	term.dirty[term.y] = true;
}

static void csi(char final)
{
	struct cell *c = row(term.y);
	int n = arg(0, 1), room = term.cols - term.x;
	if (term.inter)
		return;
	if (term.private == '>' && final == 'c') {
		reply("\033[>0;0;0c");
		return;
	}
	if (term.private && term.private != '?')
		return;

	switch (final) {
	case '@': /* insert blanks */
		n = clamp(n, 0, room);
		memmove(c + term.x + n, c + term.x, (size_t)(room - n) * sizeof(*c));
		blank(c + term.x, n);
		term.dirty[term.y] = true;
		break;
	case 'P': /* delete characters */
		n = clamp(n, 0, room);
		memmove(c + term.x, c + term.x + n, (size_t)(room - n) * sizeof(*c));
		blank(c + term.cols - n, n);
		term.dirty[term.y] = true;
		break;
	case 'X': /* erase characters */
		blank(c + term.x, clamp(n, 0, room));
		term.dirty[term.y] = true;
		break;
	case 'A':
		move_to(term.x, term.y >= term.top ? (term.y - n < term.top ? term.top : term.y - n) : term.y - n);
		break;
	case 'B':
	case 'e':
		move_to(term.x, term.y <= term.bottom ? (term.y + n > term.bottom ? term.bottom : term.y + n) : term.y + n);
		break;
	case 'C':
	case 'a':
		move_to(term.x + n, term.y);
		break;
	case 'D':
		move_to(term.x - n, term.y);
		break;
	case 'E':
		move_to(0, term.y + n);
		break;
	case 'F':
		move_to(0, term.y - n);
		break;
	case 'G':
	case '`':
		move_to(n - 1, term.y);
		break;
	case 'd':
		move_to(term.x, n - 1);
		break;
	case 'H':
	case 'f':
		move_to(arg(1, 1) - 1, n - 1);
		break;
	case 'J':
		erase_display(arg(0, 0));
		term.wrap = false;
		break;
	case 'K':
		erase_line(arg(0, 0));
		term.wrap = false;
		break;
	case 'L':
		if (term.y >= term.top && term.y <= term.bottom) {
			scroll_down(term.y, term.bottom, n);
			move_to(0, term.y);
		}
		break;
	case 'M':
		if (term.y >= term.top && term.y <= term.bottom) {
			scroll_up(term.y, term.bottom, n, false);
			move_to(0, term.y);
		}
		break;
	case 'S':
		scroll_up(term.top, term.bottom, n, false);
		break;
	case 'T':
		scroll_down(term.top, term.bottom, n);
		break;
	case 'b': /* repeat the last character */
		for (n = clamp(n, 0, term.cols * term.rows); n; n--)
			put(term.last);
		break;
	case 'r': {
		int top = arg(0, 1) - 1, bottom = arg(1, term.rows) - 1;
		if (top < bottom && bottom < term.rows) {
			term.top = top;
			term.bottom = bottom;
			move_to(0, 0);
		}
		break;
	}
	case 's':
		save_cursor();
		break;
	case 'u':
		restore_cursor();
		break;
	case 'm':
		if (!term.private)
			sgr();
		break;
	case 'h':
		set_modes(true);
		break;
	case 'l':
		set_modes(false);
		break;
	case 'n':
		if (arg(0, 0) == 6) {
			char buf[32];
			snprintf(buf, sizeof(buf), "\033[%d;%dR", term.y + 1, term.x + 1);
			reply(buf);
		} else if (arg(0, 0) == 5) {
			reply("\033[0n");
		}
		break;
	case 'c':
		if (!term.private)
			reply("\033[?1;2c");
		break;
	}
}

static void esc(unsigned char c)
{
	term.state = GROUND;
	switch (c) {
	case '[':
		term.state = CSI;
		memset(term.params, 0, sizeof(term.params));
		term.nparams = 0;
		term.private = term.inter = 0;
		break;
	case ']': /* strings: OSC (window title), DCS, APC, PM */
	case 'P':
	case '_':
	case '^':
		term.state = STRING;
		break;
	case '(':
	case ')':
		term.charset_set = c == ')';
		term.state = CHARSET;
		break;
	case '*': /* G2, G3: not used */
	case '+':
	case '#':
	case '%':
	case ' ':
		term.state = SKIP;
		break;
	case '7':
		save_cursor();
		break;
	case '8':
		restore_cursor();
		break;
	case 'D':
		line_feed();
		break;
	case 'E':
		term.x = 0;
		line_feed();
		break;
	case 'M':
		reverse_line_feed();
		break;
	case 'c':
		reset();
		history_clear();
		break;
	}
}

static void control(unsigned char c)
{
	switch (c) {
	case 0x07:
		ui_vibrate(UI_VIBRATE_TAP);
		break;
	case '\b':
		move_to(term.x - 1, term.y);
		break;
	case '\t':
		move_to((term.x / 8 + 1) * 8, term.y);
		break;
	case '\n':
	case '\v':
	case '\f':
		line_feed();
		break;
	case '\r':
		move_to(0, term.y);
		break;
	case 0x0e:
		term.charset = 1;
		break;
	case 0x0f:
		term.charset = 0;
		break;
	case 0x18:
	case 0x1a:
		term.state = GROUND;
		break;
	case 0x1b:
		term.state = ESC;
		break;
	}
}

static void parse(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];
		if (term.state == STRING) { /* until BEL or ESC \ */
			if (c == 0x07)
				term.state = GROUND;
			else if (c == 0x1b)
				term.state = ESC;
			continue;
		}
		if (c < 0x20 || c == 0x7f) {
			term.utf8_left = 0;
			control(c);
			continue;
		}
		switch (term.state) {
		case GROUND:
			if (c < 0x80) {
				term.utf8_left = 0;
				put(c);
			} else if ((c & 0xc0) == 0x80) {
				if (term.utf8_left) {
					term.utf8 = term.utf8 << 6 | (c & 0x3f);
					if (--term.utf8_left == 0)
						put(term.utf8);
				}
			} else if (c >= 0xc2 && c <= 0xf4) {
				term.utf8_left = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
				term.utf8 = c & (0x3f >> term.utf8_left);
			} else {
				term.utf8_left = 0;
				put(0xfffd);
			}
			break;
		case ESC:
			esc(c);
			break;
		case CSI:
			if (c >= '0' && c <= '9') {
				if (!term.nparams)
					term.nparams = 1;
				int *p = &term.params[term.nparams - 1];
				if (*p < 10000)
					*p = *p * 10 + (c - '0');
			} else if (c == ';' || c == ':') {
				if (!term.nparams)
					term.nparams = 1;
				if (term.nparams < MAX_PARAMS)
					term.nparams++;
			} else if (c >= 0x3c && c <= 0x3f) {
				term.private = (char)c;
			} else if (c >= 0x20 && c <= 0x2f) {
				term.inter = (char)c;
			} else {
				term.state = GROUND;
				csi((char)c);
			}
			break;
		case CHARSET:
			term.graphics[term.charset_set] = c == '0';
			term.state = GROUND;
			break;
		case SKIP:
		case STRING:
			term.state = GROUND;
			break;
		}
	}
}

/* Drawing: the rows that changed, the cursor, the end of the view */

static void flush(void)
{
	for (int y = 0; y < term.rows; y++) {
		if (term.dirty[y]) {
			line_render(term.lines[y], row(y));
			term.dirty[y] = false;
		}
	}
	lv_obj_set_hidden(term.cursor, !term.cursor_on);
	lv_obj_set_pos(term.cursor, term.x * term.cw, (term.history + term.y) * term.lh);
	if (term.follow) {
		term.follow = false;
		lv_obj_update_layout(term.view);
		int32_t below = lv_obj_get_scroll_bottom(term.view);
		if (below > 0)
			lv_obj_scroll_by(term.view, 0, -below, LV_ANIM_OFF);
	}
}

/* Shell */

static void send(const char *s, size_t n)
{
	while (term.fd >= 0 && n) {
		ssize_t w = write(term.fd, s, n);
		if (w < 0) {
			if (errno != EINTR) {
				perror("terminal: write");
				break;
			}
			continue;
		}
		s += w;
		n -= (size_t)w;
	}
	term.follow = true;
}

static bool shell_start(void)
{
	struct passwd *pw = getpwuid(getuid());
	const char *home = pw && pw->pw_dir[0] ? pw->pw_dir : "/";
#ifdef TERMINAL_SHELL /* the tests' and the simulator's (tests/Makefile): the host user's would be theirs */
	const char *shell = TERMINAL_SHELL;
#else
	const char *shell = pw && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
#endif
	const char *base = strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell;
	const char *tz = getenv("TZ");

	/* Everything the child needs, made before fork() */
	char arg0[64], env_home[256], env_shell[256], env_user[64], env_logname[64], env_tz[256];
	snprintf(arg0, sizeof(arg0), "-%s", base);
	snprintf(env_home, sizeof(env_home), "HOME=%s", home);
	snprintf(env_shell, sizeof(env_shell), "SHELL=%s", shell);
	snprintf(env_user, sizeof(env_user), "USER=%s", pw ? pw->pw_name : "root");
	snprintf(env_logname, sizeof(env_logname), "LOGNAME=%s", pw ? pw->pw_name : "root");
	snprintf(env_tz, sizeof(env_tz), "TZ=%s", tz ? tz : "UTC");
	char *envp[] = {
		"TERM=xterm", "LANG=C.UTF-8", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		env_home, env_shell, env_user, env_logname, env_tz, NULL,
	};
	struct winsize ws = { .ws_row = (unsigned short)term.rows, .ws_col = (unsigned short)term.cols };

	fflush(NULL);
	pid_t pid = forkpty(&term.fd, NULL, NULL, &ws);
	if (pid < 0) {
		perror("terminal: forkpty");
		term.fd = -1;
		return false;
	}
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		for (int s = 1; s < NSIG; s++)
			signal(s, SIG_DFL);
		close_range(3, ~0U, 0);
		if (chdir(home) < 0 && chdir("/") < 0)
			_exit(126);
		execle(shell, arg0, (char *)NULL, envp);
		_exit(127);
	}
	term.pid = pid;
	fcntl(term.fd, F_SETFL, fcntl(term.fd, F_GETFL) | O_NONBLOCK);
	fcntl(term.fd, F_SETFD, FD_CLOEXEC);
	return true;
}

/* The shell's session after SIGHUP: killed if it is still there, reaped */
static void kill_cb(lv_timer_t *timer)
{
	pid_t pid = (pid_t)(intptr_t)lv_timer_get_user_data(timer);
	kill(-pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

static void tick_cb(lv_timer_t *timer)
{
	(void)timer;
	if (term.resized) {
		term.resized = false;
		resize(lv_obj_get_content_height(term.view) / term.lh);
		term.follow = true;
	}

	static unsigned char buf[16384];
	size_t total = 0;
	while (term.fd >= 0 && total < READ_MAX) {
		ssize_t n = read(term.fd, buf, sizeof(buf));
		if (n > 0) {
			parse(buf, (size_t)n);
			total += (size_t)n;
			term.follow = true;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			break;
		} else { /* EIO: the shell exited */
			close(term.fd);
			term.fd = -1;
			if (ui_current() == term.scr) {
				ui_pop();
				return;
			}
		}
	}
	flush();
}

/* Input */

static void keyboard_show(bool show)
{
	ui_keyboard_show(term.kb, show);
	lv_obj_set_hidden(term.keys, !show);
	term.follow = true;
}

static bool ctrl_take(void)
{
	if (!lv_buttonmatrix_has_button_ctrl(term.keys, 2, LV_BUTTONMATRIX_CTRL_CHECKED))
		return false;
	lv_buttonmatrix_clear_button_ctrl(term.keys, 2, LV_BUTTONMATRIX_CTRL_CHECKED);
	return true;
}

/* What the keyboard types: to the shell, not into the text area */
static void ta_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_CANCEL) {
		keyboard_show(false);
		return;
	}
	if (code != LV_EVENT_INSERT)
		return;
	const char *text = lv_event_get_param(e);
	lv_textarea_set_insert_replace(term.ta, "");

	if (strcmp(text, "\n") == 0) {
		send("\r", 1);
	} else if (ctrl_take() && text[0] && !text[1]) {
		char c = text[0];
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		if (c == '?')
			c = 0x7f;
		else if (c >= '@' && c <= '_')
			c &= 0x1f;
		else if (c == ' ')
			c = 0;
		send(&c, 1);
	} else {
		send(text, strlen(text)); /* backspace is DEL, as for a VT */
	}
}

static const char *const keys_map[] = {
	/* NON-NLS-BEGIN: the keys' names */
	"Esc", "Tab", "Ctrl", "|", "/", "-", LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT, "",
	/* NON-NLS-END */
};

#define KEY (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)

/* Arrows act on press and repeat while held */
static const lv_buttonmatrix_ctrl_t keys_ctrl[] = {
	KEY, KEY, KEY | LV_BUTTONMATRIX_CTRL_CHECKABLE, KEY, KEY, KEY, 0, 0, 0, 0,
};

static void keys_cb(lv_event_t *e)
{
	(void)e;
	uint32_t id = lv_buttonmatrix_get_selected_button(term.keys);
	if (id == LV_BUTTONMATRIX_BUTTON_NONE)
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	if (id == 2)
		return; /* Ctrl: checked for the next key */
	if (id >= 6) {
		char seq[4] = { 0x1b, term.app_cursor ? 'O' : '[', "DABC"[id - 6], 0 };
		send(seq, 3);
		return;
	}
	static const char *const keys[] = { "\033", "\t", NULL, "|", "/", "-" };
	send(keys[id], strlen(keys[id]));
}

static void view_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_SIZE_CHANGED)
		term.resized = true;
	else if (lv_obj_is_hidden(term.kb))
		keyboard_show(true);
}

static void delete_cb(lv_event_t *e)
{
	(void)e;
	lv_timer_delete(term.timer);
	if (term.fd >= 0)
		close(term.fd);
	if (term.pid > 0) {
		kill(-term.pid, SIGHUP);
		kill(-term.pid, SIGCONT);
		lv_timer_t *timer = lv_timer_create(kill_cb, KILL_MS, (void *)(intptr_t)term.pid);
		lv_timer_set_repeat_count(timer, 1);
	}
	free(term.cells);
	free(term.other);
	free(term.text);
	memset(&term, 0, sizeof(term));
}

void terminal_open(void)
{
	lv_obj_t *body;
	term.scr = ui_app_screen_create(_("Terminal"), NULL, &body);
	lv_obj_add_event_cb(term.scr, delete_cb, LV_EVENT_DELETE, NULL);
	term.fd = -1;
	term.cw = lv_font_get_glyph_width(&ui_font_mono_20, 'M', 0);
	term.lh = lv_font_get_line_height(&ui_font_mono_20);

	uint32_t bg = ui_light ? 0xffffff : 0x000000;
	term.view = ui_box(body);
	lv_obj_set_width(term.view, LV_PCT(100));
	lv_obj_set_flex_grow(term.view, 1);
	lv_obj_set_flex_flow(term.view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_opa(term.view, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(term.view, lv_color_hex(bg), 0);
	lv_obj_set_style_text_font(term.view, &ui_font_mono_20, 0);
	lv_obj_set_style_text_color(term.view, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_scrollable(term.view, true);
	lv_obj_set_scroll_dir(term.view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(term.view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_clickable(term.view, true);
	lv_obj_add_event_cb(term.view, view_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(term.view, view_cb, LV_EVENT_SIZE_CHANGED, NULL);

	/* Under the text: the row's label draws the character over it */
	term.cursor = lv_obj_create(term.view);
	lv_obj_remove_style_all(term.cursor);
	lv_obj_set_size(term.cursor, term.cw, term.lh);
	lv_obj_set_style_bg_opa(term.cursor, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(term.cursor, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_ignore_layout(term.cursor, true);
	lv_obj_set_clickable(term.cursor, false);

	term.keys = lv_buttonmatrix_create(body);
	lv_buttonmatrix_set_map(term.keys, keys_map);
	lv_buttonmatrix_set_ctrl_map(term.keys, keys_ctrl);
	lv_obj_set_size(term.keys, LV_PCT(100), KEYS_H);
	lv_obj_set_style_pad_all(term.keys, 10, 0);
	lv_obj_set_style_pad_bottom(term.keys, 0, 0);
	lv_obj_set_style_pad_gap(term.keys, 8, 0);
	lv_obj_set_style_radius(term.keys, 0, 0);
	lv_obj_set_style_border_width(term.keys, 0, 0);
	lv_obj_set_style_shadow_width(term.keys, 0, 0);
	lv_obj_set_style_bg_opa(term.keys, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(term.keys, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_style_text_font(term.keys, &ui_font_24, LV_PART_ITEMS);
	lv_obj_set_style_text_color(term.keys, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_radius(term.keys, 12, LV_PART_ITEMS);
	lv_obj_set_style_border_width(term.keys, 0, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width(term.keys, 0, LV_PART_ITEMS);
	lv_obj_set_style_outline_width(term.keys, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_color_filter_opa(term.keys, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(term.keys, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(term.keys, lv_color_hex(UI_COLOR_BUTTON), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(term.keys, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(term.keys, lv_color_hex(UI_COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(term.keys, lv_color_hex(UI_COLOR_ACCENT_PRESSED),
	                          LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(term.keys, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_add_event_cb(term.keys, keys_cb, LV_EVENT_VALUE_CHANGED, NULL);

	/* One letter before the cursor: the keyboard stays in lower case */
	term.ta = lv_textarea_create(body);
	lv_textarea_set_text(term.ta, "x");
	lv_obj_set_hidden(term.ta, true);
	lv_obj_add_event_cb(term.ta, ta_cb, LV_EVENT_ALL, NULL);
	term.kb = ui_keyboard_create(body, term.ta);

	/* 59 columns on the 720 px screen, centered */
	int32_t width = lv_display_get_horizontal_resolution(lv_obj_get_display(term.scr));
	term.cols = (width - 8) / term.cw;
	lv_obj_set_style_pad_hor(term.view, (width - term.cols * term.cw) / 2, 0);
	term.cells = malloc((size_t)MAX_ROWS * term.cols * sizeof(struct cell));
	term.other = malloc((size_t)MAX_ROWS * term.cols * sizeof(struct cell));
	term.text = malloc((size_t)term.cols * 4 + 1);

	keyboard_show(true);
	lv_obj_update_layout(term.scr);
	resize(lv_obj_get_content_height(term.view) / term.lh);
	reset();
	term.resized = false;

	if (!shell_start()) {
		const char *text = _("No shell: see phone-ui's log");
		parse((const unsigned char *)text, strlen(text)); /* UTF-8 */
	}
	term.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	ui_push(term.scr);
}
