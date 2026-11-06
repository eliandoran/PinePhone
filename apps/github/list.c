/*
 * A repository's open issues or pull requests, newest first, PER_PAGE a page, from
 * GitHub's search (is:issue or is:pr): its total gives the number of pages, which
 * GitHub's plain lists only say in a header. A bar at the bottom goes to a page:
 * first, previous, NUMBERS page numbers around the one shown (selected), next, last;
 * those leading nowhere dimmed, the bar hidden for one page. The search serves
 * MAX_RESULTS at most: no pages beyond. The title says how many there are (all of
 * them, once known). Each row: its number, author (a pull request's Draft) and when
 * it was opened, then its title; tapping one opens it (pr.c).
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"
#include "ui_symbols.h"

#define PER_PAGE 30
#define MAX_RESULTS 1000 /* GitHub's search, whatever the total */
#define NUMBERS 5        /* page numbers in the bar */
#define BUTTON_W 68

/* A row's, freed with it */
struct row {
	long long number;
	char *title;
};

static struct {
	lv_obj_t *scr, *list, *status, *card;
	lv_obj_t *title; /* the header's */
	lv_obj_t *bar, *first, *prev, *next, *last, *numbers[NUMBERS];
	bool pulls;
	char repo[160]; /* "o/r" */
	int page;       /* the one shown or asked for, from 1 */
	int pages;      /* how many, 0 until known */
	char title_text[64]; /* "42 issues" */
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

static void row_add(const struct json *j, bool divider)
{
	struct row *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->number = json_int(json_get(j, "number"), 0);
	r->title = strdup(json_str(json_get(j, "title"), ""));
	lv_obj_t *row = ui_row_create(l.card, LV_SIZE_CONTENT, divider, row_cb, r);
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

static void enable(lv_obj_t *button, bool on)
{
	lv_obj_set_state(button, LV_STATE_DISABLED, !on);
}

/* The bar for l.page of l.pages: the numbers around it, the arrows that lead somewhere */
static void bar_update(void)
{
	lv_obj_set_hidden(l.bar, l.pages <= 1);
	if (l.pages <= 1)
		return;
	int count = l.pages < NUMBERS ? l.pages : NUMBERS;
	int start = l.page - NUMBERS / 2;
	if (start > l.pages - count + 1)
		start = l.pages - count + 1;
	if (start < 1)
		start = 1;
	for (int i = 0; i < NUMBERS; i++) {
		lv_obj_t *b = l.numbers[i];
		lv_obj_set_hidden(b, i >= count);
		lv_obj_set_user_data(b, (void *)(intptr_t)(start + i));
		lv_label_set_text_fmt(lv_obj_get_child(b, 0), "%d", start + i);
		lv_obj_set_state(b, LV_STATE_CHECKED, start + i == l.page);
	}
	enable(l.first, l.page > 1);
	enable(l.prev, l.page > 1);
	enable(l.next, l.page < l.pages);
	enable(l.last, l.page < l.pages);
}

static void status(const char *text)
{
	ui_set_text(l.status, text);
	lv_obj_set_hidden(l.status, false);
}

static void loaded(const char *data, long code)
{
	if (code == 401) {
		github_refused();
		return;
	}
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	const struct json *items = json_get(j, "items");
	if (!items || items->kind != JSON_ARRAY) {
		if (!code) {
			status(_("Cannot reach GitHub"));
		} else {
			char text[64];
			snprintf(text, sizeof(text), _("GitHub answered with an error (HTTP %ld)"), code);
			status(text);
		}
		json_free(j);
		return;
	}
	long long total = json_int(json_get(j, "total_count"), 0);
	if (total > 0) {
		snprintf(l.title_text, sizeof(l.title_text),
			 l.pulls ? P_("%lld pull request", "%lld pull requests", total)
				 : P_("%lld issue", "%lld issues", total),
			 total);
		lv_label_set_text_static(l.title, l.title_text);
	}
	if (total > MAX_RESULTS)
		total = MAX_RESULTS;
	l.pages = (int)((total + PER_PAGE - 1) / PER_PAGE);
	for (const struct json *e = items->child; e; e = e->next)
		row_add(e, e->next);
	json_free(j);
	bool any = lv_obj_get_child_count(l.card);
	lv_obj_set_hidden(l.card, !any);
	if (any)
		lv_obj_set_hidden(l.status, true);
	else
		status(l.pulls ? _("No open pull requests") : _("No open issues"));
	bar_update();
}

/* Page `page`: the rows that were there go, the list from its top */
static void go(int page)
{
	l.page = page;
	lv_obj_clean(l.card);
	lv_obj_set_hidden(l.card, true);
	status(_("Loading..."));
	lv_obj_scroll_to_y(l.list, 0, LV_ANIM_OFF);
	bar_update();
	char path[320];
	snprintf(path, sizeof(path),
		 "/search/issues?q=repo:%s+is:%s+is:open&sort=created&order=desc&per_page=%d&page=%d", /* NON-NLS */
		 l.repo, l.pulls ? "pr" : "issue", PER_PAGE, page);
	api_request(&l.fetch, "GET", path, NULL, loaded);
}

/* user_data: the page, or -1 first, -2 previous, -3 next, -4 last */
static void page_cb(lv_event_t *e)
{
	lv_obj_t *b = lv_event_get_current_target_obj(e);
	int to = (int)(intptr_t)lv_obj_get_user_data(b);
	if (to == -1)
		to = 1;
	else if (to == -2)
		to = l.page - 1;
	else if (to == -3)
		to = l.page + 1;
	else if (to == -4)
		to = l.pages;
	if (to >= 1 && to <= l.pages)
		go(to);
}

static lv_obj_t *page_button(const char *text, intptr_t to)
{
	lv_obj_t *b = ui_button(l.bar, BUTTON_W, 72, UI_COLOR_CARD, UI_COLOR_PRESSED, text, page_cb, NULL);
	lv_obj_set_user_data(b, (void *)to);
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &ui_font_24, 0);
	/* The page shown */
	lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
	/* Leading nowhere */
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	return b;
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
	lv_obj_t *header, *body;
	l.scr = ui_app_screen_create(pulls ? _("Pull requests") : _("Issues"), &header, &body);
	/* After the back button: how many, once known */
	l.title = lv_obj_get_child(header, 1);
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
	lv_obj_set_scroll_dir(l.list, LV_DIR_VER);
	l.status = ui_empty_label(l.list, "");
	l.card = ui_card_create(l.list);

	/* The bar, under the list */
	l.bar = ui_box(body);
	lv_obj_set_size(l.bar, LV_PCT(100), 112);
	lv_obj_set_flex_flow(l.bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(l.bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_bg_opa(l.bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(l.bar, lv_color_hex(UI_COLOR_BAR), 0);
	l.first = page_button(UI_SYMBOL_ANGLES_LEFT, -1);
	l.prev = page_button(LV_SYMBOL_LEFT, -2);
	for (int i = 0; i < NUMBERS; i++)
		l.numbers[i] = page_button("", 0);
	l.next = page_button(LV_SYMBOL_RIGHT, -3);
	l.last = page_button(UI_SYMBOL_ANGLES_RIGHT, -4);

	go(1);
	ui_push(l.scr);
}
