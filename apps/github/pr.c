/*
 * A pull request, an issue or a discussion: a card with its title, key
 * (owner/repo#7), state, and a pull request's branches, an issue's labels or a
 * discussion's category, then its conversation as a chat (ui_chat, as Messages
 * and Telegram): the description first, then the comments and a pull request's
 * reviews in the order they came, a discussion's comments each followed by its
 * replies; the account's own on the right. Comments are sent from the compose bar.
 * Each message is a bubble, its Markdown rendered (markdown.c), HTML comments
 * (templates' hints) left out. A pull request's files and commits are not shown yet.
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

#define MAX_LENGTH 65536 /* GitHub's limit for a comment */
#define MAX_MESSAGE 65536 /* bytes of a message's text: its labels are cut shorter (markdown.c) */

#define COLOR_MERGED 0x8957e5 /* GitHub's, also an issue closed as completed */
#define COLOR_GRAY 0x6e7681      /* a draft, an issue not planned */
#define LABELS 8                 /* an issue's labels shown */
#define REPLY_INDENT 48          /* a discussion's reply, from its comment */

/* GraphQL: a discussion's number by its title, the discussion, a comment on it.
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
#define COMMENT_MUTATION                                                                     \
	"mutation($id: ID!, $body: String!) {"                                               \
	" addDiscussionComment(input: {discussionId: $id, body: $body}) {"                   \
	" comment { " MESSAGE_FIELDS " } } }"

enum kind { COMMENT, REVIEW };
enum type { PR, ISSUE, DISCUSSION };

struct entry {
	enum kind kind;
	char *login, *body;
	char *html; /* GitHub's rendering of body, NULL if it gave none */
	const char *review; /* "approved"..., a review's */
	const char *reply_to; /* a discussion's reply: its comment's author, interned */
	bool answer;          /* a discussion's comment marked as its answer */
	time_t time;
};

struct entries {
	struct entry *e;
	unsigned count;
};

/* The open pull request, issue or discussion */
static struct {
	lv_obj_t *scr, *header;
	struct ui_chat chat;
	enum type type;
	char path[192];   /* "/repos/o/r/pulls/7", "/repos/o/r/issues/7"; "o/r" for a discussion */
	char target[192]; /* where comments go: "/repos/o/r/issues/7/comments", a discussion's node ID */
	const char *title, *key, *base, *head; /* interned */
	enum { OPEN, DRAFT, MERGED, CLOSED, DONE, NOT_PLANNED, ANSWERED, OUTDATED, DUPLICATE } state;
	struct {
		const char *name; /* interned */
		uint32_t color;
	} labels[LABELS];
	unsigned label_count;
	struct entry description;
	struct entries list, reviews; /* comments, reviews */
	bool loaded;   /* the pull request, issue or discussion */
	long error;    /* its HTTP status if it failed, 0: none or offline */
	char error_text[192]; /* or GraphQL's reason */
	bool failed, not_found;
	char *sending; /* a comment on its way */
	char send_error[192];
	struct ui_fetch pr, comments_fetch, reviews_fetch;
} p;

/* Outlive the screen */
static struct ui_fetch send_fetch, me_fetch;
static char send_to[192]; /* the target it goes to */
static bool send_graphql; /* to a discussion */
static char me[64]; /* the account's login, "" until known */

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

