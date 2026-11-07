/*
 * A pull request, an issue or a discussion: a card with its title, key
 * (owner/repo#7), state, and a pull request's branches, an issue's labels or a
 * discussion's category, then its conversation as a chat (ui_chat, as Messages
 * and Telegram): the comments and a pull request's reviews in the order they came,
 * the day above those of each day (a review a line with its state's icon, its text
 * below), a discussion's comments each followed by its replies; the account's own on
 * the right. Comments are sent from the compose bar. Each message is a bubble, its
 * Markdown rendered (markdown.c), HTML comments (templates' hints) left out.
 *
 * A pull request has three tabs: its overview (the card, who opened it, rows for its
 * status checks and its milestone, its description across the screen, merging), its
 * conversation, which also has the compose bar, and its files with their diffs
 * (files.c), asked for when first shown. An issue or a discussion has
 * the card then its description first in the conversation. The status checks
 * (GraphQL) are summed up in their row, each listed on a screen of their own when
 * tapped. An open pull request is merged from the overview when the account can push
 * to its repository: tapped, the ways the repository allows (GitHub's names), one of
 * them merges the head shown (GitHub refuses if it moved since). Who can triage the
 * repository taps the milestone's row to choose another (milestone.c), set with the
 * REST API's issue (a pull request is one there too). A pull request's commits are
 * not shown yet.
 *
 * Discussions are only in GitHub's GraphQL API, and their notifications have no
 * URL: one is found by its repository and title (the latest of that title), then
 * loaded by its number.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"
#include "ui_symbols.h"

#define MAX_LENGTH 65536 /* GitHub's limit for a comment */
#define MAX_MESSAGE 65536 /* bytes of a message's text: its labels are cut shorter (markdown.c) */

#define COLOR_MERGED 0x8957e5 /* GitHub's, also an issue closed as completed */
#define COLOR_GRAY 0x6e7681      /* a draft, an issue not planned */
#define LABELS 8                 /* an issue's labels shown */
#define REPLY_INDENT 48          /* a discussion's reply, from its comment */
#define CHECKS 100               /* a pull request's status checks shown */
#define ROW_H 88                 /* the overview's rows: checks, milestone */
#define MAX_HTML_H 20000         /* a description's GitHub rendering, cut there */

/* NON-NLS-BEGIN: GraphQL: a discussion's number by its title, the discussion, a comment on it,
 * a pull request's checks and whether the account can merge it, what it can do with an issue.
 * On one line, without quotes: they go in a JSON string as they are */
#define SEARCH_QUERY                                                                         \
	"query($q: String!) { search(query: $q, type: DISCUSSION, first: 20) {"              \
	" nodes { ... on Discussion { number title updatedAt } } } }"
#define MESSAGE_FIELDS "author { login } createdAt body bodyHTML"
#define DISCUSSION_QUERY                                                                     \
	"query($owner: String!, $name: String!, $number: Int!) {"                            \
	" repository(owner: $owner, name: $name) { discussion(number: $number) {"            \
	" id title closed stateReason isAnswered category { name } " MESSAGE_FIELDS           \
	" comments(first: 100) { nodes { isAnswer " MESSAGE_FIELDS                            \
	" replies(first: 100) { nodes { " MESSAGE_FIELDS " } } } } } } }"
#define STATUS_QUERY                                                                         \
	"query($owner: String!, $name: String!, $number: Int!) {"                            \
	" repository(owner: $owner, name: $name) { viewerPermission"                         \
	" mergeCommitAllowed squashMergeAllowed rebaseMergeAllowed"                          \
	" pullRequest(number: $number) { mergeable commits(last: 1) { nodes { commit {"      \
	" statusCheckRollup { contexts(first: 100) {"                                        \
	" nodes { __typename ... on CheckRun { name status conclusion }"                      \
	" ... on StatusContext { context state } } } } } } } } } }"
#define PERMISSION_QUERY                                                                     \
	"query($owner: String!, $name: String!) {"                                           \
	" repository(owner: $owner, name: $name) { viewerPermission } }"
#define COMMENT_MUTATION                                                                  \
	"mutation($id: ID!, $body: String!) {"                                               \
	" addDiscussionComment(input: {discussionId: $id, body: $body}) {"                   \
	" comment { " MESSAGE_FIELDS " } } }"
/* NON-NLS-END */

enum kind { COMMENT, REVIEW };
enum type { PR, ISSUE, DISCUSSION };
enum merge { MERGE_COMMIT, SQUASH, REBASE, MERGES };

/* The ways to merge: the REST API's merge_method, the GraphQL field saying the
 * repository allows it, GitHub's name for it */
static const struct {
	const char *method, *allowed, *name;
} merges[MERGES] = {
	[MERGE_COMMIT] = {"merge", "mergeCommitAllowed", N_("Create a merge commit")}, /* NON-NLS */
	[SQUASH] = {"squash", "squashMergeAllowed", N_("Squash and merge")},           /* NON-NLS */
	[REBASE] = {"rebase", "rebaseMergeAllowed", N_("Rebase and merge")},           /* NON-NLS */
};

struct entry {
	enum kind kind;
	char *login, *body;
	char *html; /* GitHub's rendering of body, NULL if it gave none */
	const char *review; /* "%s approved"..., a review's (N_(): translated when shown) */
	const char *review_symbol; /* its state's icon */
	uint32_t review_color;     /* its color, 0: dim */
	const char *reply_to; /* a discussion's reply: its comment's author, interned */
	bool answer;          /* a discussion's comment marked as its answer */
	time_t time;
};

struct entries {
	struct entry *e;
	unsigned count;
};

enum tab { OVERVIEW, CONVERSATION, FILES, TABS };

/* The open pull request, issue or discussion */
static struct {
	lv_obj_t *scr, *header;
	struct ui_chat chat;
	/* A pull request's: its tabs, the conversation's and the files' counts beside their
	 * names, the overview and the files (scrolling columns) */
	lv_obj_t *tabs[TABS], *count, *files_count, *overview, *files;
	bool files_asked; /* once, when first shown */
	enum tab tab;
	enum type type;
	char path[192];   /* "/repos/o/r/pulls/7", "/repos/o/r/issues/7"; "o/r" for a discussion */
	char target[192]; /* where comments go: "/repos/o/r/issues/7/comments", a discussion's node ID */
	const char *title, *key, *base, *head; /* interned */
	char head_sha[48]; /* the commit a merge merges */
	enum { OPEN, DRAFT, MERGED, CLOSED, DONE, NOT_PLANNED, ANSWERED, OUTDATED, DUPLICATE } state;
	struct {
		const char *name; /* interned */
		uint32_t color;
	} labels[LABELS];
	unsigned label_count;
	struct entry description;
	struct entries list, reviews; /* comments, reviews */
	struct {
		const char *name; /* interned */
		enum check state;
	} checks[CHECKS]; /* a pull request's, failed first */
	unsigned check_count;
	bool can_merge;  /* the account can push to the repository */
	bool can_triage;  /* or triage it: milestones */
	const char *repo; /* "o/r", interned; NULL for a discussion */
	long long milestone; /* its number, 0: none */
	const char *milestone_title; /* interned */
	const char *milestone_new; /* the one being set, interned; NULL: none */
	bool milestone_saving;
	char milestone_error[192];
	bool conflicts;   /* GitHub cannot merge it */
	bool merge_allowed[MERGES]; /* by the repository */
	bool merge_open;  /* the ways shown */
	char merge_error[192];
	bool loaded;   /* the pull request, issue or discussion */
	long error;    /* its HTTP status if it failed, 0: none or offline */
	char error_text[192]; /* or GraphQL's reason */
	bool failed, not_found;
	char *sending; /* a comment on its way */
	char send_error[192];
	struct ui_fetch pr, comments_fetch, reviews_fetch, status_fetch, files_fetch;
} p;

