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
 * - pr.c             a pull request, an issue or a discussion (GraphQL): its state and
 *                    conversation, commenting
 * - markdown.c       Markdown as widgets, parsed by md4c.c (MD4C 0.5.2,
 *                    unchanged, MIT: md4c-LICENSE.md), in md_fonts.c's fonts too
 *                    (gen-fonts.sh), shortcodes.c's emoji names (gen-shortcodes.sh)
 * - plugin.c         what phone-ui looks for in github.so
 */

#ifndef GITHUB_H
#define GITHUB_H

#include <stdbool.h>

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
/* A request to the API (path from the host, "/notifications") with the token;
 * body NULL: none */
void api_request(struct ui_fetch *f, const char *method, const char *path, const char *body,
                 void (*done)(const char *data, long status));
/* The same with another media type: FULL, texts with their HTML (body_html) */
#define FULL "application/vnd.github.full+json"
void api_request_as(struct ui_fetch *f, const char *accept, const char *method, const char *path,
                    const char *body, void (*done)(const char *data, long status));

/* notifications.c */
void notifications_create(lv_obj_t *body, lv_obj_t *header);
void notifications_show(bool shown); /* the list and its header button */
void notifications_load(void);

/* repos.c */
void repos_create(lv_obj_t *body);
void repos_show(bool shown);
void repos_load(void);
void repos_forget_account(void); /* signed out: another account next */

/* readme.c: the README of `repo` ("o/r") on a page of its own, over the
 * repository's page */
void readme_open(const char *repo);

/* pr.c */
/* The pull request at `path` ("/repos/o/r/pulls/7") of `repo` ("o/r"), its title
 * shown until it loads */
void pr_open(const char *path, const char *repo, const char *title);
/* The same for the issue at `path` ("/repos/o/r/issues/7") */
void issue_open(const char *path, const char *repo, const char *title);
/* The latest discussion of `repo` ("o/r") with that title: notifications give it no URL */
void discussion_open(const char *repo, const char *title);
void pr_forget_account(void); /* signed out: another account next */

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