/* Centered, dim: reviews, errors */
static lv_obj_t *centered_label(const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(p.chat.list);
	lv_label_set_text_static(label, "");
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

/* A message in a bubble (markdown.c: its Markdown, or GitHub's HTML of it): without
 * \r and HTML comments (templates' hints), cut at MAX_MESSAGE bytes */
static void bubble(lv_obj_t *group, bool outgoing, const char *body, const char *html)
{
	/* Room for the ellipsis after a cut */
	char *text = malloc(strlen(body) + 4), *out = text;
	if (!text)
		return;
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
	markdown_bubble(group, outgoing, text, html);
	free(text);
}

/* A comment, a review's text or the description: the author's name above it
 * unless it is the account's, the time below. A discussion's answer has a line
 * above it, a reply is set in and says whose comment it answers */
static void message(const struct entry *e)
{
	if (e->answer) {
		lv_obj_t *label = centered_label(&ui_font_20);
		lv_label_set_text_static(label, LV_SYMBOL_OK " Answer");
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_GREEN), 0);
	}
	bool outgoing = me[0] && !strcmp(e->login, me);
	lv_obj_t *time;
	lv_obj_t *group = ui_chat_group_create(&p.chat, outgoing, &time);
	if (e->reply_to)
		lv_obj_set_style_pad_left(group, REPLY_INDENT, 0);
	if (!outgoing) {
		lv_obj_t *name = lv_label_create(group);
		if (e->reply_to)
			ui_set_text_fmt(name, "%s, replying to %s", e->login, e->reply_to);
		else
			ui_set_text(name, e->login);
		lv_obj_set_style_text_font(name, &ui_font_20, 0);
		lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_ACCENT), 0);
		lv_obj_set_style_pad_hor(name, 12, 0);
		lv_obj_move_to_index(name, 0);
	}
	char when[32] = "";
	if (e->time)
		ui_when_format(when, sizeof(when), e->time);
	ui_set_text(time, when);
	bubble(group, outgoing, e->body, e->html);
}

static void review(const struct entry *e)
{
	lv_obj_t *label = centered_label(&ui_font_20);
	ui_set_text_fmt(label, "%s %s", e->login, e->review);
	if (e->body[0])
		message(e);
}

/* A state or a label: static text on a rounded color, the text black or white,
 * whichever shows on it */
static void pill(lv_obj_t *line, const char *text, uint32_t color)
{
	lv_obj_t *pill = ui_box(line);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(pill, 16, 0);
	lv_obj_set_style_pad_ver(pill, 6, 0);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(color), 0);
	lv_obj_t *label = lv_label_create(pill);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	unsigned luma = ((color >> 16) * 299 + (color >> 8 & 0xff) * 587 + (color & 0xff) * 114) / 1000;
	lv_obj_set_style_text_color(label, luma > 160 ? lv_color_black() : lv_color_white(), 0);
}

/* The card at the top */
static void summary(void)
{
	lv_obj_t *card = ui_card_create(p.chat.list);
	lv_obj_set_style_pad_all(card, 24, 0);
	lv_obj_set_style_pad_row(card, 12, 0);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text_static(title, p.title);
	lv_obj_set_width(title, LV_PCT(100));
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(title, &ui_font_26, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);

	if (p.key)
		ui_dim_label(card, p.key, &ui_font_20);

	if (!p.loaded)
		return;
	lv_obj_t *line = ui_row_line(card);
	static const struct {
		const char *text;
		uint32_t color;
	} states[] = {
		[OPEN] = {"Open", UI_COLOR_GREEN},
		[DRAFT] = {"Draft", COLOR_GRAY},
		[MERGED] = {"Merged", COLOR_MERGED},
		[CLOSED] = {"Closed", UI_COLOR_RED},
		[DONE] = {"Closed", COLOR_MERGED},
		[NOT_PLANNED] = {"Not planned", COLOR_GRAY},
		[ANSWERED] = {"Answered", UI_COLOR_GREEN},
		[OUTDATED] = {"Outdated", COLOR_GRAY},
		[DUPLICATE] = {"Duplicate", COLOR_GRAY},
	};
	pill(line, states[p.state].text, states[p.state].color);

	if (p.type == PR) {
		lv_obj_t *branches = ui_dim_label(line, "", &ui_font_20);
		ui_set_text_fmt(branches, "Into %s from %s", p.base, p.head);
		lv_obj_set_flex_grow(branches, 1);
		lv_label_set_long_mode(branches, LV_LABEL_LONG_MODE_WRAP);
		return;
	}
	/* The labels (a discussion's category), in their colors, wrapping */
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(line, 10, 0);
	lv_obj_set_style_pad_column(line, 10, 0);
	for (unsigned i = 0; i < p.label_count; i++)
		pill(line, p.labels[i].name, p.labels[i].color);
}

