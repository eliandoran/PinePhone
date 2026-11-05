/*
 * GitHub: a light client of GitHub's REST API, for the phone where its website
 * is too slow. Signs in with a personal access token, lists the notifications
 * and marks them read. Shared between its files:
 *
 * - github.c         the app's screen: the token's form or the notifications; the
 *                    token, and requests to the API with it
 * - notifications.c  the notifications: the list, marking them read
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

/* notifications.c */
void notifications_create(lv_obj_t *body, lv_obj_t *header);
void notifications_show(bool shown); /* and loads them */

#endif
