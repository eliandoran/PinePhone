/*
 * A repository's open issues or pull requests, newest first (GitHub's order), a
 * page of PER_PAGE at a time: Load more at the end asks for the next one, until a
 * page comes short. Each row: its number, author (a pull request's Draft) and when
 * it was opened, then its title; tapping one opens it (pr.c). GitHub's issues
 * include its pull requests: the issues' list leaves them out.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"

#define PER_PAGE 30

/* A row's, freed with it */
struct row {
	long long number;
	char *title;
};

static struct {
	lv_obj_t *scr, *list, *status, *card, *error, *more;
	bool pulls;
	char repo[160]; /* "o/r" */
	int page;       /* the last one loaded, 0: none yet */
	struct ui_fetch fetch;
} l;

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

static void row_cb(lv_event_t *e)
{
	const struct row *r = lv_event_get_user_data(e);
	char path[224];
	snprintf(path, sizeof(path), "/repos/%s/%s/%lld", l.repo, l.pulls ? "pulls" : "issues", r->number);
	(l.pulls ? pr_open : issue_open)(path, l.repo, r->title);
}

static void row_delete_cb(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);
	free(r->title);
	free(r);
}

static void row_add(const struct json *j)
{
	struct row *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->number = json_int(json_get(j, "number"), 0);
	r->title = strdup(json_str(json_get(j, "title"), ""));
	if (lv_obj_get_child_count(l.card))
		ui_row_set_divider(lv_obj_get_child(l.card, -1), true);
	lv_obj_t *row = ui_row_create(l.card, LV_SIZE_CONTENT, false, row_cb, r);
	lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, r);
	lv_obj_set_style_pad_ver(row, 20, 0);
	lv_obj_t *text = ui_row_text(row);

	lv_obj_t *top = ui_row_line(text);
	lv_obj_t *meta = ui_one_line_label(top, "", &ui_font_20);
	const char *login = json_str(json_get(json_get(j, "user"), "login"), "");
	if (json_bool(json_get(j, "draft")))
		ui_set_text_fmt(meta, "#%lld \xc2\xb7 %s \xc2\xb7 %s", r->number, login, _("Draft"));
	else
		ui_set_text_fmt(meta, "#%lld \xc2\xb7 %s", r->number, login);
	lv_obj_set_style_text_color(meta, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_flex_grow(meta, 1);
	char when[32] = "";
	time_t created = parse_time(json_str(json_get(j, "created_at"), ""));
	if (created)
		ui_when_format(when, sizeof(when), created);
	lv_obj_t *time = ui_dim_label(top, "", &ui_font_20);
	ui_set_text(time, when);

	lv_obj_t *title = lv_label_create(text);
	lv_label_set_text(title, r->title);
	lv_obj_set_width(title, LV_PCT(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(title, &ui_font_24, 0);
}

static void more_text(const char *text)
{
	lv_label_set_text_static(lv_obj_get_child(l.more, 0), text);
}

/* What went wrong: in place of the list for the first page, above the button for
 * the next ones */
static void failed(long code)
{
	char text[64];
	if (!code)
		snprintf(text, sizeof(text), "%s", _("Cannot reach GitHub"));
	else
		snprintf(text, sizeof(text), _("GitHub answered with an error (HTTP %ld)"), code);
	if (!l.page) {
		ui_set_text(l.status, text);
		return;
	}
	ui_set_text(l.error, text);
	lv_obj_set_hidden(l.error, false);
	more_text(_("Load more"));
}

static void loaded(const char *data, long code)
{
	if (code == 401) {
		github_refused();
		return;
	}
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	if (!j || j->kind != JSON_ARRAY) {
		failed(code);
		json_free(j);
		return;
	}
	unsigned count = 0;
	for (const struct json *e = j->child; e; e = e->next) {
		count++;
		if (!l.pulls && json_get(e, "pull_request"))
			continue;
		row_add(e);
	}
	json_free(j);
	l.page++;
	bool any = lv_obj_get_child_count(l.card);
	lv_obj_set_hidden(l.status, any || count == PER_PAGE);
	if (!any)
		ui_set_text(l.status, l.pulls ? _("No open pull requests") : _("No open issues"));
	lv_obj_set_hidden(l.card, !any);
	lv_obj_set_hidden(l.error, true);
	/* A full page: there may be more */
	lv_obj_set_hidden(l.more, count < PER_PAGE);
	more_text(_("Load more"));
}

static void load(void)
{
	char path[256];
	snprintf(path, sizeof(path), "/repos/%s/%s?state=open&per_page=%d&page=%d", l.repo,
		 l.pulls ? "pulls" : "issues", PER_PAGE, l.page + 1);
	api_request(&l.fetch, "GET", path, NULL, loaded);
}

static void more_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (l.fetch.curl)
		return;
	more_text(_("Loading..."));
	load();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&l.fetch);
	memset(&l, 0, sizeof(l));
}

void list_open(const char *repo, bool pulls)
{
	if (l.scr)
		return;
	lv_obj_t *body;
	l.scr = ui_app_screen_create(pulls ? _("Pull requests") : _("Issues"), NULL, &body);
	lv_obj_add_event_cb(l.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);
	l.pulls = pulls;
	snprintf(l.repo, sizeof(l.repo), "%s", repo);

	l.list = lv_obj_create(body);
	lv_obj_remove_style_all(l.list);
	lv_obj_set_width(l.list, LV_PCT(100));
	lv_obj_set_flex_grow(l.list, 1);
	lv_obj_set_flex_flow(l.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(l.list, 16, 0);
	lv_obj_set_style_pad_row(l.list, 16, 0);
	lv_obj_set_scroll_dir(l.list, LV_DIR_VER);

	l.status = ui_empty_label(l.list, _("Loading..."));
	l.card = ui_card_create(l.list);
	lv_obj_set_hidden(l.card, true);
	l.error = ui_empty_label(l.list, "");
	lv_obj_set_hidden(l.error, true);
	l.more = ui_button(l.list, LV_PCT(100), 88, UI_COLOR_CARD, UI_COLOR_PRESSED, _("Load more"), more_cb, NULL);
	lv_obj_set_hidden(l.more, true);

	load();
	ui_push(l.scr);
}
