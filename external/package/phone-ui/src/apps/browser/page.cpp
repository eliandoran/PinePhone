/*
 * The browser's litehtml container: litehtml parses the page, applies its CSS and
 * lays it out; this draws it on the CPU into BGRA buffers (the tiles browser.c
 * uploads as textures), with the UI's bitmap fonts at the size nearest to the one
 * asked for (a book's: all in its serif, ui_font_book, at the size asked), a
 * monospace family (code) in ui_font_mono_20 whatever its size. Bold is
 * each glyph drawn twice 1 px apart, italic the glyphs sheared. Emoji are the fonts'
 * image glyphs (emoji.c), drawn in their colors, sequences composed as labels do.
 * Not drawn: rounded corners, border styles (all solid), radial and conic
 * gradients (a fill of their middle color). Images are scaled nearest-neighbor.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <litehtml/litehtml.h>
#include <litehtml/url.h>

#include "lvgl.h"

#include "browser.h"

extern "C" {
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_20)
LV_FONT_DECLARE(ui_font_24)
LV_FONT_DECLARE(ui_font_26)
LV_FONT_DECLARE(ui_font_32)
LV_FONT_DECLARE(ui_font_48)
LV_FONT_DECLARE(ui_font_mono_20)
extern const uint8_t ui_font_book_ttf[];
extern const size_t ui_font_book_ttf_size;
char *ui_emoji_compose(const char *text); /* ui.h's (emoji.c) */
}

namespace {

constexpr float S = BROWSER_SCALE;

/* The UI's sizes, the nearest one taken */
const lv_font_t *const fonts[] = { &ui_font_16, &ui_font_20, &ui_font_24,
                                   &ui_font_26, &ui_font_32, &ui_font_48 };
const int font_sizes[] = { 16, 20, 24, 26, 32, 48 };
constexpr int FONTS = sizeof(font_sizes) / sizeof(font_sizes[0]);
/* A font: one of those, the monospace (MONO_FONT), or the books' serif at BOOK_FONT
 * + its size in pixels */
constexpr int MONO_FONT = FONTS;
constexpr int BOOK_FONT = 1000;
/* A word longer than this (characters) is laid out in pieces (split_text()), a
 * piece without a slash or the like at most PIECE long */
constexpr size_t LONG_WORD = 24, PIECE = 12;

#if LV_USE_TINY_TTF
/* The books' serif at each size asked, made when first asked and kept: Tiny TTF
 * draws its glyphs from the TrueType (ui_font_book.c), Inter for what it lacks */
const lv_font_t *book_font(int px)
{
	static std::map<int, lv_font_t *> made;
	lv_font_t *&f = made[px];
	if (!f) {
		f = lv_tiny_ttf_create_data_ex(ui_font_book_ttf, ui_font_book_ttf_size, px,
		                               LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
		if (f)
			f->fallback = &ui_font_26;
	}
	return f ? f : &ui_font_26;
}
#else
const lv_font_t *book_font(int)
{
	return &ui_font_26;
}
#endif

const lv_font_t *font_get(int font)
{
	return font >= BOOK_FONT ? book_font(font - BOOK_FONT) : font == MONO_FONT ? &ui_font_mono_20 : fonts[font];
}

int font_size(int font)
{
	return font >= BOOK_FONT ? font - BOOK_FONT : font == MONO_FONT ? 20 : font_sizes[font];
}

float f32(litehtml::pixel_t v)
{
	return (float)v;
}

/* CSS to device pixels */
int dev(float v)
{
	return (int)std::lround(v * S);
}

/* A page is built on a thread of its own when long (browser.c's jobs):
 * what it measures text with (the glyphs below, LVGL's fonts, emoji.c) is the
 * interface's, used under LVGL's lock, which the interface holds while it runs
 * (lv_timer_handler(), main.c's frames). Recursive: taken on the UI thread too. */
struct UiLock {
	UiLock() { lv_lock(); }
	~UiLock() { lv_unlock(); }
	UiLock(const UiLock &) = delete;
	UiLock &operator=(const UiLock &) = delete;
};

struct Rect {
	int x0, y0, x1, y1; /* x1 and y1 excluded */
	bool empty() const { return x0 >= x1 || y0 >= y1; }
	Rect operator&(const Rect &o) const
	{
		return { std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1) };
	}
};

/* A tile being drawn: rows y0 to y0 + h - 1 of the page. Measuring (page_paginate()):
 * no pixels, an empty clip, and where text and images are go to spans */
struct Canvas {
	uint8_t *px;
	int w, h, y0;
	Rect clip; /* the tile and the clips litehtml set */
	std::vector<std::pair<int, int>> *spans = nullptr; /* rows y0 to y1 - 1 */
	bool dark = false; /* a dark page (page_set_dark()) */
	uint32_t paper = PAGE_DARK_PAPER, ink = PAGE_DARK_INK; /* its colors then (page_set_dark_colors()) */
	int *right = nullptr; /* measuring too: how far right anything was drawn */

