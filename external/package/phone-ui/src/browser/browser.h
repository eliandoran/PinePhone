/*
 * Browser internals: page.cpp lays out and draws pages with litehtml (C++),
 * browser.c is the app (screen, downloads, tiles). Positions are in device pixels
 * here; litehtml works in CSS pixels, BROWSER_SCALE device pixels each.
 */

#ifndef BROWSER_H
#define BROWSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 720 px wide screen = 480 CSS px, body text (16 CSS px) in the 24 px font */
#define BROWSER_SCALE 1.5f

enum page_resource { PAGE_CSS, PAGE_IMAGE };

struct page;

/* Parses `html` (any encoding, sniffed by litehtml) from `url` and lays it out in
 * a viewport of w x h. Style sheets and images it refers to are listed by
 * page_next_wanted(); the page is laid out without them until they are set. */
struct page *page_create(const char *html, size_t size, const char *url, int w, int h);
void page_destroy(struct page *p);

/* The next style sheet or image (`kind`) to download (absolute URL, valid until the page
 * is destroyed), each once; NULL when there is none */
const char *page_next_wanted(struct page *p, enum page_resource kind);
/* A style sheet that arrived (text NULL: failed); page_rebuild() applies them */
void page_set_css(struct page *p, const char *url, const char *text);
/* Parses the page again with the style sheets set since */
void page_rebuild(struct page *p);
/* An image that arrived, BGRA w x h (malloc'd, the page frees it; NULL: failed),
 * natural_w x natural_h before it was scaled down; page_relayout() applies them */
void page_set_image(struct page *p, const char *url, uint8_t *bgra, int w, int h,
                    int natural_w, int natural_h);
/* Lays the page out again (images change sizes) */
void page_relayout(struct page *p);

int page_height(struct page *p);
const char *page_title(struct page *p); /* "" if none */
const char *page_url(struct page *p);

/* Draws the page's rows y to y + h - 1 into a BGRA buffer w x h (opaque) */
void page_draw(struct page *p, uint8_t *bgra, int w, int h, int y);

/* The absolute URL of the link at (x, y) (malloc'd), NULL if there is none */
char *page_click(struct page *p, int x, int y);
/* Where the element with this id (or <a name>) starts, -1 if there is none */
int page_anchor_y(struct page *p, const char *fragment);

/* `ref` resolved against `base` (malloc'd) */
char *page_resolve(const char *base, const char *ref);

#ifdef __cplusplus
}
#endif

#endif
