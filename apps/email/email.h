/*
 * Email: the account's mail over IMAP (and SMTP to come) through libetpan, on a
 * thread of its own (client.c). Shared between its files:
 *
 * - email.c    the app's screen: the set-up form until there is an account, then the
 *              inbox, the account's screen from the header (removing it); the account
 *              kept in /var/lib/app-data/email/account.json
 * - setup.c    setting up the account: the address and password, the servers from
 *              Thunderbird's database or asked, the password tried on the IMAP server
 * - client.c   the IMAP session: libetpan's blocking calls on a worker thread, jobs
 *              queued by the UI, their results back on it; TLS verified here
 * - inbox.c    the inbox's last messages, kept in inbox.json for the next opening
 * - message.c  a message opened: its body's HTML or plain text part, shown, kept in
 *              bodies/, marked read
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
#define BODIES_DIR EMAIL_DIR "/bodies" /* <uid>.txt or .html: the messages read, kept */

/* The account (one): its address, the login on both servers, the servers as URLs
 * (imaps://host:port/, smtps://host:port/: TLS from the start; plain imap:// and
 * smtp:// only to the loopback address, the tests' servers) */
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
/* Plain IMAP and SMTP go there and nowhere else: the loopback address */
bool host_is_loopback(const char *host);

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
void inbox_mark_seen(unsigned long uid); /* read: the list and its cache follow */

void message_open(const struct mail *m);

/* client.c: the account's IMAP server through libetpan, whose calls block: they run on
 * a thread of their own, the jobs below queued by the UI and done in order, one session
 * kept logged in across them (made again when it broke), each job's result handed back
 * on the UI thread. A status is MAIL_OK or why not. */
enum {
	MAIL_OK = 0,
	MAIL_LOGIN_DENIED = -1, /* the server refused the password */
	MAIL_UNREACHABLE = -2,  /* no server there, or it went */
	MAIL_CERTIFICATE = -3,  /* its certificate is not for its name, or untrusted */
	MAIL_FAILED = -4,       /* the server refused a command, or did not answer as one */
	MAIL_NO_TEXT = -5,      /* the message has neither an HTML nor a plain text part */
	MAIL_GARBLED = -6,      /* the server's answer could not be made sense of */
};
typedef void (*mail_done_fn)(int status, void *user);
typedef void (*mail_listed_fn)(int status, const struct mail *mails, unsigned count, void *user);
typedef void (*mail_body_fn)(int status, const char *text, bool html, void *user);
/* Logs in as `a` in a session of its own, closed again: whether it can */
void mail_check_login(const struct account *a, mail_done_fn done, void *user);
/* The inbox's last `max` messages, newest first */
void mail_list_inbox(unsigned max, mail_listed_fn done, void *user);
/* A message's HTML part, or its plain text one, decoded to UTF-8 (MAIL_NO_TEXT: neither) */
void mail_fetch_body(unsigned long uid, mail_body_fn done, void *user);
void mail_mark_seen(unsigned long uid, mail_done_fn done, void *user);
/* The jobs queued with `user`: their done() is not called (their screen is gone) */
void mail_cancel(void *user);
/* The account removed: its session closed */
void mail_disconnect(void);
/* What went wrong, for the user, naming the server's `host` */
void mail_error_text(int status, const char *host, char *out, size_t size);

#endif