	Rect rect(const litehtml::position &p) const
	{
		return { dev(f32(p.x)), dev(f32(p.y)) - y0, dev(f32(p.x) + f32(p.width)),
		         dev(f32(p.y) + f32(p.height)) - y0 };
	}
};

/* A color as drawn: on a dark page, as light as it was dark (white the paper, black
 * the ink, each channel shifted so that they come out exactly), its hue kept */
litehtml::web_color tone(const Canvas *c, litehtml::web_color col)
{
	if (!c->dark)
		return col;
	float y = 0.2126f * col.red + 0.7152f * col.green + 0.0722f * col.blue;
	auto channel = [y](uint8_t v, float paper, float ink) {
		float shift = ink - y * (ink - paper) / 255 - y;
		return (uint8_t)std::clamp(std::lround(v + shift), 0L, 255L);
	};
	col.red = channel(col.red, c->paper >> 16 & 0xff, c->ink >> 16 & 0xff);
	col.green = channel(col.green, c->paper >> 8 & 0xff, c->ink >> 8 & 0xff);
	col.blue = channel(col.blue, c->paper & 0xff, c->ink & 0xff);
	return col;
}

/* Glyphs: A8 bitmaps from LVGL's fonts, or an emoji's BGRA image, kept for good (a
 * few hundred per size) */

struct Glyph {
	int adv = 0, w = 0, h = 0, ofs_x = 0, ofs_y = 0;
	std::vector<uint8_t> alpha;
	std::vector<uint8_t> bgra; /* an emoji, straight alpha */
};

std::unordered_map<uint64_t, Glyph> glyphs;

/* An emoji's image (emoji.c's ARGB8888 draw buffer) copied */
void emoji_copy(Glyph &g, const lv_font_glyph_dsc_t &d)
{
	const lv_draw_buf_t *src = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap((lv_font_glyph_dsc_t *)&d, nullptr);
	if (!src || src->header.cf != LV_COLOR_FORMAT_ARGB8888 || src->header.w < d.box_w || src->header.h < d.box_h)
		return;
	g.w = d.box_w;
	g.h = d.box_h;
	g.ofs_x = d.ofs_x;
	g.ofs_y = d.ofs_y;
	g.bgra.resize((size_t)g.w * g.h * 4);
	for (int y = 0; y < g.h; y++)
		memcpy(&g.bgra[(size_t)y * g.w * 4], src->data + (size_t)y * src->header.stride, (size_t)g.w * 4);
}

const Glyph &glyph_get(int font, uint32_t cp)
{
	uint64_t key = (uint64_t)font << 32 | cp;
	auto it = glyphs.find(key);
	if (it != glyphs.end())
		return it->second;
	const lv_font_t *f = font_get(font);
	lv_font_glyph_dsc_t d;
	memset(&d, 0, sizeof(d));
	if (!lv_font_get_glyph_dsc(f, &d, cp, 0)) {
		/* Not kept beyond the text: an emoji may come later (the browser's build
		 * fetches them) */
		static Glyph missing;
		missing = Glyph();
		missing.adv = cp == ' ' ? font_size(font) / 4 : font_size(font) / 2;
		return missing;
	}
	Glyph &g = glyphs[key];
	g.adv = d.adv_w;
	if (d.format == LV_FONT_GLYPH_FORMAT_IMAGE && d.box_w && d.box_h) {
		emoji_copy(g, d);
		return g;
	}
	if (d.format < LV_FONT_GLYPH_FORMAT_A1 || d.format > LV_FONT_GLYPH_FORMAT_A8 || !d.box_w || !d.box_h)
		return g;
	lv_draw_buf_t *buf = lv_draw_buf_create(d.box_w, d.box_h, LV_COLOR_FORMAT_A8, 0);
	if (!buf)
		return g;
	const lv_draw_buf_t *out = (const lv_draw_buf_t *)lv_font_get_glyph_bitmap(&d, buf);
	if (out) {
		g.w = d.box_w;
		g.h = d.box_h;
		g.ofs_x = d.ofs_x;
		g.ofs_y = d.ofs_y;
		g.alpha.resize((size_t)g.w * g.h);
		for (int y = 0; y < g.h; y++)
			memcpy(&g.alpha[(size_t)y * g.w], out->data + (size_t)y * out->header.stride, g.w);
	}
	lv_font_glyph_release_draw_data(&d);
	lv_draw_buf_destroy(buf);
	return g;
}

uint32_t utf8_next(const char *&s)
{
	const unsigned char *u = (const unsigned char *)s;
	uint32_t c = *u;
	int n = c < 0xc0 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
	if (n)
		c &= 0x3f >> n;
	s++;
	for (int i = 0; i < n && (*s & 0xc0) == 0x80; i++)
		c = c << 6 | (*s++ & 0x3f);
	return c;
}

/* Characters the fonts lack, as ones they have; 0: nothing */
uint32_t substitute(uint32_t cp)
{
	if (cp == 0xa0 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == '\t')
		return ' ';
	if (cp == '\n' || cp == '\r' || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) || cp == 0xfeff)
		return 0;
	if ((cp >= 0x2010 && cp <= 0x2012) || cp == 0x2212)
		return '-';
	if (cp == 0x2022 || cp == 0x2219)
		return 0xb7;
	if (cp == 0x2032)
		return '\'';
	if (cp == 0x2033)
		return '"';
	return cp;
}

struct Font {
	int index;
	int ascent, descent, x_height, line; /* device pixels; line: decorations */
	bool bold, italic;
	int decoration;
	litehtml::web_color decoration_color;
};

/* Text with its emoji sequences as the characters the fonts draw them with: a copy
 * when it may have any (bytes from U+2000 up: joiners, selectors, keycaps, flags...) */
struct Composed {
	char *copy = nullptr;
	const char *text;
	explicit Composed(const char *t) : text(t)
	{
		for (const unsigned char *s = (const unsigned char *)t; *s; s++)
			if (*s >= 0xe2) {
				copy = ui_emoji_compose(t);
				if (copy)
					text = copy;
				break;
			}
	}
	~Composed() { free(copy); }
	Composed(const Composed &) = delete;
	Composed &operator=(const Composed &) = delete;
};

int text_measure(const Font &f, const char *text)
{
	int w = 0;
	Composed composed(text);
	for (const char *s = composed.text; *s;) {
		uint32_t cp = substitute(utf8_next(s));
		if (cp)
			w += glyph_get(f.index, cp).adv + f.bold;
	}
	return w;
}

/* Pixels */

inline void blend(uint8_t *d, uint8_t r, uint8_t g, uint8_t b, unsigned a)
{
	if (a >= 255) {
		d[0] = b;
		d[1] = g;
		d[2] = r;
		return;
	}
	unsigned na = 255 - a;
	d[0] = (uint8_t)((b * a + d[0] * na + 127) / 255);
	d[1] = (uint8_t)((g * a + d[1] * na + 127) / 255);
	d[2] = (uint8_t)((r * a + d[2] * na + 127) / 255);
}

void fill(Canvas *c, Rect r, litehtml::web_color col)
{
	col = tone(c, col);
	r = r & c->clip;
	if (r.empty() || !col.alpha)
		return;
	for (int y = r.y0; y < r.y1; y++) {
		uint8_t *d = c->px + ((size_t)y * c->w + r.x0) * 4;
		for (int x = r.x0; x < r.x1; x++, d += 4)
			blend(d, col.red, col.green, col.blue, col.alpha);
	}
}

