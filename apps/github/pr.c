/*
 * A pull request: a card with its title, key (owner/repo#7), state and
 * branches, then its conversation as a chat (ui_chat, as Messages and
 * Telegram): the description first, then the comments and reviews in the order
 * they came, the account's own on the right. Comments are sent from the
 * compose bar. Each message is a bubble, its Markdown rendered (markdown.c),
 * HTML comments (templates' hints) left out. Files and commits are
 * not shown yet.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "github.h"

#define MAX_LENGTH 65536 /* GitHub's limit for a comment */
#define MAX_MESSAGE 3000 /* bytes in a bubble: a label is a texture as large */

#define COLOR_MERGED 0x8957e5 /* GitHub's */

enum kind { COMMENT, REVIEW };

struct entry {
	enum kind kind;
	char *login, *body;
	const char *review; /* "approved"..., a review's */
	time_t time;
};

struct entries {
	struct entry *e;
	unsigned count;
};

/* The open pull request */
static struct {
	lv_obj_t *scr;
	struct ui_chat chat;
	char path[192];     /* "/repos/o/r/pulls/7" */
	char comments[192]; /* "/repos/o/r/issues/7/comments" */
	const char *title, *key, *base, *head; /* interned */
	enum { OPEN, DRAFT, MERGED, CLOSED } state;
	struct entry description;
	struct entries list, reviews; /* comments, reviews */
	bool loaded;   /* the pull request */
	long error;    /* its HTTP status if it failed, 0: none or offline */
	bool failed;
	char *sending; /* a comment on its way */
	char send_error[192];
	struct ui_fetch pr, comments_fetch, reviews_fetch;
} p;

/* Outlive the screen */
static struct ui_fetch send_fetch, me_fetch;
static char send_to[192]; /* the comments' path it goes to */
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
	memset(e, 0, sizeof(*e));
}

static void entries_free(struct entries *l)
{
	for (unsigned i = 0; i < l->count; i++)
		entry_free(&l->e[i]);
	free(l->e);
	memset(l, 0, sizeof(*l));
}

static void entry_set(struct entry *e, enum kind kind, const struct json *j, const char *time_key)
{
	e->kind = kind;
	e->login = strdup(json_str(json_get(json_get(j, "user"), "login"), ""));
	e->body = strdup(json_str(json_get(j, "body"), ""));
	e->time = parse_time(json_str(json_get(j, time_key), ""));
}

static void entries_add(struct entries *l, enum kind kind, const struct json *j, const char *time_key)
{
	struct entry *e = realloc(l->e, (l->count + 1) * sizeof(*e));
	if (!e)
		return;
	l->e = e;
	memset(&e[l->count], 0, sizeof(*e));
	entry_set(&e[l->count++], kind, j, time_key);
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

/* A message in a bubble, its Markdown rendered (markdown.c): without \r and HTML
 * comments (templates' hints), cut at MAX_MESSAGE bytes */
static void bubble(lv_obj_t *group, bool outgoing, const char *body)
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
	markdown_bubble(group, outgoing, text);
	free(text);
}

/* A comment, a review's text or the description: the author's name above it
 * unless it is the account's, the time below */