/* Outlive the screen */
static struct ui_fetch send_fetch, me_fetch;
static char send_to[192]; /* the target it goes to */
static bool send_graphql; /* to a discussion */
static char me[64]; /* the account's login, "" until known */
static struct ui_fetch merge_fetch;
static char merge_path[192]; /* the pull request it merges */
static struct ui_fetch milestone_fetch;
static char milestone_path[192]; /* the issue it changes ("/repos/o/r/issues/7") */

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

static void entry_free(struct entry *e)
{
	free(e->login);
	free(e->body);
	free(e->html);
	memset(e, 0, sizeof(*e));
}

static void entries_free(struct entries *l)
{
	for (unsigned i = 0; i < l->count; i++)
		entry_free(&l->e[i]);
	free(l->e);
	memset(l, 0, sizeof(*l));
}

static void entry_fill(struct entry *e, enum kind kind, const char *login, const char *body, const char *html,
		       const char *time)
{
	e->kind = kind;
	e->login = strdup(login);
	e->body = strdup(body);
	e->html = html ? strdup(html) : NULL;
	e->time = parse_time(time);
}

/* From the REST API */
static void entry_set(struct entry *e, enum kind kind, const struct json *j, const char *time_key)
{
	entry_fill(e, kind, json_str(json_get(json_get(j, "user"), "login"), ""), json_str(json_get(j, "body"), ""),
		   json_str(json_get(j, "body_html"), NULL), json_str(json_get(j, time_key), ""));
}

/* From GraphQL (MESSAGE_FIELDS): a deleted account has no author */
static void entry_set_graphql(struct entry *e, const struct json *j)
{
	entry_fill(e, COMMENT, json_str(json_get(json_get(j, "author"), "login"), "ghost"),
		   json_str(json_get(j, "body"), ""), json_str(json_get(j, "bodyHTML"), NULL),
		   json_str(json_get(j, "createdAt"), ""));
}

static struct entry *entries_new(struct entries *l)
{
	struct entry *e = realloc(l->e, (l->count + 1) * sizeof(*e));
	if (!e)
		return NULL;
	l->e = e;
	memset(&e[l->count], 0, sizeof(*e));
	return &e[l->count++];
}

static void entries_add(struct entries *l, enum kind kind, const struct json *j, const char *time_key)
{
	struct entry *e = entries_new(l);
	if (e)
		entry_set(e, kind, j, time_key);
}

/* Centered, dim, in `parent`: days, errors, a discussion's answer */
static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, "");
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

/* A message's text as shown: without \r and HTML comments (templates' hints), cut at
 * MAX_MESSAGE bytes; malloc'd, NULL if there is no memory */
static char *message_text(const char *body)
{
	/* Room for the ellipsis after a cut */
	char *text = malloc(strlen(body) + 4), *out = text;
	if (!text)
		return NULL;
	for (const char *s = body; *s;) {
		if (!strncmp(s, "<!--", 4)) {
			const char *end = strstr(s + 4, "-->");
			s = end ? end + 3 : s + strlen(s);
		} else if (*s == '\r') {
			s++;
		} else {
			*out++ = *s++;
		}
	}
	*out = '\0';
	if (out - text > MAX_MESSAGE) {
		/* Whole characters, then an ellipsis */
		char *cut = text + MAX_MESSAGE;
		while (cut > text && ((unsigned char)*cut & 0xc0) == 0x80)
			cut--;
		memcpy(cut, "\xe2\x80\xa6", 4);
	}
	return text;
}

/* A message in a bubble (markdown.c: its Markdown, or GitHub's HTML of it) */
static void bubble(lv_obj_t *group, bool outgoing, const char *body, const char *html)
{
	char *text = message_text(body);
	if (!text)
		return;
	markdown_bubble(group, outgoing, text, html);
	free(text);
}

/* The day above the messages of `t`'s, if it is not `*last`'s (then it is): today,
 * yesterday, its name this week, its date before */
static void day_label(time_t t, time_t *last)
{
	struct tm tm, before;
	localtime_r(&t, &tm);
	localtime_r(last, &before);
	if (*last && tm.tm_year == before.tm_year && tm.tm_yday == before.tm_yday)
		return;
	*last = t;
	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);
	today.tm_hour = today.tm_min = today.tm_sec = 0;
	time_t midnight = mktime(&today);
	lv_obj_t *label = centered_label(p.chat.list, UI_FONT_CAPTION);
	if (t >= midnight)
		lv_label_set_text_static(label, _("Today"));
	else if (t >= midnight - 86400)
		lv_label_set_text_static(label, _("Yesterday"));
	else if (t >= midnight - 6 * 86400)
		lv_label_set_text_static(label, ui_day_name(tm.tm_wday, false));
	else if (tm.tm_year == today.tm_year)
		ui_set_text_fmt(label, "%d %s", tm.tm_mday, ui_month_name(tm.tm_mon, false));
	else
		ui_set_text_fmt(label, "%d %s %d", tm.tm_mday, ui_month_name(tm.tm_mon, false), tm.tm_year + 1900);
}

/* A comment, a review's text or the description: the author's name above it unless
 * it is the account's or `named` is false (a review's, under its line), the time
 * below (`days`: the hour, under its day; or ui_when_format()'s). A discussion's
 * answer has a line above it, a reply is set in and says whose comment it answers */
static void message(const struct entry *e, bool named, bool days)
{
	if (e->answer) {
		lv_obj_t *label = centered_label(p.chat.list, UI_FONT_CAPTION);
		ui_set_text_fmt(label, LV_SYMBOL_OK " %s", _("Answer"));
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_GREEN), 0);
	}
	bool outgoing = me[0] && !strcmp(e->login, me);
	lv_obj_t *time;
	lv_obj_t *group = ui_chat_group_create(&p.chat, outgoing, &time);
	if (e->reply_to)
		lv_obj_set_style_pad_left(group, REPLY_INDENT, 0);
	if (!outgoing && named) {
		lv_obj_t *name = lv_label_create(group);
		if (e->reply_to)
			ui_set_text_fmt(name, _("%s, replying to %s"), e->login, e->reply_to);
		else
			ui_set_text(name, e->login);
		lv_obj_set_style_text_font(name, UI_FONT_CAPTION, 0);
		lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_pad_hor(name, 12, 0);
		lv_obj_move_to_index(name, 0);
	}
	char when[32] = "";
	if (e->time && days) {
		struct tm tm;
		localtime_r(&e->time, &tm);
		strftime(when, sizeof(when), "%H:%M", &tm);
	} else if (e->time) {
		ui_when_format(when, sizeof(when), e->time);
	}
	ui_set_text(time, when);
	bubble(group, outgoing, e->body, e->html);
}