/* A disc filling r (a ring `ring` px wide if ring > 0), antialiased */
void disc(Canvas *c, Rect r, litehtml::web_color col, float ring)
{
	col = tone(c, col);
	float cx = (r.x0 + r.x1) / 2.0f, cy = (r.y0 + r.y1) / 2.0f;
	float rad = std::min(r.x1 - r.x0, r.y1 - r.y0) / 2.0f;
	Rect a = r & c->clip;
	for (int y = a.y0; y < a.y1; y++) {
		for (int x = a.x0; x < a.x1; x++) {
			float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
			float cover = std::clamp(rad - d + 0.5f, 0.0f, 1.0f);
			if (ring > 0)
				cover = std::min(cover, std::clamp(d - (rad - ring) + 0.5f, 0.0f, 1.0f));
			if (cover > 0)
				blend(c->px + ((size_t)y * c->w + x) * 4, col.red, col.green, col.blue,
				      (unsigned)(cover * col.alpha));
		}
	}
}

/* r filled, its corners rounded (radii in pixels: top left, top right, bottom right,
 * bottom left), antialiased; clipped to `clip` */
void rounded(Canvas *c, Rect r, Rect clip, litehtml::web_color col, const float rad[4])
{
	col = tone(c, col);
	Rect a = r & clip & c->clip;
	float half = std::min(r.x1 - r.x0, r.y1 - r.y0) / 2.0f;
	for (int y = a.y0; y < a.y1; y++) {
		uint8_t *d = c->px + ((size_t)y * c->w + a.x0) * 4;
		for (int x = a.x0; x < a.x1; x++, d += 4) {
			float px = x + 0.5f, py = y + 0.5f;
			bool left = px < (r.x0 + r.x1) / 2.0f, top = py < (r.y0 + r.y1) / 2.0f;
			float k = std::min(rad[top ? (left ? 0 : 1) : (left ? 3 : 2)], half);
			/* The corner's circle's center */
			float cx = left ? r.x0 + k : r.x1 - k, cy = top ? r.y0 + k : r.y1 - k;
			float cover = 1;
			if (k > 0 && (left ? px < cx : px > cx) && (top ? py < cy : py > cy))
				cover = std::clamp(k - std::hypot(px - cx, py - cy) + 0.5f, 0.0f, 1.0f);
			if (cover > 0)
				blend(d, col.red, col.green, col.blue, (unsigned)(cover * col.alpha));
		}
	}
}

void glyph_blit(Canvas *c, const Glyph &g, int gx, int gy, int base, litehtml::web_color col,
                bool italic)
{
	col = tone(c, col);
	for (int r = 0; r < g.h; r++) {
		int y = gy + r;
		if (y < c->clip.y0 || y >= c->clip.y1)
			continue;
		int x0 = gx + (italic ? (base - y) / 5 : 0);
		const uint8_t *a = &g.alpha[(size_t)r * g.w];
		uint8_t *row = c->px + (size_t)y * c->w * 4;
		for (int k = 0; k < g.w; k++) {
			int x = x0 + k;
			if (!a[k] || x < c->clip.x0 || x >= c->clip.x1)
				continue;
			blend(row + x * 4, col.red, col.green, col.blue, a[k] * col.alpha / 255);
		}
	}
}

/* An emoji in its colors, `alpha` the text's */
void emoji_blit(Canvas *c, const Glyph &g, int gx, int gy, unsigned alpha)
{
	for (int r = 0; r < g.h; r++) {
		int y = gy + r;
		if (y < c->clip.y0 || y >= c->clip.y1)
			continue;
		const uint8_t *p = &g.bgra[(size_t)r * g.w * 4];
		uint8_t *row = c->px + (size_t)y * c->w * 4;
		for (int k = 0; k < g.w; k++, p += 4) {
			int x = gx + k;
			if (!p[3] || x < c->clip.x0 || x >= c->clip.x1)
				continue;
			blend(row + x * 4, p[2], p[1], p[0], p[3] * alpha / 255);
		}
	}
}

struct Image {
	uint8_t *px = nullptr; /* BGRA, NULL: failed */
	int w = 0, h = 0, natural_w = 0, natural_h = 0;
	bool line_art = false; /* line_art(): turned as the text on dark pages */
};

/* Whether a BGRA image is line art (an engraving, an ink drawing; on white or
 * transparent): its opaque pixels nearly all gray and nearly all close to black or
 * white. Photos, gray ones too, have colors or many tones in between. */
bool line_art(const uint8_t *px, int w, int h)
{
	size_t opaque = 0, gray = 0, extreme = 0;
	for (size_t i = 0; i < (size_t)w * h; i++) {
		const uint8_t *p = px + i * 4;
		if (p[3] < 128)
			continue;
		opaque++;
		int b = p[0], g = p[1], r = p[2];
		if (std::max({ r, g, b }) - std::min({ r, g, b }) > 24)
			continue;
		gray++;
		int y = (r * 54 + g * 183 + b * 19) >> 8;
		extreme += y < 64 || y > 192;
	}
	return opaque && gray >= opaque * 98 / 100 && extreme >= opaque * 85 / 100;
}

/* An image scaled into dst, drawn where it meets `clip`; line art turned on a dark
 * page */
void image_blit(Canvas *c, const Image &img, Rect dst, Rect clip)
{
	Rect r = dst & clip & c->clip;
	int dw = dst.x1 - dst.x0, dh = dst.y1 - dst.y0;
	if (r.empty() || dw <= 0 || dh <= 0)
		return;
	bool turn = c->dark && img.line_art;
	for (int y = r.y0; y < r.y1; y++) {
		int sy = (int)((int64_t)(y - dst.y0) * img.h / dh);
		const uint8_t *src = img.px + (size_t)sy * img.w * 4;
		uint8_t *d = c->px + ((size_t)y * c->w + r.x0) * 4;
		for (int x = r.x0; x < r.x1; x++, d += 4) {
			const uint8_t *s = src + (int64_t)(x - dst.x0) * img.w / dw * 4;
			if (!s[3])
				continue;
			if (turn) {
				litehtml::web_color col = tone(c, litehtml::web_color(s[2], s[1], s[0]));
				blend(d, col.red, col.green, col.blue, s[3]);
			} else {
				blend(d, s[2], s[1], s[0], s[3]);
			}
		}
	}
}

