/*
 * The account's own repositories (not those it is a member or collaborator of),
 * most recently pushed to first: each row its name and when it was pushed to,
 * its description, then what it is (private, a fork, archived), its language and
 * its stars. Kept between openings and shown while loading again, as the
 * notifications are; forgotten when signing out.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"

#define PAGE "100" /* repositories loaded (GitHub's most per page) */

struct repo {
	char *name, *description; /* description NULL: none */
	char *meta;               /* "Private · C · ★ 12", NULL: nothing to say */
	time_t pushed;
};

static struct {
	struct repo *items;
	unsigned count;
	bool loaded; /* at least once */
	struct ui_fetch fetch;
} r;

/* The list, while the screen is open */
static struct {
	lv_obj_t *list, *card, *empty;
} v;

static time_t parse_time(const char *iso)
{
	struct tm tm = {0};
	if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
		   &tm.tm_sec) != 6)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon--;
	return timegm(&tm);
}

static void clear(void)
{
	for (unsigned i = 0; i < r.count; i++) {
		free(r.items[i].name);
		free(r.items[i].description);
		free(r.items[i].meta);
	}
	free(r.items);
	r.items = NULL;
	r.count = 0;
}

/* `part` after what `meta` has, with a dot between */
static void meta_add(char *meta, size_t size, const char *part)
{
	size_t n = strlen(meta);
	snprintf(meta + n, size - n, "%s%s", n ? " \xc2\xb7 " : "", part);
}

static void parse(const struct json *list)
{
	clear();
	unsigned count = 0;
	for (const struct json *e = list->child; e; e = e->next)
		count++;
	r.items = calloc(count ? count : 1, sizeof(*r.items));
	for (const struct json *e = list->child; e; e = e->next) {
		struct repo *it = &r.items[r.count++];
		it->name = strdup(json_str(json_get(e, "name"), ""));
		const char *description = json_str(json_get(e, "description"), "");
		it->description = description[0] ? strdup(description) : NULL;
		char meta[256] = "";
		if (json_bool(json_get(e, "private")))
			meta_add(meta, sizeof(meta), "Private");
		if (json_bool(json_get(e, "fork")))
			meta_add(meta, sizeof(meta), "Fork");
		if (json_bool(json_get(e, "archived")))
			meta_add(meta, sizeof(meta), "Archived");
		const char *language = json_str(json_get(e, "language"), "");
		if (language[0])
			meta_add(meta, sizeof(meta), language);
		long long stars = json_int(json_get(e, "stargazers_count"), 0);
		if (stars > 0) {
			char s[32];
			snprintf(s, sizeof(s), "\xe2\x98\x85 %lld", stars); /* ★ */
			meta_add(meta, sizeof(meta), s);
		}
		it->meta = meta[0] ? strdup(meta) : NULL;
		it->pushed = parse_time(json_str(json_get(e, "pushed_at"), ""));
	}
}

static lv_obj_t *wrapped_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

/* The rows again, from r.items */
static void rows_update(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.card);
	for (unsigned i = 0; i < r.count; i++) {
		const struct repo *it = &r.items[i];
		lv_obj_t *row = ui_row_create(v.card, LV_SIZE_CONTENT, i + 1 < r.count, NULL, NULL);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_t *text = ui_row_text(row);

		lv_obj_t *top = ui_row_line(text);
		lv_obj_t *name = ui_one_line_label(top, "", &ui_font_24);
		ui_set_text(name, it->name);
		lv_obj_set_flex_grow(name, 1);
		char when[32] = "";
		if (it->pushed)
			ui_when_format(when, sizeof(when), it->pushed);
		lv_obj_t *time = ui_dim_label(top, "", &ui_font_20);
		ui_set_text(time, when);

		if (it->description)
			wrapped_label(text, it->description, &ui_font_20, UI_COLOR_DIM);
		if (it->meta)
			wrapped_label(text, it->meta, &ui_font_20, UI_COLOR_DIM);
	}
	lv_obj_set_hidden(v.card, !r.count);
}

static void status(const char *text)
{
	if (!v.list)
		return;
	if (text)
		ui_set_text(v.empty, text);
	lv_obj_set_hidden(v.empty, !text);
}

static void loaded(const char *data, long code)
{
	struct json *list = data && code == 200 ? json_parse(data) : NULL;
	if (code == 401) {
		github_sign_out("GitHub refused this token.");
	} else if (list && list->kind == JSON_ARRAY) {
		r.loaded = true;
		parse(list);
		rows_update();
		status(r.count ? NULL : "No repositories");
	} else if (!code) {
		/* What was loaded before stays */
		if (!r.loaded || !r.count)
			status("Cannot reach GitHub");
	} else {
		char text[64];
		snprintf(text, sizeof(text), "GitHub answered with an error (HTTP %ld)", code);
		if (!r.loaded || !r.count)
			status(text);
	}
	json_free(list);
}

void repos_load(void)
{
	if (!r.loaded)
		status("Loading...");
	api_request(&r.fetch, "GET", "/user/repos?affiliation=owner&sort=pushed&per_page=" PAGE, NULL, loaded);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* What it loads would have nowhere to go */
	ui_fetch_stop(&r.fetch);
	memset(&v, 0, sizeof(v));
}

void repos_create(lv_obj_t *body)
{
	v.list = lv_obj_create(body);
	lv_obj_remove_style_all(v.list);
	lv_obj_set_width(v.list, LV_PCT(100));
	lv_obj_set_flex_grow(v.list, 1);
	lv_obj_set_flex_flow(v.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(v.list, 16, 0);
	lv_obj_set_scroll_dir(v.list, LV_DIR_VER);
	lv_obj_add_event_cb(v.list, delete_cb, LV_EVENT_DELETE, NULL);

	v.empty = ui_empty_label(v.list, "");
	lv_obj_set_hidden(v.empty, true);
	v.card = ui_card_create(v.list);
	lv_obj_set_hidden(v.card, true);
	rows_update();
}

void repos_show(bool shown)
{
	if (v.list)
		lv_obj_set_hidden(v.list, !shown);
}

void repos_forget_account(void)
{
	ui_fetch_stop(&r.fetch);
	clear();
	r.loaded = false;
	rows_update();
	status(NULL);
}
