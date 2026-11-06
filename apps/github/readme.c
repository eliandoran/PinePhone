/*
 * A repository's README on a page of its own, from the repository's page: its
 * Markdown (asked for raw) as widgets, as comments are (markdown.c), on a card the
 * page's width. When it has what the widgets do not show (HTML: centered logos,
 * badges), GitHub's rendering of it is asked for too and laid out as a page, its
 * relative images and links from the repository's default branch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "github.h"

#define RAW "application/vnd.github.raw+json"
#define HTML "application/vnd.github.html+json"
#define MAX_HTML_H 16384 /* a README as a page: cut there (32 tiles of textures) */

static struct {
	lv_obj_t *scr, *list, *status;
	char path[256]; /* "/repos/o/r/readme" */
	char base[256]; /* "https://github.com/o/r/raw/HEAD/": its relative addresses */
	char *text;     /* the Markdown, while its HTML loads */
	struct ui_fetch fetch;
} r;

static void status(const char *text)
{
	ui_set_text(r.status, text);
	lv_obj_set_hidden(r.status, false);
}

/* What went wrong, true if anything did; a refused token signs out */
static bool failed(long code)
{
	if (code == 200)
		return false;
	if (code == 401) {
		/* This page and the repository's, back to the form */
		ui_pop();
		ui_pop();
		github_sign_out("GitHub refused this token.");
	} else if (code == 404) {
		status("No README");
	} else if (!code) {
		status("Cannot reach GitHub");
	} else {
		char text[64];
		snprintf(text, sizeof(text), "GitHub answered with an error (HTTP %ld)", code);
		status(text);
	}
	return true;
}

static void show(const char *text, const char *html)
{
	lv_obj_set_hidden(r.status, true);
	lv_obj_t *card = ui_card_create(r.list);
	lv_obj_set_style_pad_all(card, 24, 0);
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 2 * 24;
	markdown_create(card, w, text, html, r.base, MAX_HTML_H);
}

static void html_loaded(const char *data, long code)
{
	if (code == 401) {
		failed(code);
		return;
	}
	/* Without it, the Markdown as the widgets show it */
	show(r.text, code == 200 && data ? data : NULL);
	free(r.text);
	r.text = NULL;
}

static void raw_loaded(const char *data, long code)
{
	if (failed(code))
		return;
	if (!data)
		data = "";
	if (markdown_needs_html(data)) {
		r.text = strdup(data);
		if (r.text) {
			api_request_as(&r.fetch, HTML, "GET", r.path, NULL, html_loaded);
			return;
		}
	}
	show(data, NULL);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&r.fetch);
	free(r.text);
	memset(&r, 0, sizeof(r));
}

void readme_open(const char *repo)
{
	if (r.scr)
		return;
	lv_obj_t *body;
	r.scr = ui_app_screen_create("README", NULL, &body);
	lv_obj_add_event_cb(r.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	r.list = lv_obj_create(body);
	lv_obj_remove_style_all(r.list);
	lv_obj_set_width(r.list, LV_PCT(100));
	lv_obj_set_flex_grow(r.list, 1);
	lv_obj_set_flex_flow(r.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(r.list, 16, 0);
	lv_obj_set_scroll_dir(r.list, LV_DIR_VER);
	r.status = ui_empty_label(r.list, "Loading...");

	snprintf(r.path, sizeof(r.path), "/repos/%s/readme", repo);
	snprintf(r.base, sizeof(r.base), "https://github.com/%s/raw/HEAD/", repo);
	api_request_as(&r.fetch, RAW, "GET", r.path, NULL, raw_loaded);
	ui_push(r.scr);
}
