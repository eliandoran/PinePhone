/*
 * GitHub: a light client of GitHub's REST API, for the phone where its website
 * is too slow. Signs in with a personal access token, lists the notifications
 * and marks them read, opens pull requests, issues and discussions, lists the
 * account's repositories. Shared between its files:
 *
 * - github.c         the app's screen: the token's form, or the repositories and
 *                    the notifications switched by a bar at the bottom; the token,
 *                    and requests to the API with it
 * - notifications.c  the notifications: the list, marking them read
 * - repos.c          the account's own repositories, a repository's page
 * - readme.c         a repository's README
 * - list.c           a repository's issues or pull requests, a page at a time
 *                    (open or closed), with their checks' summaries
 * - pager.c          the bar going from page to page (list.c, repos.c)
 * - segments.c       a segmented control: the lists' open or closed, a pull request's tabs
 * - checks.c         status checks' states and icons, pull requests' and issues' icons
 * - pr.c             a pull request, an issue or a discussion (GraphQL): its state,
 *                    a pull request's checks, the conversation, commenting, merging,
 *                    the milestone, its files (files.c)
 * - milestone.c      a repository's open milestones to choose one from
 * - files.c          a pull request's files and their diffs
 * - markdown.c       Markdown as widgets, parsed by md4c.c (MD4C 0.5.2,
 *                    unchanged, MIT: md4c-LICENSE.md), in md_fonts.c's fonts too
 *                    (gen-fonts.sh), shortcodes.c's emoji names (gen-shortcodes.sh)
 * - plugin.c         what phone-ui looks for in github.so
 */

#ifndef GITHUB_H
#define GITHUB_H

#include <stdbool.h>

#include "i18n.h"
#include "json.h"
#include "ui.h"

#define DIR "/var/lib/app-data/github"
#define TOKEN_FILE DIR "/token"
#define API "https://api.github.com"

/* github.c */
void github_open(void);
/* The token's form instead of the rest, with `error` (static text, NULL: none):
 * after GitHub refused the token, which is forgotten */
void github_sign_out(const char *error);
/* A request answered 401: the screens over GitHub's closed, then signed out, saying so */
void github_refused(void);
/* A request to the API (path from the host, "/notifications") with the token;
 * body NULL: none */
void api_request(struct ui_fetch *f, const char *method, const char *path, const char *body,
                 void (*done)(const char *data, long status));
/* The same with another media type: FULL, texts with their HTML (body_html) */
#define FULL "application/vnd.github.full+json"
void api_request_as(struct ui_fetch *f, const char *accept, const char *method, const char *path,
                    const char *body, void (*done)(const char *data, long status));
/* A request to GitHub's GraphQL API: `query` on one line without quotes (it goes in a
 * JSON string as it is), `variables` a JSON object */
void graphql(struct ui_fetch *f, const char *query, const char *variables,
             void (*done)(const char *data, long status));
/* A GraphQL answer's data, NULL with `error` set to its first error's message (""
 * if it has none) when it has none */
const struct json *graphql_data(const struct json *j, char *error, size_t size);

/* checks.c: status checks (a commit's statusCheckRollup in GraphQL), in the order
 * they are listed: failed first */
enum check { CHECK_FAILED, CHECK_PENDING, CHECK_PASSED, CHECK_SKIPPED, CHECK_NONE };
/* A CheckRun or StatusContext's, or the rollup's (CHECK_NONE: null, unknown) */
enum check check_state(const struct json *j);
/* `label` showing the icon of `state`, in its color (hidden for CHECK_NONE) */
void check_icon_set(lv_obj_t *label, enum check state);
/* A pull request's or an issue's state: `label` showing its icon, in its color (GitHub's,
 * for the theme) */
enum state {
	STATE_PR_OPEN, STATE_PR_DRAFT, STATE_PR_MERGED, STATE_PR_CLOSED,
	STATE_ISSUE_OPEN, STATE_ISSUE_DONE, STATE_ISSUE_SKIPPED, /* skipped: not planned, a duplicate */
};
void state_icon_set(lv_obj_t *label, enum state state);

