/*
 * GitHub's notifications, as its inbox has them: read ones too (all=true),
 * newest first, the unread ones with a dot. Each row: the repository and the
 * time, the subject's title, its kind and why it came. Tapping one marks its
 * thread read and opens it if it is a pull request, an issue or a discussion
 * (pr.c); the header's button marks them all read. Pulled down, the list loads again.
 * Swiped away, one is marked done: gone from the inbox, as on github.com, until
 * its thread has news.
 * They are kept between openings and shown while loading again.
 * Marking goes in a queue of its own, one request at a time, which outlives the
 * screen.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"

#define PAGE "50" /* notifications loaded (GitHub's most per page) */
#define QUEUE 64 /* threads waiting to be marked read */
#define ALL ""   /* in the queue: all of them */

struct notification {
	char id[24];  /* the thread's */
	char *repo, *title;
	char *url;          /* a pull request's or an issue's API URL, NULL: another kind */
	bool issue, discussion; /* else a pull request, if it has a URL */
	bool talk; /* about a comment or a mention: opens on the conversation */
	const char *type, *reason; /* "Pull request", "Review requested": the tables' (N_()), or the
				    * API's when they have none, interned */
	time_t updated;
	bool unread;
};

static struct {
	struct notification *items;
	unsigned count;
	bool loaded;   /* at least once */
	struct ui_fetch fetch;
	/* Marking read or done */
	struct {
		char id[24];
		bool done; /* marked done (gone from the inbox), else read */
	} queue[QUEUE];
	unsigned queued;
	struct ui_fetch mark;
} n;

/* The list, while the screen is open */
static struct {
	lv_obj_t *list, *card, *empty;
	lv_obj_t *mark_all;
	lv_obj_t **dots; /* per notification */
} v;

struct label {
	const char *api, *text;
};

static const struct label kinds[] = {
	{"PullRequest", N_("Pull request")}, /* NON-NLS: the API's */
	{"Issue", N_("Issue")}, /* NON-NLS: the API's */
	{"Release", N_("Release")}, /* NON-NLS: the API's */
	{"Discussion", N_("Discussion")}, /* NON-NLS: the API's */
	{"Commit", N_("Commit")}, /* NON-NLS: the API's */
	{"CheckSuite", N_("Checks")}, /* NON-NLS: the API's */
	{"RepositoryVulnerabilityAlert", N_("Security alert")}, /* NON-NLS: the API's */
	{"RepositoryDependabotAlertsThread", N_("Dependabot")}, /* NON-NLS: the API's */
	{"RepositoryInvitation", N_("Invitation")}, /* NON-NLS: the API's */
}, reasons[] = {
	{"approval_requested", N_("Approval requested")}, /* NON-NLS: the API's */
	{"assign", N_("Assigned")}, /* NON-NLS: the API's */
	{"author", N_("Author")}, /* NON-NLS: the API's */
	{"ci_activity", N_("CI activity")}, /* NON-NLS: the API's */
	{"comment", N_("Comment")}, /* NON-NLS: the API's */
	{"invitation", N_("Invitation")}, /* NON-NLS: the API's */
	{"manual", N_("Subscribed")}, /* NON-NLS: the API's */
	{"member_feature_requested", N_("Feature requested")}, /* NON-NLS: the API's */
	{"mention", N_("Mentioned")}, /* NON-NLS: the API's */
	{"review_requested", N_("Review requested")}, /* NON-NLS: the API's */
	{"security_advisory_credit", N_("Advisory credit")}, /* NON-NLS: the API's */
	{"security_alert", N_("Security alert")}, /* NON-NLS: the API's */
	{"state_change", N_("State changed")}, /* NON-NLS: the API's */
	{"subscribed", N_("Subscribed")}, /* NON-NLS: the API's */
	{"team_mention", N_("Team mentioned")}, /* NON-NLS: the API's */
};

static const char *lookup(const char *api, const struct label *t, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (!strcmp(t[i].api, api))
			return t[i].text;
	/* One GitHub added since: as it says it */
	return ui_intern(api);
}

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
	for (unsigned i = 0; i < n.count; i++) {
		free(n.items[i].repo);
		free(n.items[i].title);
		free(n.items[i].url);
	}
	free(n.items);
	n.items = NULL;
	n.count = 0;
}