/* A review: a line with its state's icon in its color, who and what, then its text */
static void review(const struct entry *e, bool days)
{
	lv_obj_t *line = ui_box(p.chat.list);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(line, 10, 0);
	lv_obj_t *icon = lv_label_create(line);
	lv_label_set_text_static(icon, e->review_symbol);
	lv_obj_set_style_text_font(icon, &ui_font_20, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(e->review_color ? e->review_color : UI_COLOR_DIM), 0);
	lv_obj_t *label = ui_dim_label(line, "", UI_FONT_CAPTION);
	ui_set_text_fmt(label, _(e->review), e->login);
	if (e->body[0])
		message(e, false, days);
}

/* A state or a label: static text on a rounded color, the text black or white,
 * whichever shows on it; a state's icon before it (`symbol`, NULL: none) */
static void pill(lv_obj_t *line, const char *symbol, const char *text, uint32_t color)
{
	lv_obj_t *pill = ui_box(line);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(pill, 16, 0);
	lv_obj_set_style_pad_ver(pill, 6, 0);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
	lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(pill, 8, 0);
	unsigned luma = ((color >> 16) * 299 + (color >> 8 & 0xff) * 587 + (color & 0xff) * 114) / 1000;
	lv_obj_set_style_text_color(pill, luma > 160 ? lv_color_black() : lv_color_white(), 0);
	lv_obj_set_style_text_font(pill, UI_FONT_CAPTION, 0);
	if (symbol)
		lv_label_set_text_static(lv_label_create(pill), symbol);
	lv_label_set_text_static(lv_label_create(pill), text);
}

static void rows_card(lv_obj_t *parent);
static void merge_show(lv_obj_t *parent);
static void error_label(lv_obj_t *parent, const char *text);

/* The card at the top: title, key, a pull request's author, the state and a pull
 * request's branches, an issue's labels or a discussion's category */
static void summary(lv_obj_t *parent)
{
	lv_obj_t *card = ui_card_create(parent);
	lv_obj_set_style_pad_all(card, 24, 0);
	lv_obj_set_style_pad_row(card, 12, 0);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text_static(title, p.title);
	lv_obj_set_width(title, LV_PCT(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);

	if (p.key)
		ui_dim_label(card, p.key, UI_FONT_CAPTION);

	if (!p.loaded)
		return;
	if (p.type == PR) {
		lv_obj_t *by = ui_dim_label(card, "", UI_FONT_CAPTION);
		char when[32] = "";
		if (p.description.time)
			ui_when_format(when, sizeof(when), p.description.time);
		ui_set_text_fmt(by, _("Opened by %s, %s"), p.description.login, when);
	}
	lv_obj_t *line = ui_row_line(card);
	static const struct {
		const char *text;
		uint32_t color;
		const char *symbol; /* as the lists' rows (checks.c), NULL: by its type */
	} states[] = {
		[OPEN] = {N_("Open"), UI_COLOR_GREEN, NULL},
		[DRAFT] = {N_("Draft"), COLOR_GRAY, UI_SYMBOL_CODE_PULL_REQUEST},
		[MERGED] = {N_("Merged"), COLOR_MERGED, UI_SYMBOL_CODE_MERGE},
		[CLOSED] = {N_("Closed"), UI_COLOR_RED, UI_SYMBOL_CODE_PULL_REQUEST},
		[DONE] = {N_("Closed"), COLOR_MERGED, UI_SYMBOL_CIRCLE_CHECK_REGULAR},
		[NOT_PLANNED] = {N_("Not planned"), COLOR_GRAY, UI_SYMBOL_BAN},
		[ANSWERED] = {N_("Answered"), UI_COLOR_GREEN, UI_SYMBOL_CIRCLE_CHECK_REGULAR},
		[OUTDATED] = {N_("Outdated"), COLOR_GRAY, UI_SYMBOL_BAN},
		[DUPLICATE] = {N_("Duplicate"), COLOR_GRAY, UI_SYMBOL_BAN},
	};
	const char *symbol = states[p.state].symbol;
	if (p.state == OPEN && p.type != DISCUSSION)
		symbol = p.type == PR ? UI_SYMBOL_CODE_PULL_REQUEST : UI_SYMBOL_CIRCLE_DOT_REGULAR;
	pill(line, symbol, _(states[p.state].text), states[p.state].color);

	if (p.type == PR) {
		/* From its branch into the other */
		lv_obj_t *branches = ui_dim_label(line, "", UI_FONT_CAPTION);
		ui_set_text_fmt(branches, "%s " LV_SYMBOL_RIGHT " %s", p.head, p.base);
		lv_obj_set_flex_grow(branches, 1);
		lv_label_set_long_mode(branches, LV_LABEL_LONG_MODE_WRAP);
	} else {
		/* The labels (a discussion's category), in their colors, wrapping */
		lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW_WRAP);
		lv_obj_set_style_pad_row(line, 10, 0);
		lv_obj_set_style_pad_column(line, 10, 0);
		for (unsigned i = 0; i < p.label_count; i++)
			pill(line, NULL, p.labels[i].name, p.labels[i].color);
	}
}

static int by_time(const void *a, const void *b)
{
	const struct entry *x = *(const struct entry *const *)a, *y = *(const struct entry *const *)b;
	return (x->time > y->time) - (x->time < y->time);
}

/* Why it is not shown (yet) */
static void status_label(lv_obj_t *parent)
{
	lv_obj_t *label = centered_label(parent, UI_FONT_BODY);
	if (!p.failed)
		lv_label_set_text_static(label, _("Loading..."));
	else if (p.not_found)
		lv_label_set_text_static(label, _("Discussion not found"));
	else if (p.error_text[0])
		ui_set_text(label, p.error_text);
	else if (!p.error)
		lv_label_set_text_static(label, _("Cannot reach GitHub"));
	else
		ui_set_text_fmt(label, _("GitHub answered with an error (HTTP %ld)"), p.error);
}

/* A pull request's overview again: the card, its rows, its description across the
 * screen, merging; the scroll position stays */
static void overview_fill(void)
{
	lv_obj_t *o = p.overview;
	lv_obj_update_layout(o);
	int32_t y = lv_obj_get_scroll_y(o);
	lv_obj_clean(o);
	summary(o);
	if (!p.loaded) {
		status_label(o);
		return;
	}
	rows_card(o);
	lv_obj_t *card = ui_card_create(o);
	lv_obj_set_style_pad_all(card, 24, 0);
	char *text = message_text(p.description.body);
	if (text && text[0]) {
		int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 2 * 24;
		char base[224];
		snprintf(base, sizeof(base), "https://github.com/%s/", p.repo ? p.repo : "");
		markdown_create(card, w, text, p.description.html, base, MAX_HTML_H);
	} else {
		ui_dim_label(card, _("No description"), UI_FONT_BODY);
	}
	free(text);
	if (p.state == OPEN)
		merge_show(o);
	lv_obj_update_layout(o);
	lv_obj_scroll_to_y(o, y, LV_ANIM_OFF);
}