/* A color through an image's alpha (a mask-image), the image scaled into dst */
void mask_blit(Canvas *c, const Image &img, Rect dst, Rect clip, litehtml::web_color col)
{
	Rect r = dst & clip & c->clip;
	int dw = dst.x1 - dst.x0, dh = dst.y1 - dst.y0;
	if (r.empty() || dw <= 0 || dh <= 0 || !col.alpha)
		return;
	for (int y = r.y0; y < r.y1; y++) {
		int sy = (int)((int64_t)(y - dst.y0) * img.h / dh);
		const uint8_t *src = img.px + (size_t)sy * img.w * 4;
		uint8_t *d = c->px + ((size_t)y * c->w + r.x0) * 4;
		for (int x = r.x0; x < r.x1; x++, d += 4) {
			unsigned a = src[(int64_t)(x - dst.x0) * img.w / dw * 4 + 3] * col.alpha / 255;
			if (a)
				blend(d, col.red, col.green, col.blue, a);
		}
	}
}

/* The first position origin + k * size at or before `from` */
int tile_start(int origin, int from, int size)
{
	return origin + (int)std::floor((float)(from - origin) / size) * size;
}

litehtml::web_color gradient_at(const std::vector<litehtml::background_layer::color_point> &p, float t)
{
	if (t <= p.front().offset)
		return p.front().color;
	for (size_t i = 1; i < p.size(); i++) {
		if (t > p[i].offset)
			continue;
		float span = p[i].offset - p[i - 1].offset;
		float f = span > 0 ? (t - p[i - 1].offset) / span : 1;
		auto mix = [f](int a, int b) { return (litehtml::byte)std::lround(a + (b - a) * f); };
		const auto &a = p[i - 1].color, &b = p[i].color;
		litehtml::web_color out;
		out.red = mix(a.red, b.red);
		out.green = mix(a.green, b.green);
		out.blue = mix(a.blue, b.blue);
		out.alpha = mix(a.alpha, b.alpha);
		return out;
	}
	return p.back().color;
}

std::string resolve_url(const std::string &base, const std::string &ref)
{
	if (ref.empty())
		return base;
	return litehtml::resolve(litehtml::url(base), litehtml::url(ref)).str();
}

class Page : public litehtml::document_container {
public:
	std::string html, url, base, title, clicked;
	int vw, vh; /* viewport, CSS pixels */
	litehtml::document::ptr doc;
	std::map<std::string, std::string> css; /* arrived, "" if failed */
	char *(*read_css)(const char *url, void *user) = nullptr; /* page_create_local() */
	void *read_css_user = nullptr;
	int book_px = 0; /* the books' font at this size for all the text, 0: the UI's */
	std::map<std::string, Image> images;
	std::deque<std::string> wanted[2]; /* per page_resource; deque: c_str() stays valid */
	size_t wanted_next[2] = { 0, 0 };
	std::set<std::string> requested;
	Canvas *canvas = nullptr;
	std::vector<Rect> clips;
	uint32_t parse_ms = 0, layout_ms = 0; /* the last build or layout */
	bool built = false; /* page_load() leaves it to page_rebuild() */
	bool dark = false; /* page_set_dark() */
	uint32_t paper = PAGE_DARK_PAPER, ink = PAGE_DARK_INK; /* page_set_dark_colors() */

	~Page()
	{
		doc.reset(); /* deletes its fonts through delete_font() */
		for (auto &i : images)
			free(i.second.px);
	}

	void build()
	{
		built = true;
		title.clear();
		base = url;
		uint32_t start = lv_tick_get();
		doc = litehtml::document::createFromString(litehtml::estring(html), this);
		parse_ms = lv_tick_elaps(start);
		if (doc)
			find_title();
		layout();
	}

	void layout()
	{
		uint32_t start = lv_tick_get();
		if (doc)
			doc->render(vw);
		layout_ms = lv_tick_elaps(start);
	}

	/* The style sheets <link>ed before <body>, asked for before the page is built:
	 * built once with them, not first without to find them (as long again: 6 s for
	 * Wikipedia's article on Wikipedia). Not with a <base>, which they would be resolved
	 * against; those it misses (in the body, @import) the build asks for. */
	void want_linked_css()
	{
		auto ifind = [this](const char *what, size_t from) {
			auto it = std::search(html.begin() + from, html.end(), what, what + strlen(what),
			                      [](char a, char b) { return std::tolower((unsigned char)a) == b; });
			return it == html.end() ? std::string::npos : (size_t)(it - html.begin());
		};
		size_t body = ifind("<body", 0);
		if (ifind("<base", 0) < body)
			return;
		for (size_t at = ifind("<link", 0); at < body; at = ifind("<link", at + 5)) {
			size_t end = html.find('>', at);
			if (end == std::string::npos)
				return;
			std::string tag = html.substr(at, end - at), lower = tag;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			std::string rel = " " + attr_value(tag, lower, "rel") + " ", href = attr_value(tag, lower, "href");
			std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
			if (rel.find(" stylesheet ") != std::string::npos && rel.find(" alternate ") == std::string::npos &&
			    !href.empty() && attr_value(tag, lower, "media") != "print")
				want(resolve_url(url, href), PAGE_CSS);
		}
	}