/* Queued to be marked read (or done, if `done`): shown so already */
static bool queued(const char *id, bool done)
{
	for (unsigned i = 0; i < n.queued; i++)
		if ((!strcmp(n.queue[i].id, ALL) && !done) ||
		    (!strcmp(n.queue[i].id, id) && (n.queue[i].done || !done)))
			return true;
	return false;
}

static void parse(const struct json *list)
{
	clear();
	unsigned count = 0;
	for (const struct json *e = list->child; e; e = e->next)
		count++;
	n.items = calloc(count ? count : 1, sizeof(*n.items));
	for (const struct json *e = list->child; e; e = e->next) {
		const char *id = json_str(json_get(e, "id"), "");
		if (queued(id, true))
			continue;
		struct notification *it = &n.items[n.count++];
		const struct json *subject = json_get(e, "subject");
		snprintf(it->id, sizeof(it->id), "%s", id);
		it->repo = strdup(json_str(json_get(json_get(e, "repository"), "full_name"), ""));
		it->title = strdup(json_str(json_get(subject, "title"), ""));
		const char *url = json_str(json_get(subject, "url"), NULL);
		const char *type = json_str(json_get(subject, "type"), "");
		it->issue = !strcmp(type, "Issue");
		it->discussion = !strcmp(type, "Discussion");
		if (url && (it->issue || !strcmp(type, "PullRequest")))
			it->url = strdup(url);
		it->type = lookup(json_str(json_get(subject, "type"), ""), kinds, LV_ARRAYLEN(kinds));
		const char *reason = json_str(json_get(e, "reason"), "");
		it->reason = lookup(reason, reasons, LV_ARRAYLEN(reasons));
		/* The API's */
		it->talk = !strcmp(reason, "comment") || !strcmp(reason, "mention") || /* NON-NLS */
			   !strcmp(reason, "team_mention"); /* NON-NLS */
		it->updated = parse_time(json_str(json_get(e, "updated_at"), ""));
		it->unread = json_bool(json_get(e, "unread")) && !queued(it->id, false);
	}
}

static void dot_set(lv_obj_t *dot, bool unread)
{
	lv_obj_set_style_bg_color(dot, lv_color_hex(unread ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
}

static void mark_next(void);

static void mark_done(const char *data, long status)
{
	LV_UNUSED(data);
	if (status / 100 != 2)
		printf("github: marking %s %s: HTTP %ld\n", n.queue[0].id[0] ? n.queue[0].id : "all",
		       n.queue[0].done ? "done" : "read", status);
	if (status == 401) {
		n.queued = 0;
		github_sign_out(_("GitHub refused this token."));
		return;
	}
	memmove(&n.queue[0], &n.queue[1], (n.queued - 1) * sizeof(n.queue[0]));
	n.queued--;
	mark_next();
}

static void mark_next(void)
{
	if (!n.queued || n.mark.curl)
		return;
	if (!n.queue[0].id[0]) {
		api_request(&n.mark, "PUT", "/notifications", "{\"read\":true}", mark_done);
	} else {
		char path[64];
		snprintf(path, sizeof(path), "/notifications/threads/%s", n.queue[0].id);
		api_request(&n.mark, n.queue[0].done ? "DELETE" : "PATCH", path, NULL, mark_done);
	}
}

static void mark(const char *id, bool done)
{
	if (n.queued == QUEUE) {
		printf("github: too many threads to mark\n");
		return;
	}
	snprintf(n.queue[n.queued].id, sizeof(n.queue[0].id), "%s", id);
	n.queue[n.queued++].done = done;
	mark_next();
}

/* Marked read, and a pull request or an issue opens */
static void row_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i >= n.count)
		return;
	struct notification *it = &n.items[i];
	if (it->unread) {
		it->unread = false;
		dot_set(v.dots[i], false);
		mark(it->id, false);
	}
	if (it->url && !strncmp(it->url, API "/", strlen(API "/")))
		(it->issue ? issue_open : pr_open)(it->url + strlen(API), it->repo, it->title, it->talk);
	else if (it->discussion)
		discussion_open(it->repo, it->title);
}

static void rows_update(void);
static void status(const char *text);

/* Swiped away: marked done, gone from the list */
static void done_cb(lv_obj_t *row, void *user_data)
{
	LV_UNUSED(row);
	unsigned i = (uintptr_t)user_data;
	if (i >= n.count)
		return;
	mark(n.items[i].id, true);
	free(n.items[i].repo);
	free(n.items[i].title);
	free(n.items[i].url);
	memmove(&n.items[i], &n.items[i + 1], (n.count - i - 1) * sizeof(*n.items));
	n.count--;
	rows_update();
	status(n.count ? NULL : _("No notifications"));
}

