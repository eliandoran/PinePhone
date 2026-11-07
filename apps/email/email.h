/*
 * Email: the account's mail over IMAP (and SMTP to come), on phone-ui's TLS sockets
 * (ui_socket_open(): libcurl connects, the protocol is ours). Shared between its files:
 *
 * - email.c    the app's screen: the set-up form until there is an account, then the
 *              inbox, the account's screen from the header (removing it); the account
 *              kept in /var/lib/app-data/email/account.json
 * - setup.c    setting up the account: the address and password, the servers from
 *              Thunderbird's database or asked, the password tried on the IMAP server
 * - session.c  an IMAP session: connecting and logging in, commands one at a time with
 *              their untagged responses (literals whole) and tagged result
 * - imap.c     what the server says, parsed: FETCH responses, EXISTS, dates, encoded
 *              headers
 * - inbox.c    the inbox's last messages (SELECT, then FETCH of their envelopes), kept
 *              in inbox.json for the next opening
 * - plugin.c   what phone-ui looks for in email.so
 */

#ifndef PHONE_UI_EMAIL_H
#define PHONE_UI_EMAIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "i18n.h"
#include "json.h"
#include "ui.h"

#define EMAIL_DIR "/var/lib/app-data/email"
#define ACCOUNT_FILE EMAIL_DIR "/account.json"
#define INBOX_FILE EMAIL_DIR "/inbox.json"
#define INBOX_MAX 50 /* messages listed: the inbox's last */

/* The account (one): its address, the login on both servers, the servers as URLs
 * (imaps://host:port/, smtps://host:port/: TLS from the start, the only kind spoken;
 * session.c refuses any other) */
struct account {
	char address[128];
	char user[128];
	char password[128];
	char imap[192];
	char smtp[192];
};
extern struct account account; /* address "" while there is none */

bool account_load(void); /* from ACCOUNT_FILE, once: whether there is one */
bool account_save(void); /* false (logged) when it could not be written */
void account_remove(void);

/* A message as listed: its envelope */
struct mail {
	unsigned long uid;
	time_t date; /* the server's INTERNALDATE */
	bool seen;
	char from[128]; /* the sender's name, their address without one */
	char address[128];
	char subject[160];
};

void email_open(void);
/* The screen's content for the state: the form, or the inbox */
void email_show(void);
/* The host of a server's URL, for messages */
void url_host(const char *url, char *out, size_t size);

void setup_create(lv_obj_t *body);

void inbox_create(lv_obj_t *body);
void inbox_forget(void); /* the account gone: its messages too */

/* session.c: a session on the account's IMAP server. imap_connect() connects, waits
 * for the greeting and logs in; ready() gets 0, or what went wrong as a status to
 * show (ui_socket_open()'s negated CURLcodes; -CURLE_LOGIN_DENIED a refused login,
 * -CURLE_RECV_ERROR the server gone or not speaking IMAP). Then commands, one at a
 * time (queued up to a few): each untagged response whole (its literals in it) to
 * untagged() (may be NULL), then done() with IMAP_OK, IMAP_NO, IMAP_BAD, or IMAP_LOST
 * when the connection went first. Callbacks may close the session. Zeroed: closed. */
enum imap_result { IMAP_OK, IMAP_NO, IMAP_BAD, IMAP_LOST = -1 };
typedef void (*imap_untagged_fn)(const char *response, size_t len, void *user);
typedef void (*imap_done_fn)(int result, void *user);
#define IMAP_QUEUE 4
struct imap_command {
	char *text; /* without its tag and CRLF */
	imap_untagged_fn untagged;
	imap_done_fn done;
	void *user;
};
struct imap_session {
	struct ui_socket sock;
	char *in; /* bytes received, up to a whole response */
	size_t in_len;
	int state;     /* session.c's; 0 closed */
	unsigned tags; /* given so far */
	bool preauth;  /* the greeting said so: no login */
	char tag[8];   /* the command's on its way */
	void (*ready)(int status, void *user);
	void *ready_user;
	struct imap_command queue[IMAP_QUEUE]; /* [0] on its way while state says so */
	unsigned queued;
};
void imap_connect(struct imap_session *s, const char *url, const char *user, const char *password,
                  void (*ready)(int status, void *user), void *ready_user);
void imap_command(struct imap_session *s, const char *text, imap_untagged_fn untagged, imap_done_fn done, void *user);
void imap_close(struct imap_session *s);
/* A string as an IMAP quoted string, with its quotes */
void imap_quote(char *out, size_t size, const char *s);

/* imap.c */
/* A "* N FETCH (UID ... FLAGS (...) INTERNALDATE "..." ENVELOPE (...))" response of
 * `len` bytes into m: false when it is not one, or cut */
bool imap_fetch_parse(const char *response, size_t len, struct mail *m);
/* N of a "* N EXISTS" response, -1 if it is another */
long imap_exists(const char *response, size_t len);
/* An INTERNALDATE, "07-Nov-2026 14:05:33 +0100": 0 when not one */
time_t imap_date_parse(const char *s);
/* A header's text with its RFC 2047 encoded words (=?charset?B?...?=, Q) decoded to
 * UTF-8, folded lines joined */
void mime_header_decode(const char *in, char *out, size_t size);

#endif