/* notifications.c */
void notifications_create(lv_obj_t *body, lv_obj_t *header);
void notifications_show(bool shown); /* the list and its header button */
void notifications_load(void);

/* repos.c */
void repos_create(lv_obj_t *body);
void repos_show(bool shown);
void repos_load(void);
void repos_forget_account(void); /* signed out: another account next */
/* The page of the repository `full_name` ("o/r"), loaded: from a bookmark, a pull
 * request's or an issue's screen */
void repo_open_name(const char *full_name);

/* readme.c: the README of `repo` ("o/r") on a page of its own, over the
 * repository's page */
void readme_open(const char *repo);

/* pager.c: a bar at the bottom of a list going from page to page; its owner keeps
 * the struct, sets page (from 1) and pages, calls pager_update(), and goes to the page
 * go() asks for */
#define PAGER_NUMBERS 5 /* page numbers in the bar */
struct pager {
	lv_obj_t *bar, *first, *prev, *next, *last, *numbers[PAGER_NUMBERS];
	int page, pages; /* pages 0: unknown (no bar) */
	void (*go)(int page);
};
void pager_create(struct pager *p, lv_obj_t *body, void (*go)(int page));
void pager_update(struct pager *p);

/* segments.c: a segmented control (Clock's tabs): `count` pills named `names` (static
 * text) in segs[], the first selected; cb gets clicks, the index as user data */
lv_obj_t *segments_create(lv_obj_t *parent, lv_obj_t *segs[], unsigned count, const char *const names[],
			  lv_event_cb_t cb);
void segments_select(lv_obj_t *segs[], unsigned count, unsigned selected);

/* list.c: the issues of `repo` ("o/r"), or its pull requests, a page at a time */
void list_open(const char *repo, bool pulls);
/* Pull request `number` of `repo` merged: its row says so, if shown */
void list_merged(const char *repo, long long number);

/* pr.c */
/* The pull request at `path` ("/repos/o/r/pulls/7") of `repo` ("o/r"), its title
 * shown until it loads, on its overview or (`talk`: from a comment, a mention) its
 * conversation */
void pr_open(const char *path, const char *repo, const char *title, bool talk);
/* The same for the issue at `path` ("/repos/o/r/issues/7"), which has no tabs */
void issue_open(const char *path, const char *repo, const char *title, bool talk);
/* The latest discussion of `repo` ("o/r") with that title: notifications give it no URL */
void discussion_open(const char *repo, const char *title);
void pr_forget_account(void); /* signed out: another account next */

/* milestone.c: the open milestones of `repo` ("o/r") on a screen of their own, `current`
 * ticked (0: none); the one tapped goes to picked() (0 and NULL: none) once it closed */
void milestone_pick(const char *repo, long long current, void (*picked)(long long number, const char *title));

/* files.c: a pull request's files (the REST API's list) as cards in `col`, their diffs */
void files_show(lv_obj_t *col, const struct json *files);

/* markdown.c: a bubble at the end of a ui_chat group (before its time) with the
 * Markdown `text` rendered, or GitHub's rendering of it (`html`, NULL if none) when it
 * has what the widgets do not show (HTML) */
void markdown_bubble(lv_obj_t *group, bool outgoing, const char *text, const char *html);
/* The same in `box`, which becomes a flex column (text in UI_COLOR_TEXT on its
 * background), its content at most `width` wide; the HTML's relative links from
 * `base_url`, cut at `max_html_h` */
void markdown_create(lv_obj_t *box, int32_t width, const char *text, const char *html, const char *base_url,
		     int32_t max_html_h);
/* Whether the widgets would not show it all: GitHub's HTML is needed */
bool markdown_needs_html(const char *text);

/* shortcodes.c (gen-shortcodes.sh): emoji by their names, sorted by name */
struct shortcode {
	const char *name, *emoji;
};
extern const struct shortcode shortcodes[];
extern const unsigned shortcode_count;

#endif
