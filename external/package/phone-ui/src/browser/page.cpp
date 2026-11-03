/*
 * The browser's litehtml container: litehtml parses the page, applies its CSS and
 * lays it out; this draws it on the CPU into BGRA buffers (the tiles browser.c
 * uploads as textures), with the UI's bitmap fonts at the size nearest to the one
 * asked for. Bold is each glyph drawn twice 1 px apart, italic the glyphs sheared.
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
}

namespace {

constexpr float S = BROWSER_SCALE;

const lv_font_t *const fonts[] = { &ui_font_16, &ui_font_20, &ui_font_24,
                                   &ui_font_26, &ui_font_32, &ui_font_48 };
const int font_sizes[] = { 16, 20, 24, 26, 32, 48 };
constexpr int FONTS = sizeof(font_sizes) / sizeof(font_sizes[0]);

float f32(litehtml::pixel_t v)
{
	return (float)v;
}

/* CSS to device pixels */
int dev(float v)
{
	return (int)std::lround(v * S);
}

struct Rect {
	int x0, y0, x1, y1; /* x1 and y1 excluded */
	bool empty() const { return x0 >= x1 || y0 >= y1; }
	Rect operator&(const Rect &o) const
	{
		return { std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1) };
	}
};

/* A tile being drawn: rows y0 to y0 + h - 1 of the page */
struct Canvas {
	uint8_t *px;
	int w, h, y0;
	Rect clip; /* the tile and the clips litehtml set */

	Rect rect(const litehtml::position &p) const
	{
		return { dev(f32(p.x)), dev(f32(p.y)) - y0, dev(f32(p.x) + f32(p.width)),
		         dev(f32(p.y) + f32(p.height)) - y0 };
	}
};

/* Glyphs: A8 bitmaps from LVGL's fonts, kept for good (a few hundred per size) */

struct Glyph {
	int adv = 0, w = 0, h = 0, ofs_x = 0, ofs_y = 0;
	std::vector<uint8_t> alpha;
};

std::unordered_map<uint64_t, Glyph> glyphs;