/* The conversation again, from what came so far (an issue's or a discussion's after
 * its card and description); the scroll position stays, or the end */
static void chat_fill(bool to_end)
{
	lv_obj_update_layout(p.chat.list);
	int32_t y = lv_obj_get_scroll_y(p.chat.list);
	lv_obj_clean(p.chat.list);

	if (p.type != PR) {
		summary(p.chat.list);
		if (p.loaded)
			rows_card(p.chat.list);
	}
	if (!p.loaded) {
		status_label(p.chat.list);
		return;
	}

	/* A discussion's as they came, replies after their comment: its times on their
	 * own; the others' in their order under their days */
	bool days = p.type != DISCUSSION;
	time_t last = 0;
	if (p.type != PR && p.description.body[0]) {
		if (days)
			day_label(p.description.time, &last);
		message(&p.description, true, days);
	} else if (p.type != PR) {
		lv_obj_t *label = centered_label(p.chat.list, UI_FONT_CAPTION);
		lv_label_set_text_static(label, _("No description"));
	}

	unsigned count = p.list.count + p.reviews.count;
	const struct entry **all = malloc((count ? count : 1) * sizeof(*all));
	if (all) {
		for (unsigned i = 0; i < p.list.count; i++)
			all[i] = &p.list.e[i];
		for (unsigned i = 0; i < p.reviews.count; i++)
			all[p.list.count + i] = &p.reviews.e[i];
		if (count && days)
			qsort(all, count, sizeof(*all), by_time);
		for (unsigned i = 0; i < count; i++) {
			if (days)
				day_label(all[i]->time, &last);
			if (all[i]->kind == REVIEW)
				review(all[i], days);
			else
				message(all[i], true, days);
		}
		free(all);
	}
	if (p.type == PR && !count && !p.sending) {
		lv_obj_t *label = centered_label(p.chat.list, UI_FONT_BODY);
		lv_label_set_text_static(label, _("No comments yet"));
	}

	if (p.sending) {
		lv_obj_t *time;
		lv_obj_t *group = ui_chat_group_create(&p.chat, true, &time);
		lv_label_set_text_static(time, _("Sending..."));
		bubble(group, true, p.sending, NULL);
		/* Not on GitHub yet: paler */
		for (uint32_t i = 0; i + 1 < lv_obj_get_child_count(group); i++)
			lv_obj_set_style_bg_opa(lv_obj_get_child(group, i), LV_OPA_60, 0);
	}
	if (p.send_error[0]) {
		lv_obj_t *label = centered_label(p.chat.list, UI_FONT_CAPTION);
		ui_set_text(label, p.send_error);
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_RED), 0);
	}

	if (to_end) {
		ui_chat_scroll_end(&p.chat, LV_ANIM_ON);
	} else {
		lv_obj_update_layout(p.chat.list);
		lv_obj_scroll_to_y(p.chat.list, y, LV_ANIM_OFF);
	}
}

/* Everything again, from what came so far */
static void fill(bool to_end)
{
	if (!p.scr)
		return;
	chat_fill(to_end);
	if (p.type != PR)
		return;
	overview_fill();
	unsigned count = p.list.count + p.reviews.count;
	if (count)
		ui_set_text_fmt(p.count, "%u", count);
	else
		lv_label_set_text_static(p.count, "");
}


/* 401: the token is no longer good */
static bool refused(long status)
{
	if (status != 401)
		return false;
	/* The others would refuse it too: once */
	ui_fetch_stop(&p.pr);
	ui_fetch_stop(&p.comments_fetch);
	ui_fetch_stop(&p.reviews_fetch);
	ui_fetch_stop(&p.status_fetch);
	ui_fetch_stop(&p.files_fetch);
	github_refused();
	return true;
}

/* In `out`: a warning sign, then `format` ("Not sent: %s") with why GitHub refused
 * (its message, English, as it gave it) or the connection failed */
static void failure_format(char *out, size_t size, const char *format, long status, const char *error)
{
	char reason[160], text[192];
	if (!status)
		snprintf(reason, sizeof(reason), "%s", _("no connection"));
	else if (!error[0])
		snprintf(reason, sizeof(reason), "HTTP %ld", status);
	else
		snprintf(reason, sizeof(reason), "%s", error);
	snprintf(text, sizeof(text), format, reason);
	snprintf(out, size, LV_SYMBOL_WARNING " %s", text);
}

/* A pull request's or an issue's (REST) */
static void milestone_parse(const struct json *j)
{
	const struct json *m = json_get(j, "milestone");
	p.milestone = json_int(json_get(m, "number"), 0);
	p.milestone_title = p.milestone ? ui_intern(json_str(json_get(m, "title"), "")) : NULL;
}

/* Pulled down (thread_open()): an answer came */
static void pulled(void)
{
	lv_obj_t *lists[] = {p.overview, p.chat.list, p.files};
	for (size_t i = 0; i < LV_ARRAYLEN(lists); i++)
		if (lists[i])
			ui_pull_refresh_done(lists[i]);
}

static void pr_loaded(const char *data, long status)
{
	pulled();
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	if (!j || j->kind != JSON_OBJECT) {
		/* Loaded again: what came before stays */
		p.failed = !p.loaded;
		p.error = status;
		json_free(j);
		fill(false);
		return;
	}
	p.title = ui_intern(json_str(json_get(j, "title"), p.title));
	milestone_parse(j);
	if (p.files_count && json_get(j, "changed_files"))
		ui_set_text_fmt(p.files_count, "%lld", json_int(json_get(j, "changed_files"), 0));
	bool open = !strcmp(json_str(json_get(j, "state"), ""), "open");
	if (p.type == ISSUE) {
		if (open)
			p.state = OPEN;
		else
			p.state = strcmp(json_str(json_get(j, "state_reason"), ""), "not_planned") ? DONE : NOT_PLANNED;
		p.label_count = 0;
		for (const struct json *l = json_get(j, "labels") ? json_get(j, "labels")->child : NULL;
		     l && p.label_count < LABELS; l = l->next) {
			p.labels[p.label_count].name = ui_intern(json_str(json_get(l, "name"), ""));
			p.labels[p.label_count++].color = strtoul(json_str(json_get(l, "color"), "6e7681"), NULL, 16);
		}
	} else {
		p.base = ui_intern(json_str(json_get(json_get(j, "base"), "ref"), ""));
		p.head = ui_intern(json_str(json_get(json_get(j, "head"), "ref"), ""));
		snprintf(p.head_sha, sizeof(p.head_sha), "%s", json_str(json_get(json_get(j, "head"), "sha"), ""));
		if (json_bool(json_get(j, "merged")))
			p.state = MERGED;
		else if (!open)
			p.state = CLOSED;
		else
			p.state = json_bool(json_get(j, "draft")) ? DRAFT : OPEN;
	}
	entry_free(&p.description);
	entry_set(&p.description, COMMENT, j, "created_at");
	p.loaded = true;
	json_free(j);
	fill(false);
}