	/* An attribute's value in a tag's text (`lower` the same lower-cased), &amp; decoded */
	static std::string attr_value(const std::string &tag, const std::string &lower, const char *name)
	{
		std::string key = std::string(name) + "=";
		for (size_t at = lower.find(key); at != std::string::npos; at = lower.find(key, at + 1)) {
			if (!std::isspace((unsigned char)lower[at - 1]))
				continue;
			size_t v = at + key.size(), end;
			if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
				end = tag.find(tag[v], v + 1);
				v++;
			} else {
				end = tag.find_first_of(" \t\r\n/", v);
			}
			std::string value = tag.substr(v, end == std::string::npos ? std::string::npos : end - v);
			for (size_t amp = value.find("&amp;"); amp != std::string::npos; amp = value.find("&amp;", amp + 1))
				value.erase(amp + 1, 4);
			return value;
		}
		return "";
	}

	/* How far right anything is drawn (device pixels): text and images past their boxes
	 * (a word that cannot break), which the document's width does not count */
	int content_width()
	{
		if (!doc)
			return 0;
		std::vector<std::pair<int, int>> spans;
		int right = 0;
		Canvas c = { nullptr, 0, 0, 0, { 0, 0, 0, 0 }, &spans };
		c.right = &right;
		canvas = &c;
		clips.clear();
		litehtml::position clip(0, 0, 1 << 20, f32(doc->height()) + 1); /* nothing culled sideways */
		doc->draw((litehtml::uint_ptr)&c, 0, 0, &clip);
		canvas = nullptr;
		return right;
	}

	/* Where pages h high start: each as long as it can be without cutting a line
	 * of text or an image through (one taller than a page is) */
	std::vector<int> paginate(int h)
	{
		std::vector<std::pair<int, int>> spans;
		int height = doc ? (int)std::ceil(f32(doc->height()) * S) : 0;
		if (doc) {
			Canvas c = { nullptr, 0, 0, 0, { 0, 0, 0, 0 }, &spans };
			canvas = &c;
			clips.clear();
			litehtml::position clip(0, 0, vw, f32(doc->height()) + 1);
			doc->draw((litehtml::uint_ptr)&c, 0, 0, &clip);
			canvas = nullptr;
		}
		/* inside[y]: a cut above row y would go through something */
		std::vector<uint8_t> inside(height + 1);
		for (auto &s : spans)
			if (s.second - s.first <= h)
				for (int y = std::max(s.first + 1, 0); y < s.second && y <= height; y++)
					inside[y] = 1;
		std::vector<int> starts = { 0 };
		for (int start = 0; start + h < height;) {
			int end = start + h;
			while (end > start + h / 2 && inside[end])
				end--;
			if (end <= start + h / 2)
				end = start + h;
			starts.push_back(end);
			start = end;
		}
		return starts;
	}

	void want(const std::string &u, page_resource kind)
	{
		/* epub: files of a book (books/), which only the reader's pages ask for */
		if (u.compare(0, 5, "data:") && u.compare(0, 4, "http") && u.compare(0, 5, "epub:"))
			return;
		if (requested.insert(u).second)
			wanted[kind].push_back(u);
	}

	void draw(uint8_t *bgra, int w, int h, int y)
	{
		/* White or the dark paper, opaque */
		if (dark) {
			const uint8_t bg[4] = { (uint8_t)(paper & 0xff), (uint8_t)(paper >> 8 & 0xff), (uint8_t)(paper >> 16 & 0xff), 0xff };
			for (size_t i = 0; i < (size_t)w * h; i++)
				memcpy(bgra + i * 4, bg, 4);
		} else {
			memset(bgra, 0xff, (size_t)w * h * 4);
		}
		Canvas c = { bgra, w, h, y, { 0, 0, w, h }, nullptr, dark, paper, ink };
		canvas = &c;
		clips.clear();
		if (doc) {
			litehtml::position clip(0, y / S - 1, w / S, h / S + 2);
			doc->draw((litehtml::uint_ptr)&c, 0, 0, &clip);
		}
		canvas = nullptr;
	}

	void clip_update()
	{
		canvas->clip = { 0, 0, canvas->w, canvas->h };
		for (const Rect &r : clips)
			canvas->clip = canvas->clip & r;
	}

	const Image *image_find(const std::string &src, const std::string &baseurl)
	{
		auto it = images.find(resolve_url(baseurl.empty() ? base : baseurl, src));
		return it == images.end() || !it->second.px ? nullptr : &it->second;
	}

	/* document_container */

	litehtml::uint_ptr create_font(const litehtml::font_description &d, const litehtml::document *,
	                               litehtml::font_metrics *fm) override
	{
		UiLock lock;
		float want = f32(d.size) * S;
		int best = 0;
		for (int i = 1; i < FONTS; i++)
			if (std::fabs(font_sizes[i] - want) < std::fabs(font_sizes[best] - want))
				best = i;
		/* Code: any of the families meaning monospace */
		std::string family = d.family;
		std::transform(family.begin(), family.end(), family.begin(), ::tolower);
		for (const char *mono : {"monospace", "mono", "courier", "consolas", "menlo", "monaco"})
			if (family.find(mono) != std::string::npos)
				best = MONO_FONT;
		/* A book's text: all of it in its font, sized as that (em, line heights) */
		if (book_px)
			best = BOOK_FONT + book_px;
		litehtml::pixel_t size = book_px ? litehtml::pixel_t(book_px / S) : d.size;
		const lv_font_t *lv = font_get(best);
		Font *f = new Font;
		f->index = best;
		f->ascent = lv->line_height - lv->base_line;
		f->descent = lv->base_line;
		f->x_height = glyph_get(best, 'x').h;
		f->line = std::max(1, font_size(best) / 16);
		f->bold = d.weight >= 600;
		f->italic = d.style == litehtml::font_style_italic;
		f->decoration = d.decoration_line;
		f->decoration_color = d.decoration_color;
		if (fm) {
			fm->font_size = size;
			fm->height = lv->line_height / S;
			fm->ascent = f->ascent / S;
			fm->descent = f->descent / S;
			fm->x_height = f->x_height / S;
			fm->ch_width = glyph_get(best, '0').adv / S;
			fm->draw_spaces = f->decoration != 0;
			fm->sub_shift = f32(size) / 5;
			fm->super_shift = f32(size) / 3;
		}
		return (litehtml::uint_ptr)f;
	}

	void delete_font(litehtml::uint_ptr font) override
	{
		delete (Font *)font;
	}

	litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr font) override
	{
		UiLock lock;
		return text_measure(*(Font *)font, text) / S;
	}

	void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr font, litehtml::web_color color,
	               const litehtml::position &pos) override
	{
		Canvas *c = (Canvas *)hdc;
		const Font &f = *(Font *)font;
		int x = dev(f32(pos.x)), top = dev(f32(pos.y)) - c->y0, base = top + f.ascent;
		if (c->spans) {
			c->spans->emplace_back(top, base + f.descent);
			if (c->right)
				*c->right = std::max(*c->right, x + (int)std::ceil(text_measure(f, text)));
			return;
		}
		if (top >= c->clip.y1 || base + f.descent <= c->clip.y0)
			return;
		int x0 = x;
		Composed composed(text);
		for (const char *s = composed.text; *s;) {
			uint32_t cp = substitute(utf8_next(s));
			if (!cp)
				continue;
			const Glyph &g = glyph_get(f.index, cp);
			if (!g.bgra.empty()) {
				/* Neither doubled for bold nor sheared */
				emoji_blit(c, g, x + g.ofs_x, base - g.ofs_y - g.h, tone(c, color).alpha);
			} else if (!g.alpha.empty()) {
				int gy = base - g.ofs_y - g.h;
				glyph_blit(c, g, x + g.ofs_x, gy, base, color, f.italic);
				if (f.bold)
					glyph_blit(c, g, x + g.ofs_x + 1, gy, base, color, f.italic);
			}
			x += g.adv + f.bold;
		}
		if (!f.decoration)
			return;
		/* To where the next word starts (its box's end, rounded as that start), not the
		 * glyphs' rounded advances: no gap in the line between words */
		int x1 = dev(f32(pos.x) + f32(pos.width));
		const litehtml::web_color &dc = f.decoration_color.is_current_color ? color : f.decoration_color;
		if (f.decoration & litehtml::text_decoration_line_underline)
			fill(c, { x0, base + f.line + 1, x1, base + 2 * f.line + 1 }, dc);
		if (f.decoration & litehtml::text_decoration_line_line_through)
			fill(c, { x0, base - f.x_height / 2, x1, base - f.x_height / 2 + f.line }, dc);
		if (f.decoration & litehtml::text_decoration_line_overline)
			fill(c, { x0, top, x1, top + f.line }, dc);
	}

	litehtml::pixel_t pt_to_px(float pt) const override
	{
		return pt * 96 / 72;
	}

	litehtml::pixel_t get_default_font_size() const override
	{
		return 16;
	}

	const char *get_default_font_name() const override
	{
		return "sans-serif";
	}

	void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &m) override
	{
		Canvas *c = (Canvas *)hdc;
		Rect r = c->rect(m.pos);
		if (!m.image.empty()) {
			const Image *img = image_find(m.image, m.baseurl ? m.baseurl : "");
			if (img)
				image_blit(c, *img, r, c->clip);
			return;
		}
		switch (m.marker_type) {
		case litehtml::list_style_type_disc:
			disc(c, r, m.color, 0);
			break;
		case litehtml::list_style_type_circle:
			disc(c, r, m.color, std::max(1.0f, (r.x1 - r.x0) / 6.0f));
			break;
		case litehtml::list_style_type_square:
			fill(c, r, m.color);
			break;
		default:
			break;
		}
	}

	void load_image(const char *src, const char *baseurl, bool) override
	{
		want(resolve_url(baseurl && *baseurl ? baseurl : base, src), PAGE_IMAGE);
	}

	void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override
	{
		const Image *img = image_find(src, baseurl ? baseurl : "");
		sz.width = img ? img->natural_w : 0;
		sz.height = img ? img->natural_h : 0;
	}

	void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const std::string &src,
	                const std::string &baseurl) override
	{
		Canvas *c = (Canvas *)hdc;
		const Image *img = image_find(src, baseurl);
		if (!img)
			return;
		if (c->spans) {
			Rect b = c->rect(layer.clip_box), o = c->rect(layer.origin_box);
			c->spans->emplace_back(std::max(b.y0, o.y0), std::min(b.y1, o.y1));
			if (c->right)
				*c->right = std::max(*c->right, std::min(b.x1, o.x1));
			return;
		}
		Rect clip = c->rect(layer.clip_box) & c->clip;
		Rect o = c->rect(layer.origin_box);
		int iw = o.x1 - o.x0, ih = o.y1 - o.y0;
		if (clip.empty() || iw <= 0 || ih <= 0)
			return;
		bool rx = layer.repeat == litehtml::background_repeat_repeat ||
		          layer.repeat == litehtml::background_repeat_repeat_x;
		bool ry = layer.repeat == litehtml::background_repeat_repeat ||
		          layer.repeat == litehtml::background_repeat_repeat_y;
		int xs = rx ? tile_start(o.x0, clip.x0, iw) : o.x0, xe = rx ? clip.x1 : o.x0 + 1;
		int ys = ry ? tile_start(o.y0, clip.y0, ih) : o.y0, ye = ry ? clip.y1 : o.y0 + 1;
		for (int y = ys; y < ye; y += ih)
			for (int x = xs; x < xe; x += iw)
				image_blit(c, *img, { x, y, x + iw, y + ih }, clip);
	}

	/* An icon (Wikipedia's): the background's color through the mask, which fits in the
	 * box, centered (mask-size and mask-position are not read); nothing until it arrives */
	void draw_mask(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const std::string &url,
	               const std::string &baseurl, const litehtml::web_color &color) override
	{
		Canvas *c = (Canvas *)hdc;
		const Image *img = image_find(url, baseurl);
		Rect box = c->rect(layer.border_box);
		int bw = box.x1 - box.x0, bh = box.y1 - box.y0;
		if (!img || bw <= 0 || bh <= 0 || img->natural_w <= 0 || img->natural_h <= 0)
			return;
		if (c->spans) {
			c->spans->emplace_back(box.y0, box.y1);
			if (c->right)
				*c->right = std::max(*c->right, box.x1);
			return;
		}
		int w = bw, h = (int)((int64_t)img->natural_h * bw / img->natural_w);
		if (h > bh) {
			h = bh;
			w = (int)((int64_t)img->natural_w * bh / img->natural_h);
		}
		int x = box.x0 + (bw - w) / 2, y = box.y0 + (bh - h) / 2;
		mask_blit(c, *img, { x, y, x + w, y + h }, c->rect(layer.clip_box), tone(c, color));
	}

	void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
	                     const litehtml::web_color &color) override
	{
		Canvas *c = (Canvas *)hdc;
		/* The root's background covers the whole canvas */
		if (layer.is_root) {
			fill(c, c->clip, color);
			return;
		}
		const litehtml::border_radiuses &br = layer.border_radius;
		const float rad[4] = { f32(br.top_left_x) * S, f32(br.top_right_x) * S, f32(br.bottom_right_x) * S,
		                       f32(br.bottom_left_x) * S };
		if (rad[0] > 0 || rad[1] > 0 || rad[2] > 0 || rad[3] > 0)
			rounded(c, c->rect(layer.border_box), c->rect(layer.clip_box), color, rad);
		else
			fill(c, c->rect(layer.border_box) & c->rect(layer.clip_box), color);
	}

	void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
	                          const litehtml::background_layer::linear_gradient &g) override
	{
		Canvas *c = (Canvas *)hdc;
		Rect r = c->rect(layer.border_box) & c->rect(layer.clip_box) & c->clip;
		if (r.empty() || g.color_points.empty())
			return;
		float sx = g.start.x * S, sy = g.start.y * S - c->y0;
		float dx = g.end.x * S - sx, dy = g.end.y * S - c->y0 - sy, len2 = dx * dx + dy * dy;
		for (int y = r.y0; y < r.y1; y++) {
			uint8_t *d = c->px + ((size_t)y * c->w + r.x0) * 4;
			for (int x = r.x0; x < r.x1; x++, d += 4) {
				float t = len2 > 0 ? ((x + 0.5f - sx) * dx + (y + 0.5f - sy) * dy) / len2 : 0;
				litehtml::web_color col = tone(c, gradient_at(g.color_points, t));
				blend(d, col.red, col.green, col.blue, col.alpha);
			}
		}
	}

	void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
	                          const litehtml::background_layer::radial_gradient &g) override
	{
		if (!g.color_points.empty())
			draw_solid_fill(hdc, layer, gradient_at(g.color_points, 0.5f));
	}

	void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
	                         const litehtml::background_layer::conic_gradient &g) override
	{
		if (!g.color_points.empty())
			draw_solid_fill(hdc, layer, gradient_at(g.color_points, 0.5f));
	}

	void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &b, const litehtml::position &pos,
	                  bool) override
	{
		Canvas *c = (Canvas *)hdc;
		Rect r = c->rect(pos);
		auto width = [](const litehtml::border &s) {
			return f32(s.width) > 0 && s.style > litehtml::border_style_hidden ? std::max(1, dev(f32(s.width))) : 0;
		};
		int t = width(b.top), bt = width(b.bottom), l = width(b.left), rt = width(b.right);
		if (t)
			fill(c, { r.x0, r.y0, r.x1, r.y0 + t }, b.top.color);
		if (bt)
			fill(c, { r.x0, r.y1 - bt, r.x1, r.y1 }, b.bottom.color);
		if (l)
			fill(c, { r.x0, r.y0 + t, r.x0 + l, r.y1 - bt }, b.left.color);
		if (rt)
			fill(c, { r.x1 - rt, r.y0 + t, r.x1, r.y1 - bt }, b.right.color);
	}

	/* Each <title> element, an SVG image's too: find_title() picks the page's */
	void set_caption(const char *) override
	{
	}

	/* The first <title> outside an <svg>, as a browser's document.title */
	void find_title()
	{
		for (auto &el : doc->root()->select_all("title")) {
			auto up = el->parent();
			while (up && strcmp(up->get_tagName(), "svg"))
				up = up->parent();
			if (!up) {
				el->get_text(title);
				return;
			}
		}
	}

	void set_base_url(const char *b) override
	{
		if (b && *b)
			base = resolve_url(url, b);
	}

	void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}

	void on_anchor_click(const char *u, const litehtml::element::ptr &) override
	{
		clicked = u ? u : "";
	}

	void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
	void set_cursor(const char *) override {}

	void transform_text(std::string &text, litehtml::text_transform tt) override
	{
		bool word = true;
		for (char &ch : text) {
			if (ch & 0x80)
				continue;
			if (tt == litehtml::text_transform_uppercase || (tt == litehtml::text_transform_capitalize && word))
				ch = (char)toupper(ch);
			else if (tt == litehtml::text_transform_lowercase)
				ch = (char)tolower(ch);
			word = ch == ' ';
		}
	}

	/* Text into words and spaces, lines broken between them (litehtml's; it has no
	 * word-break): a word longer than LONG_WORD characters (a web address in a message,
	 * nothing to break at) in pieces too, after each slash, dash, dot and the like, and
	 * every PIECE characters when those leave one longer. Laid out whole while it fits,
	 * broken where the line ends otherwise, as overflow-wrap: anywhere would: no page
	 * wider than its window, nor shrunk to it (ui_html_set_fit()), for one word */
	void split_text(const char *text, const std::function<void(const char *)> &on_word,
	                const std::function<void(const char *)> &on_space) override
	{
		litehtml::document_container::split_text(
			text,
			[&](const char *word) {
				size_t chars = 0;
				for (const char *p = word; *p; p++)
					chars += ((unsigned char)*p & 0xC0) != 0x80;
				if (chars <= LONG_WORD) {
					on_word(word);
					return;
				}
				std::string piece;
				size_t piece_chars = 0;
				for (const char *p = word; *p; p++) {
					piece += *p;
					piece_chars += ((unsigned char)*p & 0xC0) != 0x80;
					bool last = ((unsigned char)p[1] & 0xC0) != 0x80; /* of its character */
					if (last && (strchr("/-.?&=_:,;", *p) || piece_chars >= PIECE) && p[1]) {
						on_word(piece.c_str());
						piece.clear();
						piece_chars = 0;
					}
				}
				if (!piece.empty())
					on_word(piece.c_str());
			},
			on_space);
	}

	void import_css(std::string &text, const std::string &u, std::string &baseurl) override
	{
		std::string abs = resolve_url(baseurl.empty() ? base : baseurl, u);
		auto it = css.find(abs);
		if (it == css.end() && read_css) {
			char *t = read_css(abs.c_str(), read_css_user);
			it = css.emplace(abs, t ? t : "").first;
			free(t);
		}
		if (it != css.end())
			text = it->second;
		else
			want(abs, PAGE_CSS);
		baseurl = abs;
	}

	void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &) override
	{
		if (!canvas)
			return;
		clips.push_back(canvas->rect(pos));
		clip_update();
	}

	void del_clip() override
	{
		if (!canvas || clips.empty())
			return;
		clips.pop_back();
		clip_update();
	}

	void get_viewport(litehtml::position &viewport) const override
	{
		viewport = litehtml::position(0, 0, vw, vh);
	}

	litehtml::element::ptr create_element(const char *, const litehtml::string_map &,
	                                      const std::shared_ptr<litehtml::document> &) override
	{
		return nullptr;
	}

	void get_media_features(litehtml::media_features &m) const override
	{
		m.type = litehtml::media_type_screen;
		m.width = m.device_width = vw;
		m.height = m.device_height = vh;
		m.color = 8;
		m.color_index = 0;
		m.monochrome = 0;
		m.resolution = 96 * S;
	}

	void get_language(std::string &language, std::string &culture) const override
	{
		language = "en";
		culture = "";
	}
};