static void mark_all_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	bool any = false;
	for (unsigned i = 0; i < n.count; i++) {
		any |= n.items[i].unread;
		n.items[i].unread = false;
		dot_set(v.dots[i], false);
	}
	if (any)
		mark(ALL, false);
}

/* The rows again, from n.items */
static void rows_update(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.card);
	free(v.dots);
	v.dots = calloc(n.count ? n.count : 1, sizeof(*v.dots));
	for (unsigned i = 0; i < n.count; i++) {
		const struct notification *it = &n.items[i];
		lv_obj_t *row = ui_row_create(v.card, LV_SIZE_CONTENT, i + 1 < n.count, row_cb, (void *)(uintptr_t)i);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		ui_swipe_away(row, LV_SYMBOL_OK, UI_COLOR_ACCENT, done_cb, (void *)(uintptr_t)i);

		/* In line with the title */
		lv_obj_t *dot = v.dots[i] = ui_box(row);
		lv_obj_set_size(dot, 16, 16);
		lv_obj_set_style_margin_top(dot, 44, 0);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		dot_set(dot, it->unread);

		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *top = ui_row_line(text);
		lv_obj_t *repo = ui_one_line_label(top, "", UI_FONT_CAPTION);
		ui_set_text(repo, it->repo);
		lv_obj_set_style_text_color(repo, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_flex_grow(repo, 1);
		char when[32] = "";
		if (it->updated)
			ui_when_format(when, sizeof(when), it->updated);
		lv_obj_t *time = ui_dim_label(top, "", UI_FONT_CAPTION);
		ui_set_text(time, when);

		lv_obj_t *title = lv_label_create(text);
		lv_label_set_text(title, it->title);
		lv_obj_set_width(title, LV_PCT(100));
		lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);

		lv_obj_t *kind = ui_dim_label(text, "", UI_FONT_CAPTION);
		/* In the language of the moment */
		ui_set_text_fmt(kind, "%s \xc2\xb7 %s", _(it->type), _(it->reason));
	}
	lv_obj_set_hidden(v.card, !n.count);
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
	if (v.list)
		ui_pull_refresh_done(v.list);
	struct json *list = data && code == 200 ? json_parse(data) : NULL;
	if (code == 401) {
		github_sign_out(_("GitHub refused this token."));
	} else if (list && list->kind == JSON_ARRAY) {
		n.loaded = true;
		parse(list);
		rows_update();
		status(n.count ? NULL : _("No notifications"));
	} else if (!code) {
		/* What was loaded before stays */
		if (!n.loaded || !n.count)
			status(_("Cannot reach GitHub"));
	} else {
		char text[64];
		snprintf(text, sizeof(text), _("GitHub answered with an error (HTTP %ld)"), code);
		if (!n.loaded || !n.count)
			status(text);
	}
	json_free(list);
}

void notifications_load(void)
{
	if (!n.loaded)
		status(_("Loading..."));
	api_request(&n.fetch, "GET", "/notifications?all=true&per_page=" PAGE, NULL, loaded);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* What it loads would have nowhere to go; marking goes on */
	ui_fetch_stop(&n.fetch);
	free(v.dots);
	memset(&v, 0, sizeof(v));
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	notifications_load();
}

void notifications_create(lv_obj_t *body, lv_obj_t *header)
{
	v.list = lv_obj_create(body);
	lv_obj_remove_style_all(v.list);
	lv_obj_set_width(v.list, LV_PCT(100));
	lv_obj_set_flex_grow(v.list, 1);
	lv_obj_set_flex_flow(v.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(v.list, 16, 0);
	lv_obj_set_scroll_dir(v.list, LV_DIR_VER);
	lv_obj_add_event_cb(v.list, delete_cb, LV_EVENT_DELETE, NULL);
	ui_pull_refresh(v.list, pull_cb, NULL);

	v.empty = ui_empty_label(v.list, "");
	lv_obj_set_hidden(v.empty, true);
	v.card = ui_card_create(v.list);
	lv_obj_set_hidden(v.card, true);

	v.mark_all = ui_header_button(header, LV_SYMBOL_OK, mark_all_cb, NULL);
	rows_update();
}

void notifications_show(bool shown)
{
	if (!v.list)
		return;
	lv_obj_set_hidden(v.list, !shown);
	lv_obj_set_hidden(v.mark_all, !shown);
}