static void comments_loaded(const char *data, long status)
{
	pulled();
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	if (j && j->kind == JSON_ARRAY) {
		entries_free(&p.list);
		for (const struct json *c = j->child; c; c = c->next)
			entries_add(&p.list, COMMENT, c, "created_at");
		fill(false);
	}
	json_free(j);
}

static void reviews_loaded(const char *data, long status)
{
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	if (j && j->kind == JSON_ARRAY) {
		entries_free(&p.reviews);
		static const struct {
			const char *api, *text, *symbol;
			uint32_t color;
		} states[] = {
			/* NON-NLS-BEGIN: the API's states */
			{"APPROVED", N_("%s approved"), UI_SYMBOL_CIRCLE_CHECK, UI_COLOR_GREEN},
			{"CHANGES_REQUESTED", N_("%s requested changes"), UI_SYMBOL_CIRCLE_XMARK, UI_COLOR_RED},
			{"COMMENTED", N_("%s reviewed"), UI_SYMBOL_CIRCLE, 0},
			{"DISMISSED", N_("%s reviewed (dismissed)"), UI_SYMBOL_CIRCLE_MINUS, 0},
			/* NON-NLS-END */
		};
		for (const struct json *r = j->child; r; r = r->next) {
			const char *state = json_str(json_get(r, "state"), "");
			/* PENDING: the account's review, not submitted */
			for (size_t i = 0; i < LV_ARRAYLEN(states); i++) {
				if (strcmp(state, states[i].api))
					continue;
				entries_add(&p.reviews, REVIEW, r, "submitted_at");
				struct entry *e = &p.reviews.e[p.reviews.count - 1];
				e->review = states[i].text;
				e->review_symbol = states[i].symbol;
				e->review_color = states[i].color;
			}
		}
		fill(false);
	}
	json_free(j);
}

/* A pull request's checks, the failed first, then pending, passed, skipped; in
 * GitHub's order within each. Whether the account can merge it, and how */
static void status_loaded(const char *data, long status)
{
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	char error[128];
	const struct json *repo = json_get(j ? graphql_data(j, error, sizeof(error)) : NULL, "repository");
	const struct json *pr = json_get(repo, "pullRequest");
	const char *permission = json_str(json_get(repo, "viewerPermission"), "");
	p.can_merge = !strcmp(permission, "ADMIN") || !strcmp(permission, "MAINTAIN") ||
		      !strcmp(permission, "WRITE");
	p.can_triage = p.can_merge || !strcmp(permission, "TRIAGE");
	for (enum merge m = 0; m < MERGES; m++)
		p.merge_allowed[m] = json_bool(json_get(repo, merges[m].allowed));
	p.conflicts = !strcmp(json_str(json_get(pr, "mergeable"), ""), "CONFLICTING");
	const struct json *commits = json_get(json_get(pr, "commits"), "nodes");
	const struct json *rollup = json_get(json_get(commits ? commits->child : NULL, "commit"), "statusCheckRollup");
	const struct json *nodes = json_get(json_get(rollup, "contexts"), "nodes");
	p.check_count = 0;
	for (enum check s = CHECK_FAILED; s < CHECK_NONE; s++) {
		for (const struct json *c = nodes ? nodes->child : NULL; c && p.check_count < CHECKS; c = c->next) {
			if (check_state(c) != s)
				continue;
			const char *name = json_str(json_get(c, "name"), json_str(json_get(c, "context"), ""));
			p.checks[p.check_count].name = ui_intern(name);
			p.checks[p.check_count++].state = s;
		}
	}
	json_free(j);
	if (p.check_count || p.can_triage)
		fill(false);
}

/* Each check on a screen of its own, as they were when tapped: its state's icon, its
 * name */
static void checks_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Checks"), &header, &body);
	lv_obj_set_style_pad_all(body, 16, 0);
	lv_obj_t *card = ui_card_create(body);
	for (unsigned i = 0; i < p.check_count; i++) {
		lv_obj_t *row = ui_row_create(card, 80, i + 1 < p.check_count, NULL, NULL);
		check_icon_set(lv_label_create(row), p.checks[i].state);
		lv_obj_t *name = ui_one_line_label(row, p.checks[i].name, UI_FONT_BODY);
		lv_obj_set_flex_grow(name, 1);
	}
	ui_push(scr);
}

/* A row: how many failed, are pending, or all passed; tapped, each */
static void checks_row(lv_obj_t *card, bool divider)
{
	unsigned count[CHECK_NONE + 1] = {0};
	for (unsigned i = 0; i < p.check_count; i++)
		count[p.checks[i].state]++;
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, checks_cb, NULL);
	lv_obj_t *icon = lv_label_create(row);
	lv_obj_t *text = lv_label_create(row);
	lv_obj_set_style_text_font(text, UI_FONT_BODY, 0);
	lv_obj_set_flex_grow(text, 1);
	if (count[CHECK_FAILED]) {
		check_icon_set(icon, CHECK_FAILED);
		ui_set_text_fmt(text, P_("%u check failed", "%u checks failed", count[CHECK_FAILED]),
				count[CHECK_FAILED]);
	} else if (count[CHECK_PENDING]) {
		check_icon_set(icon, CHECK_PENDING);
		ui_set_text_fmt(text, P_("%u check pending", "%u checks pending", count[CHECK_PENDING]),
				count[CHECK_PENDING]);
	} else {
		check_icon_set(icon, CHECK_PASSED);
		lv_label_set_text_static(text, _("All checks passed"));
	}
	ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
}

static void milestone_row(lv_obj_t *card);

/* Under the card: a pull request's checks, the milestone (to who can set one even
 * without one); why it was not set */
static void rows_card(lv_obj_t *parent)
{
	bool checks = p.type == PR && p.check_count;
	bool milestone = p.type != DISCUSSION && (p.milestone || p.can_triage);
	if (!checks && !milestone)
		return;
	lv_obj_t *card = ui_card_create(parent);
	if (checks)
		checks_row(card, milestone);
	if (milestone)
		milestone_row(card);
	error_label(parent, p.milestone_error);
}

static void merged(const char *data, long status)
{
	struct json *j = data ? json_parse(data) : NULL;
	/* Its pull request still open */
	bool here = p.scr && !strcmp(merge_path, p.path);
	if (status == 200 && json_bool(json_get(j, "merged"))) {
		if (here)
			p.state = MERGED;
		/* "/repos/o/r/pulls/7": its row in the list below */
		char owner[96], name[96], repo[200];
		long long number;
		if (sscanf(merge_path, "/repos/%95[^/]/%95[^/]/pulls/%lld", owner, name, &number) == 3) { /* NON-NLS */
			snprintf(repo, sizeof(repo), "%s/%s", owner, name);
			list_merged(repo, number);
		}
	} else {
		const char *error = json_str(json_get(j, "message"), "");
		printf("github: %s not merged: HTTP %ld %s\n", merge_path, status, error);
		if (here)
			failure_format(p.merge_error, sizeof(p.merge_error), _("Not merged: %s"), status, error);
	}
	json_free(j);
	merge_path[0] = '\0';
	if (here && !refused(status))
		fill(false);
}