Page *P(struct page *p)
{
	return (Page *)p;
}

} // namespace

extern "C" {

struct page *page_create(const char *html, size_t size, const char *url, int w, int h)
{
	return page_create_local(html, size, url, w, h, nullptr, nullptr, 0);
}

struct page *page_create_local(const char *html, size_t size, const char *url, int w, int h,
                               char *(*read_css)(const char *url, void *user), void *user, int font_px)
{
	Page *p = new Page;
	p->read_css = read_css;
	p->read_css_user = user;
	p->book_px = font_px;
	p->html.assign(html, size);
	p->url = url;
	p->vw = (int)(w / S);
	p->vh = (int)(h / S);
	try {
		p->build();
	} catch (const std::exception &e) {
		printf("browser: %s: %s\n", url, e.what());
		p->doc.reset();
	}
	return (struct page *)p;
}

struct page *page_load(const char *html, size_t size, const char *url, int w, int h)
{
	Page *p = new Page;
	p->html.assign(html, size);
	p->url = url;
	p->vw = (int)(w / S);
	p->vh = (int)(h / S);
	p->want_linked_css();
	return (struct page *)p;
}

bool page_built(struct page *p)
{
	return P(p)->built;
}

void page_destroy(struct page *p)
{
	delete P(p);
}

const char *page_next_wanted(struct page *p, enum page_resource kind)
{
	Page *pg = P(p);
	if (pg->wanted_next[kind] >= pg->wanted[kind].size())
		return NULL;
	return pg->wanted[kind][pg->wanted_next[kind]++].c_str();
}

void page_set_css(struct page *p, const char *url, const char *text)
{
	P(p)->css[url] = text ? text : "";
}

void page_rebuild(struct page *p)
{
	try {
		P(p)->build();
	} catch (const std::exception &e) {
		printf("browser: %s\n", e.what());
		P(p)->doc.reset();
	}
}

void page_set_image(struct page *p, const char *url, uint8_t *bgra, int w, int h, int natural_w, int natural_h)
{
	Image &img = P(p)->images[url];
	free(img.px);
	img = { bgra, w, h, natural_w, natural_h, bgra && line_art(bgra, w, h) };
}

void page_relayout(struct page *p)
{
	P(p)->layout();
}

int page_paginate(struct page *p, int h, int **starts)
{
	std::vector<int> s = P(p)->paginate(h);
	*starts = (int *)malloc(s.size() * sizeof(int));
	if (!*starts)
		return 0;
	memcpy(*starts, s.data(), s.size() * sizeof(int));
	return (int)s.size();
}

void page_timings(struct page *p, unsigned *parse_ms, unsigned *layout_ms)
{
	*parse_ms = P(p)->parse_ms;
	*layout_ms = P(p)->layout_ms;
}

int page_height(struct page *p)
{
	Page *pg = P(p);
	return pg->doc ? (int)std::ceil(f32(pg->doc->height()) * S) : 0;
}

int page_width(struct page *p)
{
	Page *pg = P(p);
	return pg->doc ? (int)std::ceil(f32(pg->doc->width()) * S) : 0;
}

int page_content_width(struct page *p)
{
	return P(p)->content_width();
}

const char *page_title(struct page *p)
{
	return P(p)->title.c_str();
}

const char *page_url(struct page *p)
{
	return P(p)->url.c_str();
}

void page_set_dark(struct page *p, bool dark)
{
	P(p)->dark = dark;
}

void page_set_dark_colors(struct page *p, uint32_t paper, uint32_t ink)
{
	P(p)->paper = paper;
	P(p)->ink = ink;
}

void page_draw(struct page *p, uint8_t *bgra, int w, int h, int y)
{
	try {
		P(p)->draw(bgra, w, h, y);
	} catch (const std::exception &e) {
		printf("browser: %s\n", e.what());
		P(p)->canvas = nullptr;
	}
}

char *page_click(struct page *p, int x, int y)
{
	Page *pg = P(p);
	if (!pg->doc)
		return NULL;
	pg->clicked.clear();
	auto redraw = [](const litehtml::position &) {}; /* boxes to draw again: a tap only follows links */
	float cx = x / S, cy = y / S;
	pg->doc->on_mouse_over(cx, cy, cx, cy, redraw);
	pg->doc->on_lbutton_down(cx, cy, cx, cy, redraw);
	pg->doc->on_lbutton_up(cx, cy, cx, cy, redraw);
	pg->doc->on_mouse_leave(redraw);
	if (pg->clicked.empty())
		return NULL;
	return strdup(resolve_url(pg->base, pg->clicked).c_str());
}

int page_anchor_y(struct page *p, const char *fragment)
{
	Page *pg = P(p);
	if (!pg->doc || !*fragment)
		return -1;
	litehtml::element::ptr el;
	try {
		el = pg->doc->root()->select_one(std::string("#") + fragment);
		if (!el)
			el = pg->doc->root()->select_one(std::string("a[name=\"") + fragment + "\"]");
	} catch (const std::exception &) {
		return -1;
	}
	return el ? dev(f32(el->get_placement().y)) : -1;
}

}
