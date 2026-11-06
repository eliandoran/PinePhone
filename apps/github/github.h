/*
 * GitHub: a light client of GitHub's REST API, for the phone where its website
 * is too slow. Signs in with a personal access token, lists the notifications
 * and marks them read, opens pull requests, issues and discussions. Shared between
 * its files:
 *
 * - github.c         the app's screen: the token's form or the notifications; the
 *                    token, and requests to the API with it
 * - notifications.c  the notifications: the list, marking them read
 * - pr.c             a pull request, an issue or a discussion (GraphQL): its state and
 *                    conversation, commenting
 * - markdown.c       messages' Markdown as widgets, parsed by md4c.c (MD4C 0.5.2,
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
void notifications_show(bool shown); /* and loads them */

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

/* shortcodes.c (gen-shortcodes.sh): emoji by their names, sorted by name */
struct shortcode {
	const char *name, *emoji;
};
extern const struct shortcode shortcodes[];
extern const unsigned shortcode_count;

#endif