static void merge_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	p.merge_open = true;
	p.merge_error[0] = '\0';
	fill(false);
}

static void merge_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	p.merge_open = false;
	fill(false);
}

/* Merged that way, the head shown */
static void merge_method_cb(lv_event_t *e)
{
	enum merge m = (enum merge)(uintptr_t)lv_event_get_user_data(e);
	if (merge_fetch.curl)
		return;
	char path[sizeof(p.path) + 8], body[128];
	snprintf(path, sizeof(path), "%s/merge", p.path);
	if (p.head_sha[0])
		snprintf(body, sizeof(body), "{\"merge_method\":\"%s\",\"sha\":\"%s\"}", merges[m].method,
			 p.head_sha);
	else
		snprintf(body, sizeof(body), "{\"merge_method\":\"%s\"}", merges[m].method);
	snprintf(merge_path, sizeof(merge_path), "%s", p.path);
	p.merge_open = false;
	api_request(&merge_fetch, "PUT", path, body, merged);
	fill(false);
}

/* Red, wrapping, in the card: what failed (failure_format()), nothing if "" */
static void error_label(lv_obj_t *card, const char *text)
{
	if (!text[0])
		return;
	lv_obj_t *error = lv_label_create(card);
	ui_set_text(error, text);
	lv_obj_set_width(error, LV_PCT(100));
	lv_label_set_long_mode(error, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(error, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(error, lv_color_hex(UI_COLOR_RED), 0);
}

/* Under the checks, when the account can merge: its conflicts, or the button, the
 * ways to merge once tapped, then why the last merge failed */
static void merge_show(lv_obj_t *card)
{
	if (!p.can_merge)
		return;
	if (p.conflicts) {
		lv_obj_t *line = ui_row_line(card);
		check_icon_set(lv_label_create(line), CHECK_FAILED);
		lv_obj_t *text = lv_label_create(line);
		lv_label_set_text_static(text, _("This branch has conflicts"));
		lv_obj_set_flex_grow(text, 1);
		return;
	}
	if (merge_fetch.curl && !strcmp(merge_path, p.path)) {
		ui_dim_label(card, _("Merging..."), UI_FONT_BODY);
	} else if (!p.merge_open) {
		ui_button(card, LV_PCT(100), 80, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, _("Merge pull request"),
			  merge_cb, NULL);
	} else {
		for (enum merge m = 0; m < MERGES; m++)
			if (p.merge_allowed[m])
				ui_button(card, LV_PCT(100), 80, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED,
					  _(merges[m].name), merge_method_cb, (void *)(uintptr_t)m);
		ui_button(card, LV_PCT(100), 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Cancel"),
			  merge_cancel_cb, NULL);
	}
	error_label(card, p.merge_error);
}

/* The issue's path (a pull request's as an issue): its comments' without them */
static void issue_path(char *out, size_t size)
{
	size_t n = strlen(p.target), suffix = strlen("/comments");
	snprintf(out, size, "%.*s", (int)(n > suffix ? n - suffix : 0), p.target);
}

static void milestone_set(const char *data, long status)
{
	struct json *j = data ? json_parse(data) : NULL;
	/* Its pull request or issue still open */
	char path[sizeof(milestone_path)] = "";
	if (p.scr && p.type != DISCUSSION)
		issue_path(path, sizeof(path));
	bool here = !strcmp(path, milestone_path);
	if (status == 200 && j && j->kind == JSON_OBJECT) {
		if (here)
			milestone_parse(j);
	} else {
		const char *error = json_str(json_get(j, "message"), "");
		printf("github: %s milestone not set: HTTP %ld %s\n", milestone_path, status, error);
		if (here)
			failure_format(p.milestone_error, sizeof(p.milestone_error), _("Milestone not set: %s"), status,
				       error);
	}
	json_free(j);
	milestone_path[0] = '\0';
	if (!here)
		return;
	p.milestone_saving = false;
	p.milestone_new = NULL;
	if (!refused(status))
		fill(false);
}

/* From milestone.c: number 0, none */
static void milestone_picked(long long number, const char *title)
{
	if (!p.scr || number == p.milestone || milestone_fetch.curl)
		return;
	char body[48];
	if (number)
		snprintf(body, sizeof(body), "{\"milestone\":%lld}", number);
	else
		snprintf(body, sizeof(body), "{\"milestone\":null}");
	issue_path(milestone_path, sizeof(milestone_path));
	p.milestone_saving = true;
	p.milestone_new = title;
	p.milestone_error[0] = '\0';
	api_request(&milestone_fetch, "PATCH", milestone_path, body, milestone_set);
	fill(false);
}

static void milestone_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!p.milestone_saving)
		milestone_pick(p.repo, p.milestone, milestone_picked);
}

/* A row: "Milestone", then its title on the right ("No milestone" only shown to who can
 * set one, tapped: the others), the one being set paler */
static void milestone_row(lv_obj_t *card)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, false, p.can_triage ? milestone_cb : NULL, NULL);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, _("Milestone"));
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	const char *title = p.milestone_saving ? p.milestone_new : p.milestone_title;
	lv_obj_t *value = ui_one_line_label(row, title ? title : _("No milestone"), UI_FONT_BODY);
	lv_obj_set_flex_grow(value, 1);
	lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_color(value, lv_color_hex(UI_COLOR_DIM), 0);
	if (p.milestone_saving)
		lv_obj_set_style_text_opa(value, LV_OPA_50, 0);
	if (p.can_triage)
		ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
}

/* The discussion's data, NULL (failed, and shown so) if it has none */
static const struct json *discussion_answer(const struct json *j, long status)
{
	const struct json *data = j ? graphql_data(j, p.error_text, sizeof(p.error_text)) : NULL;
	if (!data) {
		p.failed = true;
		p.error = status;
		if (status == 200 && !p.error_text[0])
			snprintf(p.error_text, sizeof(p.error_text), _("GitHub's answer has no discussion"));
		fill(false);
	}
	return data;
}

