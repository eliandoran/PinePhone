/*
 * A repository's issues or pull requests, open or closed (chosen at the top),
 * newest first, PER_PAGE a page, from GitHub's search (is:issue or is:pr, is:open
 * or is:closed): its total gives the number of pages, which GitHub's plain lists
 * only say in a header. A bar at the bottom goes to a page: first, previous, NUMBERS
 * page numbers around the one shown (selected), next, last; those leading nowhere
 * dimmed, the bar hidden for one page. The search serves MAX_RESULTS at most: no
 * pages beyond. The title says how many there are (all of them, once known). Each
 * row: its number, author, a tag (a pull request's Draft; Merged, Not planned or
 * Duplicate for a closed one) and when it was opened, then its title; tapping one
 * opens it (pr.c). The header's refresh button loads the page shown again; one merged
 * meanwhile (pr.c) says so in its row, as the search may still give it as open.
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
	char *title, *login;
	lv_obj_t *meta; /* "#7 · elian · Draft" */
	lv_obj_t *checks; /* a pull request's icon, once known */
};

static struct {
	lv_obj_t *scr, *list, *status, *card;
	lv_obj_t *title; /* the header's */
	lv_obj_t *filters[2]; /* Open, Closed */
	lv_obj_t *bar, *first, *prev, *next, *last, *numbers[NUMBERS];
	bool pulls;
	bool closed;     /* the closed ones, not the open ones */
	char repo[160]; /* "o/r" */
	int page;       /* the one shown or asked for, from 1 */
	int pages;      /* how many, 0 until known */
	char title_text[64]; /* "42 issues" */
	struct row *rows[PER_PAGE]; /* the page's, for their checks */
	unsigned row_count;
	struct ui_fetch fetch;
	struct ui_fetch checks_fetch;
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
	free(r->login);
	free(r);
}

/* Its number, author and tag (NULL: none) */
static void meta_set(struct row *r, const char *tag)
{
	if (!r->login)
		return;
	if (tag)
		ui_set_text_fmt(r->meta, "#%lld \xc2\xb7 %s \xc2\xb7 %s", r->number, r->login, tag);
	else
		ui_set_text_fmt(r->meta, "#%lld \xc2\xb7 %s", r->number, r->login);
}