static int by_time(const void *a, const void *b)
{
	const struct entry *x = *(const struct entry *const *)a, *y = *(const struct entry *const *)b;
	return (x->time > y->time) - (x->time < y->time);
}

/* Everything again, from what came so far; the scroll position stays */
static void fill(bool to_end)
{
	if (!p.scr)
		return;
	lv_obj_update_layout(p.chat.list);
	int32_t y = lv_obj_get_scroll_y(p.chat.list);
	lv_obj_clean(p.chat.list);

	summary();
	if (!p.loaded) {
		lv_obj_t *label = centered_label(&ui_font_24);
		if (!p.failed)
			lv_label_set_text_static(label, "Loading...");
		else if (p.not_found)
			lv_label_set_text_static(label, "Discussion not found");
		else if (p.error_text[0])
			ui_set_text(label, p.error_text);
		else if (!p.error)
			lv_label_set_text_static(label, "Cannot reach GitHub");
		else
			ui_set_text_fmt(label, "GitHub answered with an error (HTTP %ld)", p.error);
		return;
	}

	if (p.description.body[0]) {
		message(&p.description);
	} else {
		lv_obj_t *label = centered_label(&ui_font_20);
		lv_label_set_text_static(label, "No description");
	}

	/* Comments and reviews in their order; a discussion's as they came, replies
	 * after their comment */
	unsigned count = p.list.count + p.reviews.count;
	const struct entry **all = malloc((count ? count : 1) * sizeof(*all));
	if (all) {
		for (unsigned i = 0; i < p.list.count; i++)
			all[i] = &p.list.e[i];
		for (unsigned i = 0; i < p.reviews.count; i++)
			all[p.list.count + i] = &p.reviews.e[i];
		if (count && p.type != DISCUSSION)
			qsort(all, count, sizeof(*all), by_time);
		for (unsigned i = 0; i < count; i++) {
			if (all[i]->kind == REVIEW)
				review(all[i]);
			else
				message(all[i]);
		}
		free(all);
	}

	if (p.sending) {
		lv_obj_t *time;
		lv_obj_t *group = ui_chat_group_create(&p.chat, true, &time);
		lv_label_set_text_static(time, "Sending...");
		bubble(group, true, p.sending, NULL);
		/* Not on GitHub yet: paler */
		for (uint32_t i = 0; i + 1 < lv_obj_get_child_count(group); i++)
			lv_obj_set_style_bg_opa(lv_obj_get_child(group, i), LV_OPA_60, 0);
	}
	if (p.send_error[0]) {
		lv_obj_t *label = centered_label(&ui_font_20);
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

/* 401: the token is no longer good */
static bool refused(long status)
{
	if (status != 401)
		return false;
	/* The others would refuse it too: popped once */
	ui_fetch_stop(&p.pr);
	ui_fetch_stop(&p.comments_fetch);
	ui_fetch_stop(&p.reviews_fetch);
	ui_pop();
	github_sign_out("GitHub refused this token.");
	return true;
}

static void pr_loaded(const char *data, long status)
{
	if (refused(status))
		return;
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	if (!j || j->kind != JSON_OBJECT) {
		p.failed = true;
		p.error = status;
		json_free(j);
		fill(false);
		return;
	}
	p.title = ui_intern(json_str(json_get(j, "title"), p.title));
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
			const char *api, *text;
		} states[] = {
			{"APPROVED", "approved"},
			{"CHANGES_REQUESTED", "requested changes"},
			{"COMMENTED", "reviewed"},
			{"DISMISSED", "reviewed (dismissed)"},
		};
		for (const struct json *r = j->child; r; r = r->next) {
			const char *state = json_str(json_get(r, "state"), "");
			/* PENDING: the account's review, not submitted */
			for (size_t i = 0; i < LV_ARRAYLEN(states); i++) {
				if (strcmp(state, states[i].api))
					continue;
				entries_add(&p.reviews, REVIEW, r, "submitted_at");
				p.reviews.e[p.reviews.count - 1].review = states[i].text;
			}
		}
		fill(false);
	}
	json_free(j);
}