static void discussion_loaded(const char *data, long status)
{
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	const struct json *d = json_get(json_get(discussion_answer(j, status), "repository"), "discussion");
	if (!d || d->kind != JSON_OBJECT) {
		if (!p.failed) {
			p.failed = p.not_found = true;
			fill(false);
		}
		json_free(j);
		return;
	}
	p.title = ui_intern(json_str(json_get(d, "title"), p.title));
	snprintf(p.target, sizeof(p.target), "%s", json_str(json_get(d, "id"), ""));
	const char *reason = json_str(json_get(d, "stateReason"), "");
	if (!json_bool(json_get(d, "closed")))
		p.state = json_bool(json_get(d, "isAnswered")) ? ANSWERED : OPEN;
	else if (!strcmp(reason, "OUTDATED"))
		p.state = OUTDATED;
	else if (!strcmp(reason, "DUPLICATE"))
		p.state = DUPLICATE;
	else
		p.state = DONE;
	const char *category = json_str(json_get(json_get(d, "category"), "name"), NULL);
	p.label_count = 0;
	if (category) {
		p.labels[0].name = ui_intern(category);
		p.labels[p.label_count++].color = COLOR_GRAY;
	}

	entry_free(&p.description);
	entry_set_graphql(&p.description, d);
	entries_free(&p.list);
	const struct json *comments = json_get(json_get(d, "comments"), "nodes");
	for (const struct json *c = comments ? comments->child : NULL; c; c = c->next) {
		struct entry *e = entries_new(&p.list);
		if (!e)
			break;
		entry_set_graphql(e, c);
		e->answer = json_bool(json_get(c, "isAnswer"));
		const char *to = ui_intern(e->login);
		const struct json *replies = json_get(json_get(c, "replies"), "nodes");
		for (const struct json *r = replies ? replies->child : NULL; r; r = r->next) {
			struct entry *reply = entries_new(&p.list);
			if (!reply)
				break;
			entry_set_graphql(reply, r);
			reply->reply_to = to;
		}
	}
	p.loaded = true;
	json_free(j);
	fill(false);
}

/* The latest with the title, then the discussion by its number */
static void discussion_found(const char *data, long status)
{
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	const struct json *nodes = json_get(json_get(discussion_answer(j, status), "search"), "nodes");
	long long number = -1;
	const char *latest = "";
	for (const struct json *d = nodes ? nodes->child : NULL; d; d = d->next) {
		const char *updated = json_str(json_get(d, "updatedAt"), "");
		/* ISO 8601 in UTC: in order as text */
		if (!strcmp(json_str(json_get(d, "title"), ""), p.title) && strcmp(updated, latest) >= 0) {
			number = json_int(json_get(d, "number"), -1);
			latest = updated;
		}
	}
	if (number < 0) {
		if (!p.failed) {
			p.failed = p.not_found = true;
			fill(false);
		}
		json_free(j);
		return;
	}

	char key[sizeof(p.path) + 24];
	snprintf(key, sizeof(key), "%s#%lld", p.path, number);
	p.key = ui_intern(key);
	/* "owner/name" */
	char owner[96], name[128], variables[512];
	const char *slash = strchr(p.path, '/');
	snprintf(owner, sizeof(owner), "%.*s", slash ? (int)(slash - p.path) : 0, p.path);
	json_escape(name, sizeof(name), slash ? slash + 1 : "");
	char escaped[192];
	json_escape(escaped, sizeof(escaped), owner);
	snprintf(variables, sizeof(variables), "{\"owner\":\"%s\",\"name\":\"%s\",\"number\":%lld}", escaped, name,
		 number);
	graphql(&p.pr, DISCUSSION_QUERY, variables, discussion_loaded);
	json_free(j);
	fill(false);
}

static void me_loaded(const char *data, long status)
{
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	snprintf(me, sizeof(me), "%s", json_str(json_get(j, "login"), ""));
	json_free(j);
	if (me[0])
		fill(false);
}

static void sent(const char *data, long status)
{
	struct json *j = data ? json_parse(data) : NULL;
	/* Its pull request, issue or discussion still open */
	bool here = p.scr && !strcmp(send_to, p.target);
	const struct json *comment = NULL;
	char error[160] = "";
	if (send_graphql && status == 200) {
		const struct json *d = graphql_data(j, error, sizeof(error));
		comment = json_get(json_get(d, "addDiscussionComment"), "comment");
	} else if (!send_graphql && status == 201) {
		comment = j;
	} else {
		snprintf(error, sizeof(error), "%s", json_str(json_get(j, "message"), ""));
	}
	bool ok = comment && comment->kind == JSON_OBJECT;
	if (ok && here) {
		struct entry *e = entries_new(&p.list);
		if (e && send_graphql)
			entry_set_graphql(e, comment);
		else if (e)
			entry_set(e, COMMENT, comment, "created_at");
	} else if (!ok && here) {
		failure_format(p.send_error, sizeof(p.send_error), _("Not sent: %s"), status, error);
		/* Back in the field, to send again */
		if (p.sending && !lv_textarea_get_text(p.chat.ta)[0])
			lv_textarea_set_text(p.chat.ta, p.sending);
	}
	if (!ok)
		printf("github: comment not sent: HTTP %ld %s\n", status, error);
	json_free(j);
	if (!here)
		return;
	free(p.sending);
	p.sending = NULL;
	if (!refused(status))
		fill(true);
}

static void send(struct ui_chat *chat, const char *text)
{
	if (send_fetch.curl || !p.target[0]) {
		/* One at a time, or the discussion not loaded yet: kept for later */
		lv_textarea_set_text(chat->ta, text);
		return;
	}
	size_t n = strlen(text);
	char *escaped = malloc(n * 6 + 1), *body = malloc(n * 6 + sizeof(p.target) + 32);
	if (escaped && body) {
		json_escape(escaped, n * 6 + 1, text);
		free(p.sending);
		p.sending = strdup(text);
		p.send_error[0] = '\0';
		snprintf(send_to, sizeof(send_to), "%s", p.target);
		send_graphql = p.type == DISCUSSION;
		if (send_graphql) {
			/* Node IDs are letters, digits, _ and - */
			sprintf(body, "{\"id\":\"%s\",\"body\":\"%s\"}", p.target, escaped);
			graphql(&send_fetch, COMMENT_MUTATION, body, sent);
		} else {
			sprintf(body, "{\"body\":\"%s\"}", escaped);
			api_request_as(&send_fetch, FULL, "POST", p.target, body, sent);
		}
		fill(true);
	}
	free(escaped);
	free(body);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&p.pr);
	ui_fetch_stop(&p.comments_fetch);
	ui_fetch_stop(&p.reviews_fetch);
	ui_fetch_stop(&p.status_fetch);
	ui_fetch_stop(&p.files_fetch);
	/* A comment, a merge or a milestone on its way still goes */
	entry_free(&p.description);
	entries_free(&p.list);
	entries_free(&p.reviews);
	free(p.sending);
	memset(&p, 0, sizeof(p));
}

void pr_forget_account(void)
{
	ui_fetch_stop(&me_fetch);
	me[0] = '\0';
}

static void files_loaded(const char *data, long status);

static void files_load(void)
{
	char path[224];
	snprintf(path, sizeof(path), "%s/files?per_page=100", p.path);
	api_request(&p.files_fetch, "GET", path, NULL, files_loaded);
}

/* The files tab again: the files, or why they did not come */
static void files_loaded(const char *data, long status)
{
	pulled();
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	lv_obj_clean(p.files);
	if (j && j->kind == JSON_ARRAY) {
		files_show(p.files, j);
	} else {
		lv_obj_t *label = centered_label(p.files, UI_FONT_BODY);
		if (!status)
			lv_label_set_text_static(label, _("Cannot reach GitHub"));
		else
			ui_set_text_fmt(label, _("GitHub answered with an error (HTTP %ld)"), status);
	}
	json_free(j);
}

/* A pull request's tab shown: the overview, the conversation and its compose bar, or
 * the files (asked for the first time) */