static void row_add(const struct json *j, bool divider)
{
	struct row *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->number = json_int(json_get(j, "number"), 0);
	r->title = strdup(json_str(json_get(j, "title"), ""));
	r->login = strdup(json_str(json_get(json_get(j, "user"), "login"), ""));
	lv_obj_t *row = ui_row_create(l.card, LV_SIZE_CONTENT, divider, row_cb, r);
	lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, r);
	lv_obj_set_style_pad_ver(row, 20, 0);
	lv_obj_t *text = ui_row_text(row);

	lv_obj_t *top = ui_row_line(text);
	r->meta = ui_one_line_label(top, "", UI_FONT_CAPTION);
	const char *reason = json_str(json_get(j, "state_reason"), "");
	const char *tag = NULL;
	if (json_bool(json_get(j, "draft")) && !l.closed)
		tag = _("Draft");
	else if (l.pulls && json_str(json_get(json_get(j, "pull_request"), "merged_at"), NULL))
		tag = _("Merged");
	else if (!strcmp(reason, "not_planned")) /* NON-NLS */
		tag = _("Not planned");
	else if (!strcmp(reason, "duplicate")) /* NON-NLS */
		tag = _("Duplicate");
	meta_set(r, tag);
	lv_obj_set_style_text_color(r->meta, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_flex_grow(r->meta, 1);
	char when[32] = "";
	time_t created = parse_time(json_str(json_get(j, "created_at"), ""));
	if (created)
		ui_when_format(when, sizeof(when), created);
	lv_obj_t *time = ui_dim_label(top, "", UI_FONT_CAPTION);
	ui_set_text(time, when);
	r->checks = lv_label_create(top);
	check_icon_set(r->checks, CHECK_NONE);
	lv_obj_set_style_text_font(r->checks, &ui_font_20, 0);
	if (l.row_count < PER_PAGE)
		l.rows[l.row_count++] = r;

	lv_obj_t *title = lv_label_create(text);
	lv_label_set_text(title, r->title);
	lv_obj_set_width(title, LV_PCT(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
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

/* NON-NLS-BEGIN: GraphQL: the rollup of each pull request's last commit, aliased by its
 * number (p7) */
#define CHECKS_QUERY_START                                                                   \
	"query($owner: String!, $name: String!) { repository(owner: $owner, name: $name) {"
#define CHECKS_QUERY_PR                                                                      \
	" p%lld: pullRequest(number: %lld) { commits(last: 1) { nodes { commit {"            \
	" statusCheckRollup { state } } } } }"
/* NON-NLS-END */

static void checks_loaded(const char *data, long code)
{
	if (code == 401) {
		github_refused();
		return;
	}
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	char error[128];
	const struct json *repo = json_get(j ? graphql_data(j, error, sizeof(error)) : NULL, "repository");
	for (unsigned i = 0; repo && i < l.row_count; i++) {
		char key[32];
		snprintf(key, sizeof(key), "p%lld", l.rows[i]->number);
		const struct json *nodes = json_get(json_get(json_get(repo, key), "commits"), "nodes");
		const struct json *commit = json_get(nodes ? nodes->child : NULL, "commit");
		check_icon_set(l.rows[i]->checks, check_state(json_get(commit, "statusCheckRollup")));
	}
	json_free(j);
}

/* The checks of the page's pull requests, in one request */
static void checks_request(void)
{
	size_t size = sizeof(CHECKS_QUERY_START) + l.row_count * (sizeof(CHECKS_QUERY_PR) + 40) + 8;
	char *query = malloc(size);
	if (!query)
		return;
	size_t n = snprintf(query, size, "%s", CHECKS_QUERY_START);
	for (unsigned i = 0; i < l.row_count; i++)
		n += snprintf(query + n, size - n, CHECKS_QUERY_PR, l.rows[i]->number, l.rows[i]->number);
	snprintf(query + n, size - n, " } }");
	char owner[sizeof(l.repo)], variables[sizeof(l.repo) + 48];
	snprintf(owner, sizeof(owner), "%s", l.repo);
	char *name = strchr(owner, '/');
	if (name) {
		*name++ = '\0';
		snprintf(variables, sizeof(variables), "{\"owner\":\"%s\",\"name\":\"%s\"}", owner, name);
		graphql(&l.checks_fetch, query, variables, checks_loaded);
	}
	free(query);
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
	} else {
		lv_label_set_text_static(l.title, l.pulls ? _("Pull requests") : _("Issues"));
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
		status(l.closed ? (l.pulls ? _("No closed pull requests") : _("No closed issues"))
				 : (l.pulls ? _("No open pull requests") : _("No open issues")));
	if (l.pulls && l.row_count)
		checks_request();
	bar_update();
}

/* Page `page`: the rows that were there go, the list from its top */
static void go(int page)
{
	l.page = page;
	ui_fetch_stop(&l.checks_fetch);
	l.row_count = 0;
	lv_obj_clean(l.card);
	lv_obj_set_hidden(l.card, true);
	status(_("Loading..."));
	lv_obj_scroll_to_y(l.list, 0, LV_ANIM_OFF);
	bar_update();
	char path[320];
	snprintf(path, sizeof(path),
		 "/search/issues?q=repo:%s+is:%s+is:%s&sort=created&order=desc&per_page=%d&page=%d", /* NON-NLS */
		 l.repo, l.pulls ? "pr" : "issue", l.closed ? "closed" : "open", PER_PAGE, page);
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
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), UI_FONT_BODY, 0);
	/* The page shown */
	lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
	/* Leading nowhere */
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	return b;
}

static void filter_cb(lv_event_t *e)
{
	bool closed = (uintptr_t)lv_event_get_user_data(e);
	if (closed == l.closed)
		return;
	l.closed = closed;
	for (unsigned i = 0; i < 2; i++)
		lv_obj_set_state(l.filters[i], LV_STATE_CHECKED, i == closed);
	/* Its own count and pages, from the first */
	lv_label_set_text_static(l.title, l.pulls ? _("Pull requests") : _("Issues"));
	l.pages = 0;
	go(1);
}

/* Open or closed, as Clock's tabs: a pill each, the one shown in the accent color */
static void filters_create(lv_obj_t *parent)
{
	static const char *const names[2] = {NC_("filter", "Open"), NC_("filter", "Closed")};

	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(bar, 16, 0);
	lv_obj_set_style_pad_bottom(bar, 8, 0);

	lv_obj_t *seg = ui_box(bar);
	lv_obj_set_size(seg, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(seg, 6, 0);
	lv_obj_set_style_pad_column(seg, 6, 0);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);

	for (uintptr_t i = 0; i < 2; i++) {
		lv_obj_t *tab = l.filters[i] = ui_box(seg);
		lv_obj_set_height(tab, 72);
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_white(), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, filter_cb, LV_EVENT_CLICKED, (void *)i);
		lv_obj_set_state(tab, LV_STATE_CHECKED, i == 0);

		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, _(names[i]));
		lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
		lv_obj_center(label);
	}
}

static void refresh_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	go(l.page);
}

void list_merged(const char *repo, long long number)
{
	if (!l.scr || !l.pulls || strcmp(repo, l.repo))
		return;
	for (unsigned i = 0; i < l.row_count; i++)
		if (l.rows[i]->number == number)
			meta_set(l.rows[i], _("Merged"));
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&l.fetch);
	ui_fetch_stop(&l.checks_fetch);
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
	ui_header_button(header, LV_SYMBOL_REFRESH, refresh_cb, NULL);
	lv_obj_add_event_cb(l.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);
	l.pulls = pulls;
	snprintf(l.repo, sizeof(l.repo), "%s", repo);
	filters_create(body);

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