const Glyph &glyph_get(int font, uint32_t cp)
{
	uint64_t key = (uint64_t)font << 32 | cp;
	auto it = glyphs.find(key);
	if (it != glyphs.end())
		return it->second;
	Glyph &g = glyphs[key];
	const lv_font_t *f = fonts[font];
	lv_font_glyph_dsc_t d;
	memset(&d, 0, sizeof(d));
	if (!lv_font_get_glyph_dsc(f, &d, cp, 0)) {
		g.adv = cp == ' ' ? font_sizes[font] / 4 : font_sizes[font] / 2;
		return g;
	}
	g.adv = d.adv_w;
	/* Bitmaps only, not the emoji images in the fallback chain */
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

int text_measure(const Font &f, const char *text)
{
	int w = 0;
	for (const char *s = text; *s;) {
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

void fill(Canvas *c, Rect r, const litehtml::web_color &col)
{
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
void disc(Canvas *c, Rect r, const litehtml::web_color &col, float ring)
{
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

void glyph_blit(Canvas *c, const Glyph &g, int gx, int gy, int base, const litehtml::web_color &col,
                bool italic)
{
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

struct Image {
	uint8_t *px = nullptr; /* BGRA, NULL: failed */
	int w = 0, h = 0, natural_w = 0, natural_h = 0;
};

/* An image scaled into dst, drawn where it meets `clip` */
void image_blit(Canvas *c, const Image &img, Rect dst, Rect clip)
{
	Rect r = dst & clip & c->clip;
	int dw = dst.x1 - dst.x0, dh = dst.y1 - dst.y0;
	if (r.empty() || dw <= 0 || dh <= 0)
		return;
	for (int y = r.y0; y < r.y1; y++) {
		int sy = (int)((int64_t)(y - dst.y0) * img.h / dh);
		const uint8_t *src = img.px + (size_t)sy * img.w * 4;
		uint8_t *d = c->px + ((size_t)y * c->w + r.x0) * 4;
		for (int x = r.x0; x < r.x1; x++, d += 4) {
			const uint8_t *s = src + (int64_t)(x - dst.x0) * img.w / dw * 4;
			if (s[3])
				blend(d, s[2], s[1], s[0], s[3]);
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
	std::map<std::string, Image> images;
	std::deque<std::string> wanted[2]; /* per page_resource; deque: c_str() stays valid */
	size_t wanted_next[2] = { 0, 0 };
	std::set<std::string> requested;
	Canvas *canvas = nullptr;
	std::vector<Rect> clips;

	~Page()
	{
		doc.reset(); /* deletes its fonts through delete_font() */
		for (auto &i : images)
			free(i.second.px);
	}

	void build()
	{
		title.clear();
		base = url;
		doc = litehtml::document::createFromString(litehtml::estring(html), this);
		if (doc)
			doc->render(vw);
	}

	void want(const std::string &u, page_resource kind)
	{
		if (u.compare(0, 5, "data:") && u.compare(0, 4, "http"))
			return;
		if (requested.insert(u).second)
			wanted[kind].push_back(u);
	}

	void draw(uint8_t *bgra, int w, int h, int y)
	{
		memset(bgra, 0xff, (size_t)w * h * 4); /* white, opaque */
		Canvas c = { bgra, w, h, y, { 0, 0, w, h } };
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
		float want = f32(d.size) * S;
		int best = 0;
		for (int i = 1; i < FONTS; i++)
			if (std::fabs(font_sizes[i] - want) < std::fabs(font_sizes[best] - want))
				best = i;
		const lv_font_t *lv = fonts[best];
		Font *f = new Font;
		f->index = best;
		f->ascent = lv->line_height - lv->base_line;
		f->descent = lv->base_line;
		f->x_height = glyph_get(best, 'x').h;
		f->line = std::max(1, font_sizes[best] / 16);
		f->bold = d.weight >= 600;
		f->italic = d.style == litehtml::font_style_italic;
		f->decoration = d.decoration_line;
		f->decoration_color = d.decoration_color;
		if (fm) {
			fm->font_size = d.size;
			fm->height = lv->line_height / S;
			fm->ascent = f->ascent / S;
			fm->descent = f->descent / S;
			fm->x_height = f->x_height / S;
			fm->ch_width = glyph_get(best, '0').adv / S;
			fm->draw_spaces = f->decoration != 0;
			fm->sub_shift = f32(d.size) / 5;
			fm->super_shift = f32(d.size) / 3;
		}
		return (litehtml::uint_ptr)f;
	}

	void delete_font(litehtml::uint_ptr font) override
	{
		delete (Font *)font;
	}

	litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr font) override
	{
		return text_measure(*(Font *)font, text) / S;
	}

	void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr font, litehtml::web_color color,
	               const litehtml::position &pos) override
	{
		Canvas *c = (Canvas *)hdc;
		const Font &f = *(Font *)font;
		int x = dev(f32(pos.x)), top = dev(f32(pos.y)) - c->y0, base = top + f.ascent;
		if (top >= c->clip.y1 || base + f.descent <= c->clip.y0)
			return;
		int x0 = x;
		for (const char *s = text; *s;) {
			uint32_t cp = substitute(utf8_next(s));
			if (!cp)
				continue;
			const Glyph &g = glyph_get(f.index, cp);
			if (!g.alpha.empty()) {
				int gy = base - g.ofs_y - g.h;
				glyph_blit(c, g, x + g.ofs_x, gy, base, color, f.italic);
				if (f.bold)
					glyph_blit(c, g, x + g.ofs_x + 1, gy, base, color, f.italic);
			}
			x += g.adv + f.bold;
		}
		if (!f.decoration)
			return;
		const litehtml::web_color &dc = f.decoration_color.is_current_color ? color : f.decoration_color;
		if (f.decoration & litehtml::text_decoration_line_underline)
			fill(c, { x0, base + f.line + 1, x, base + 2 * f.line + 1 }, dc);
		if (f.decoration & litehtml::text_decoration_line_line_through)
			fill(c, { x0, base - f.x_height / 2, x, base - f.x_height / 2 + f.line }, dc);
		if (f.decoration & litehtml::text_decoration_line_overline)
			fill(c, { x0, top, x, top + f.line }, dc);
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

	void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
	                     const litehtml::web_color &color) override
	{
		Canvas *c = (Canvas *)hdc;
		/* The root's background covers the whole canvas */
		fill(c, layer.is_root ? c->clip : c->rect(layer.border_box) & c->rect(layer.clip_box), color);
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
				litehtml::web_color col = gradient_at(g.color_points, t);
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

	void set_caption(const char *caption) override
	{
		title = caption ? caption : "";
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

	void transform_text(litehtml::string &text, litehtml::text_transform tt) override
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

	void import_css(litehtml::string &text, const litehtml::string &u, litehtml::string &baseurl) override
	{
		std::string abs = resolve_url(baseurl.empty() ? base : baseurl, u);
		auto it = css.find(abs);
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

	void get_language(litehtml::string &language, litehtml::string &culture) const override
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
	Page *p = new Page;
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
	img = { bgra, w, h, natural_w, natural_h };
}

void page_relayout(struct page *p)
{
	Page *pg = P(p);
	if (pg->doc)
		pg->doc->render(pg->vw);
}

int page_height(struct page *p)
{
	Page *pg = P(p);
	return pg->doc ? (int)std::ceil(f32(pg->doc->height()) * S) : 0;
}

const char *page_title(struct page *p)
{
	return P(p)->title.c_str();
}

const char *page_url(struct page *p)
{
	return P(p)->url.c_str();
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
	litehtml::position::vector boxes;
	float cx = x / S, cy = y / S;
	pg->doc->on_mouse_over(cx, cy, cx, cy, boxes);
	pg->doc->on_lbutton_down(cx, cy, cx, cy, boxes);
	pg->doc->on_lbutton_up(cx, cy, cx, cy, boxes);
	pg->doc->on_mouse_leave(boxes);
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

char *page_resolve(const char *base, const char *ref)
{
	return strdup(resolve_url(base, ref).c_str());
}

}