static void tab_select(enum tab tab)
{
	p.tab = tab;
	segments_select(p.tabs, TABS, tab);
	bool talk = tab == CONVERSATION;
	lv_obj_set_hidden(p.overview, tab != OVERVIEW);
	lv_obj_set_hidden(p.files, tab != FILES);
	lv_obj_set_hidden(p.chat.list, !talk);
	lv_obj_set_hidden(lv_obj_get_parent(p.chat.ta), !talk);
	if (!talk)
		ui_keyboard_show(p.chat.kb, false);
	if (tab == FILES && !p.files_asked) {
		p.files_asked = true;
		lv_obj_t *label = centered_label(p.files, UI_FONT_BODY);
		lv_label_set_text_static(label, _("Loading..."));
		files_load();
	}
}

static void tab_cb(lv_event_t *e)
{
	tab_select((enum tab)(uintptr_t)lv_event_get_user_data(e));
}

/* How many, beside a tab's name */
static lv_obj_t *count_label(lv_obj_t *tab)
{
	lv_obj_t *label = lv_label_create(tab);
	lv_label_set_text_static(label, "");
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
	return label;
}

/* A tab's scrolling column */
static lv_obj_t *tab_column(lv_obj_t *body)
{
	lv_obj_t *col = lv_obj_create(body);
	lv_obj_remove_style_all(col);
	lv_obj_set_width(col, LV_PCT(100));
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(col, 16, 0);
	lv_obj_set_style_pad_row(col, 16, 0);
	lv_obj_set_scroll_dir(col, LV_DIR_VER);
	return col;
}

/* A pull request's tabs, then its overview and its files, above the conversation */
static void tabs_create(lv_obj_t *body)
{
	static const char *const names[TABS] = {N_("Overview"), N_("Conversation"), N_("Files")};
	const char *translated[TABS] = {_(names[OVERVIEW]), _(names[CONVERSATION]), _(names[FILES])};
	segments_create(body, p.tabs, TABS, translated, tab_cb);
	/* How many messages, how many files */
	p.count = count_label(p.tabs[CONVERSATION]);
	p.files_count = count_label(p.tabs[FILES]);
	p.overview = tab_column(body);
	p.files = tab_column(body);
}

/* The screen, empty until it loads */
static bool screen_open(enum type type, const char *title)
{
	static const char *const names[] = {[PR] = N_("Pull request"), [ISSUE] = N_("Issue"),
					    [DISCUSSION] = N_("Discussion")};
	if (p.scr)
		return false;
	lv_obj_t *body;
	p.scr = ui_app_screen_create(_(names[type]), &p.header, &body);
	lv_obj_add_event_cb(p.scr, delete_cb, LV_EVENT_DELETE, NULL);
	p.type = type;
	p.title = ui_intern(title);

	if (type == PR)
		tabs_create(body);
	p.chat.send = send;
	ui_chat_create(&p.chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(p.chat.ta, _("Comment"));
	if (!me[0] && !me_fetch.curl)
		api_request(&me_fetch, "GET", "/user", NULL, me_loaded);
	return true;
}

static void repo_cb(lv_event_t *e)
{
	repo_open_name(lv_event_get_user_data(e));
}

/* The pull request or issue, its comments, reviews, what the account can do */
static void thread_load(void)
{
	const char *path = p.path, *repo = p.repo, *number = strrchr(path, '/') + 1;
	char reviews[224], comments[224];
	snprintf(reviews, sizeof(reviews), "%s/reviews?per_page=100", path);
	snprintf(comments, sizeof(comments), "%s?per_page=100", p.target);
	api_request_as(&p.pr, FULL, "GET", path, NULL, pr_loaded);
	api_request_as(&p.comments_fetch, FULL, "GET", comments, NULL, comments_loaded);
	if (p.type == PR)
		api_request_as(&p.reviews_fetch, FULL, "GET", reviews, NULL, reviews_loaded);
	/* What the account can do (merge, triage), a pull request's checks */
	char owner[96], name[96], variables[256];
	if (sscanf(repo, "%95[^/]/%95s", owner, name) == 2) { /* NON-NLS */
		if (p.type == PR) {
			snprintf(variables, sizeof(variables), "{\"owner\":\"%s\",\"name\":\"%s\",\"number\":%s}", owner,
				 name, number);
			graphql(&p.status_fetch, STATUS_QUERY, variables, status_loaded);
		} else {
			snprintf(variables, sizeof(variables), "{\"owner\":\"%s\",\"name\":\"%s\"}", owner, name);
			graphql(&p.status_fetch, PERMISSION_QUERY, variables, status_loaded);
		}
	}
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	thread_load();
	if (p.files_asked)
		files_load();
}

static void thread_open(enum type type, const char *path, const char *repo, const char *title, bool talk)
{
	/* "/repos/o/r/pulls/7", "/repos/o/r/issues/7" */
	const char *number = strrchr(path, '/');
	if (!number || !screen_open(type, title))
		return;
	number++;
	snprintf(p.path, sizeof(p.path), "%s", path);
	snprintf(p.target, sizeof(p.target), "/repos/%s/issues/%s/comments", repo, number);
	char key[160];
	snprintf(key, sizeof(key), "%s#%s", repo, number);
	p.key = ui_intern(key);
	p.repo = ui_intern(repo);
	/* Its repository, from the header */
	ui_header_button(p.header, LV_SYMBOL_DIRECTORY, repo_cb, (void *)p.repo);
	thread_load();
	/* Pulled down, each tab loads it all again */
	ui_pull_refresh(p.chat.list, pull_cb, NULL);
	if (type == PR) {
		ui_pull_refresh(p.overview, pull_cb, NULL);
		ui_pull_refresh(p.files, pull_cb, NULL);
	}
	if (type == PR)
		tab_select(talk ? CONVERSATION : OVERVIEW);
	fill(false);
	ui_push(p.scr);
}

void pr_open(const char *path, const char *repo, const char *title, bool talk)
{
	thread_open(PR, path, repo, title, talk);
}

void issue_open(const char *path, const char *repo, const char *title, bool talk)
{
	thread_open(ISSUE, path, repo, title, talk);
}

void discussion_open(const char *repo, const char *title)
{
	if (!screen_open(DISCUSSION, title))
		return;
	snprintf(p.path, sizeof(p.path), "%s", repo);
	p.key = ui_intern(repo);
	/* Its words as a phrase in its title: GitHub's search cannot have quotes in
	 * one, they go */
	char q[512], phrase[256], variables[1100];
	snprintf(phrase, sizeof(phrase), "%s", title);
	for (char *s = phrase; *s; s++)
		if (*s == '"' || *s == '\\')
			*s = ' ';
	snprintf(q, sizeof(q), "repo:%s in:title \"%s\"", repo, phrase); /* NON-NLS: GitHub's search */
	char escaped[sizeof(q) * 2];
	json_escape(escaped, sizeof(escaped), q);
	snprintf(variables, sizeof(variables), "{\"q\":\"%s\"}", escaped);
	graphql(&p.pr, SEARCH_QUERY, variables, discussion_found);
	fill(false);
	ui_push(p.scr);
}
