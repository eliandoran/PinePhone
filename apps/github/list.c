/*
 * A repository's issues or pull requests, open or closed (chosen at the top),
 * newest first, PER_PAGE a page, from GitHub's search (is:issue or is:pr, is:open
 * or is:closed): its total gives the number of pages, which GitHub's plain lists
 * only say in a header. A bar at the bottom goes to a page (pager.c). The search
 * serves MAX_RESULTS at most: no pages beyond. The title says how many there are
 * (all of them, once known). Each row: its number, author, a tag (a pull request's
 * Draft; Merged, Not planned or Duplicate for a closed one) and when it was opened,
 * then its title; tapping one opens it (pr.c). The header's refresh button loads the page shown again; one merged
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

/* A row's, freed with it */
struct row {
	long long number;
	char *title, *login;
	lv_obj_t *meta; /* "#7 · elian · Draft" */
	lv_obj_t *icon; /* its state's, at the start of the row */
	lv_obj_t *checks; /* a pull request's icon, once known */
};

static struct {
	lv_obj_t *scr, *list, *status, *card;
	lv_obj_t *title; /* the header's */
	lv_obj_t *filters[2]; /* Open, Closed */
	struct pager pager; /* its page: the one shown or asked for */
	bool pulls;
	bool closed;     /* the closed ones, not the open ones */
	char repo[160]; /* "o/r" */
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
	(l.pulls ? pr_open : issue_open)(path, l.repo, r->title, false);
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
	/* The icon level with the first line, as on GitHub */
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	r->icon = lv_label_create(row);
	lv_obj_set_style_text_font(r->icon, &ui_font_24, 0);
	lv_obj_t *text = ui_row_text(row);

	lv_obj_t *top = ui_row_line(text);
	r->meta = ui_one_line_label(top, "", UI_FONT_CAPTION);
	const char *reason = json_str(json_get(j, "state_reason"), "");
	bool open = !strcmp(json_str(json_get(j, "state"), "open"), "open"); /* NON-NLS */
	bool merged = l.pulls && json_str(json_get(json_get(j, "pull_request"), "merged_at"), NULL);
	bool skipped = !strcmp(reason, "not_planned") || !strcmp(reason, "duplicate"); /* NON-NLS */
	if (l.pulls)
		state_icon_set(r->icon, merged ? STATE_PR_MERGED
					: !open ? STATE_PR_CLOSED
					: json_bool(json_get(j, "draft")) ? STATE_PR_DRAFT : STATE_PR_OPEN);
	else
		state_icon_set(r->icon, open ? STATE_ISSUE_OPEN : skipped ? STATE_ISSUE_SKIPPED : STATE_ISSUE_DONE);
	const char *tag = NULL;
	if (json_bool(json_get(j, "draft")) && !l.closed)
		tag = _("Draft");
	else if (merged)
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
	l.pager.pages = (int)((total + PER_PAGE - 1) / PER_PAGE);
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
	pager_update(&l.pager);
}

/* Page `page`: the rows that were there go, the list from its top */
static void go(int page)
{
	l.pager.page = page;
	ui_fetch_stop(&l.checks_fetch);
	l.row_count = 0;
	lv_obj_clean(l.card);
	lv_obj_set_hidden(l.card, true);
	status(_("Loading..."));
	lv_obj_scroll_to_y(l.list, 0, LV_ANIM_OFF);
	pager_update(&l.pager);
	char path[320];
	snprintf(path, sizeof(path),
		 "/search/issues?q=repo:%s+is:%s+is:%s&sort=created&order=desc&per_page=%d&page=%d", /* NON-NLS */
		 l.repo, l.pulls ? "pr" : "issue", l.closed ? "closed" : "open", PER_PAGE, page);
	api_request(&l.fetch, "GET", path, NULL, loaded);
}

static void filter_cb(lv_event_t *e)
{
	bool closed = (uintptr_t)lv_event_get_user_data(e);
	if (closed == l.closed)
		return;
	l.closed = closed;
	segments_select(l.filters, 2, closed);
	/* Its own count and pages, from the first */
	lv_label_set_text_static(l.title, l.pulls ? _("Pull requests") : _("Issues"));
	l.pager.pages = 0;
	go(1);
}

/* Open or closed (segments.c) */
static void filters_create(lv_obj_t *parent)
{
	static const char *const names[2] = {NC_("filter", "Open"), NC_("filter", "Closed")};
	const char *translated[2] = {_(names[0]), _(names[1])};
	segments_create(parent, l.filters, 2, translated, filter_cb);
}

static void refresh_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	go(l.pager.page);
}

void list_merged(const char *repo, long long number)
{
	if (!l.scr || !l.pulls || strcmp(repo, l.repo))
		return;
	for (unsigned i = 0; i < l.row_count; i++)
		if (l.rows[i]->number == number) {
			meta_set(l.rows[i], _("Merged"));
			state_icon_set(l.rows[i]->icon, STATE_PR_MERGED);
		}
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
	pager_create(&l.pager, body, go);

	go(1);
	ui_push(l.scr);
}