static void message(const struct entry *e)
{
	bool outgoing = me[0] && !strcmp(e->login, me);
	lv_obj_t *time;
	lv_obj_t *group = ui_chat_group_create(&p.chat, outgoing, &time);
	if (!outgoing) {
		lv_obj_t *name = lv_label_create(group);
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
	bubble(group, outgoing, e->body);
}

static void review(const struct entry *e)
{
	lv_obj_t *label = centered_label(&ui_font_20);
	ui_set_text_fmt(label, "%s %s", e->login, e->review);
	if (e->body[0])
		message(e);
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

	lv_obj_t *key = ui_dim_label(card, p.key, &ui_font_20);
	LV_UNUSED(key);

	if (!p.loaded)
		return;
	lv_obj_t *line = ui_row_line(card);
	static const struct {
		const char *text;
		uint32_t color;
	} states[] = {
		[OPEN] = {"Open", UI_COLOR_GREEN},
		[DRAFT] = {"Draft", 0x6e7681},
		[MERGED] = {"Merged", COLOR_MERGED},
		[CLOSED] = {"Closed", UI_COLOR_RED},
	};
	lv_obj_t *pill = ui_box(line);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(pill, 16, 0);
	lv_obj_set_style_pad_ver(pill, 6, 0);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pill, lv_color_hex(states[p.state].color), 0);
	lv_obj_t *state = lv_label_create(pill);
	lv_label_set_text_static(state, states[p.state].text);
	lv_obj_set_style_text_font(state, &ui_font_20, 0);
	lv_obj_set_style_text_color(state, lv_color_white(), 0);

	lv_obj_t *branches = ui_dim_label(line, "", &ui_font_20);
	ui_set_text_fmt(branches, "Into %s from %s", p.base, p.head);
	lv_obj_set_flex_grow(branches, 1);
	lv_label_set_long_mode(branches, LV_LABEL_LONG_MODE_WRAP);
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

	/* Comments and reviews in their order (sorted stably: by index too) */
	unsigned count = p.list.count + p.reviews.count;
	const struct entry **all = malloc((count ? count : 1) * sizeof(*all));
	if (all) {
		for (unsigned i = 0; i < p.list.count; i++)
			all[i] = &p.list.e[i];
		for (unsigned i = 0; i < p.reviews.count; i++)
			all[p.list.count + i] = &p.reviews.e[i];
		if (count)
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
		bubble(group, true, p.sending);
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
	p.base = ui_intern(json_str(json_get(json_get(j, "base"), "ref"), ""));
	p.head = ui_intern(json_str(json_get(json_get(j, "head"), "ref"), ""));
	if (json_bool(json_get(j, "merged")))
		p.state = MERGED;
	else if (strcmp(json_str(json_get(j, "state"), ""), "open"))
		p.state = CLOSED;
	else
		p.state = json_bool(json_get(j, "draft")) ? DRAFT : OPEN;
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
	/* Its pull request still open */
	bool here = p.scr && !strcmp(send_to, p.comments);
	if (status == 201 && j && j->kind == JSON_OBJECT) {
		if (here)
			entries_add(&p.list, COMMENT, j, "created_at");
	} else if (here) {
		const char *message = json_str(json_get(j, "message"), status ? "" : "no connection");
		if (status && !message[0])
			snprintf(p.send_error, sizeof(p.send_error), LV_SYMBOL_WARNING " Not sent: HTTP %ld", status);
		else
			snprintf(p.send_error, sizeof(p.send_error), LV_SYMBOL_WARNING " Not sent: %s", message);
		/* Back in the field, to send again */
		if (p.sending && !lv_textarea_get_text(p.chat.ta)[0])
			lv_textarea_set_text(p.chat.ta, p.sending);
	}
	if (status != 201)
		printf("github: comment not sent: HTTP %ld\n", status);
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
	if (send_fetch.curl) {
		/* One at a time: kept for later */
		lv_textarea_set_text(chat->ta, text);
		return;
	}
	size_t n = strlen(text);
	char *escaped = malloc(n * 6 + 1), *body = malloc(n * 6 + 16);
	if (escaped && body) {
		json_escape(escaped, n * 6 + 1, text);
		sprintf(body, "{\"body\":\"%s\"}", escaped);
		free(p.sending);
		p.sending = strdup(text);
		p.send_error[0] = '\0';
		snprintf(send_to, sizeof(send_to), "%s", p.comments);
		api_request(&send_fetch, "POST", p.comments, body, sent);
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

void pr_open(const char *path, const char *repo, const char *title)
{
	/* "/repos/o/r/pulls/7" */
	const char *number = strrchr(path, '/');
	if (p.scr || !number)
		return;
	number++;

	lv_obj_t *body;
	p.scr = ui_app_screen_create("Pull request", NULL, &body);
	lv_obj_add_event_cb(p.scr, delete_cb, LV_EVENT_DELETE, NULL);
	snprintf(p.path, sizeof(p.path), "%s", path);
	snprintf(p.comments, sizeof(p.comments), "/repos/%s/issues/%s/comments", repo, number);
	p.title = ui_intern(title);
	char key[160];
	snprintf(key, sizeof(key), "%s#%s", repo, number);
	p.key = ui_intern(key);

	p.chat.send = send;
	ui_chat_create(&p.chat, body, MAX_LENGTH);
	lv_textarea_set_placeholder_text(p.chat.ta, "Comment");

	char reviews[224], comments[224];
	snprintf(reviews, sizeof(reviews), "%s/reviews?per_page=100", path);
	snprintf(comments, sizeof(comments), "%s?per_page=100", p.comments);
	api_request(&p.pr, "GET", path, NULL, pr_loaded);
	api_request(&p.comments_fetch, "GET", comments, NULL, comments_loaded);
	api_request(&p.reviews_fetch, "GET", reviews, NULL, reviews_loaded);
	if (!me[0] && !me_fetch.curl)
		api_request(&me_fetch, "GET", "/user", NULL, me_loaded);
	fill(false);
	ui_push(p.scr);
}
