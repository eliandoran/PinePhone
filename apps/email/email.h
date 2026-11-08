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
 * - folders.c  the account's folders (the inbox, the special ones, the rest), kept in
 *              folders.json, their screen, the one open (FOLDER_FILE)
 * - inbox.c    the folder open: its last messages, kept in folders/<name>.json
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
#define FOLDERS_FILE EMAIL_DIR "/folders.json" /* the server's folders, as listed last */
#define FOLDER_FILE EMAIL_DIR "/folder"        /* the folder open, by name */
#define FOLDERS_DIR EMAIL_DIR "/folders"       /* <folder>.json: its messages listed last */
#define INBOX_PAGE 50 /* messages listed at a time: the folder's last, then the next older */
#define INBOX_MAX 500 /* kept, the oldest loaded dropped past it */
#define BODIES_DIR EMAIL_DIR "/bodies" /* <folder>/<uid>.txt or .html: the messages read, kept */

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

/* A folder of the account */
enum folder_role { FOLDER_OTHER, FOLDER_INBOX, FOLDER_SENT, FOLDER_DRAFTS, FOLDER_TRASH, FOLDER_JUNK, FOLDER_ARCHIVE };
struct folder {
	char name[128];  /* as the server names it, on the wire (modified UTF-7) */
	char title[128]; /* for the user: decoded, " / " between a path's parts */
	enum folder_role role;
	unsigned unseen;
};

void email_open(void);
/* The screen's content for the state: the form, or the inbox */
void email_show(void);
/* The host of a server's URL, for messages */
void url_host(const char *url, char *out, size_t size);

void setup_create(lv_obj_t *body);

/* folders.c */
const struct folder *folder_current(void); /* the one open, the inbox at first */
const char *folder_title(const struct folder *f); /* Inbox, Sent... translated, the title else */
void folders_open(void); /* their screen: one tapped is opened (email_show()) */
void folders_forget(void); /* the account gone */
/* The name of a folder as a file name: every byte but letters, digits, . - _ as %XX */
void folder_file_name(const char *folder, char *out, size_t size);

void inbox_create(lv_obj_t *body); /* the folder open */
void inbox_forget(void); /* the account gone: its messages too */
void inbox_mark_seen(const char *folder, unsigned long uid); /* read: the list and its cache follow */

void message_open(const char *folder, const struct mail *m);

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
/* The messages listed, and the folder's UIDVALIDITY: its UIDs renumbered when it changes */
typedef void (*mail_listed_fn)(int status, const struct mail *mails, unsigned count, unsigned long uidvalidity,
                               void *user);
typedef void (*mail_folders_fn)(int status, const struct folder *folders, unsigned count, void *user);
typedef void (*mail_body_fn)(int status, const char *text, bool html, void *user);
/* Logs in as `a` in a session of its own, closed again: whether it can */
void mail_check_login(const struct account *a, mail_done_fn done, void *user);
/* The account's folders that can hold messages, in the order shown, how many unread in each */
void mail_list_folders(mail_folders_fn done, void *user);
/* The folder's last `max` messages, newest first */
void mail_list(const char *folder, unsigned max, mail_listed_fn done, void *user);
/* The `max` messages before the one with UID `before`, newest first; fewer, none, when
 * the first of the folder is among them */
void mail_list_older(const char *folder, unsigned long before, unsigned max, mail_listed_fn done, void *user);
/* A message's HTML part, or its plain text one, decoded to UTF-8 (MAIL_NO_TEXT: neither) */
void mail_fetch_body(const char *folder, unsigned long uid, mail_body_fn done, void *user);
void mail_mark_seen(const char *folder, unsigned long uid, mail_done_fn done, void *user);
/* The jobs queued with `user`: their done() is not called (their screen is gone) */
void mail_cancel(void *user);
/* The account removed: its session closed */
void mail_disconnect(void);
/* What went wrong, for the user, naming the server's `host` */
void mail_error_text(int status, const char *host, char *out, size_t size);

#endif