/* A GraphQL request: `variables` a JSON object */
static void graphql(struct ui_fetch *f, const char *query, const char *variables,
		    void (*done)(const char *data, long status))
{
	size_t n = strlen(query) + strlen(variables) + 32;
	char *body = malloc(n);
	if (!body)
		return;
	snprintf(body, n, "{\"query\":\"%s\",\"variables\":%s}", query, variables);
	api_request(f, "POST", "/graphql", body, done);
	free(body);
}

/* A GraphQL answer's data, NULL with `error` set to its first error's message (""
 * if it has none) when it has none */
static const struct json *graphql_data(const struct json *j, char *error, size_t size)
{
	const struct json *data = json_get(j, "data"), *errors = json_get(j, "errors");
	snprintf(error, size, "%s", json_str(json_get(errors ? errors->child : NULL, "message"), ""));
	return data && data->kind == JSON_OBJECT ? data : NULL;
}

/* The discussion's data, NULL (failed, and shown so) if it has none */
static const struct json *discussion_answer(const struct json *j, long status)
{
	const struct json *data = j ? graphql_data(j, p.error_text, sizeof(p.error_text)) : NULL;
	if (!data) {
		p.failed = true;
		p.error = status;
		if (status == 200 && !p.error_text[0])
			snprintf(p.error_text, sizeof(p.error_text), "GitHub's answer has no discussion");
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
		if (!status)
			snprintf(p.send_error, sizeof(p.send_error), LV_SYMBOL_WARNING " Not sent: no connection");
		else if (!error[0])
			snprintf(p.send_error, sizeof(p.send_error), LV_SYMBOL_WARNING " Not sent: HTTP %ld", status);
		else
			snprintf(p.send_error, sizeof(p.send_error), LV_SYMBOL_WARNING " Not sent: %s", error);
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
	/* A comment on its way still goes */
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

/* The screen, empty until it loads */
static bool screen_open(enum type type, const char *title)
{
	static const char *const names[] = {[PR] = "Pull request", [ISSUE] = "Issue", [DISCUSSION] = "Discussion"};
	if (p.scr)
		return false;
	lv_obj_t *body;
	p.scr = ui_app_screen_create(names[type], &p.header, &body);
	lv_obj_add_event_cb(p.scr, delete_cb, LV_EVENT_DELETE, NULL);
	p.type = type;
	p.title = ui_intern(title);

	p.chat.send = send;
	ui_chat_create(&p.chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(p.chat.ta, "Comment");
	if (!me[0] && !me_fetch.curl)
		api_request(&me_fetch, "GET", "/user", NULL, me_loaded);
	return true;
}

static void repo_cb(lv_event_t *e)
{
	repo_open_name(lv_event_get_user_data(e));
}

static void thread_open(enum type type, const char *path, const char *repo, const char *title)
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
	/* Its repository, from the header */
	ui_header_button(p.header, LV_SYMBOL_DIRECTORY, repo_cb, (void *)ui_intern(repo));

	char reviews[224], comments[224];
	snprintf(reviews, sizeof(reviews), "%s/reviews?per_page=100", path);
	snprintf(comments, sizeof(comments), "%s?per_page=100", p.target);
	api_request_as(&p.pr, FULL, "GET", path, NULL, pr_loaded);
	api_request_as(&p.comments_fetch, FULL, "GET", comments, NULL, comments_loaded);
	if (type == PR)
		api_request_as(&p.reviews_fetch, FULL, "GET", reviews, NULL, reviews_loaded);
	fill(false);
	ui_push(p.scr);
}

void pr_open(const char *path, const char *repo, const char *title)
{
	thread_open(PR, path, repo, title);
}

void issue_open(const char *path, const char *repo, const char *title)
{
	thread_open(ISSUE, path, repo, title);
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
	snprintf(q, sizeof(q), "repo:%s in:title \"%s\"", repo, phrase);
	char escaped[sizeof(q) * 2];
	json_escape(escaped, sizeof(escaped), q);
	snprintf(variables, sizeof(variables), "{\"q\":\"%s\"}", escaped);
	graphql(&p.pr, SEARCH_QUERY, variables, discussion_found);
	fill(false);
	ui_push(p.scr);
}
