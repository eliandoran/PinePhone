/*
 * Email: the account's mail over IMAP and SMTP, through phone-ui's libcurl
 * (ui_fetch_mail()). Shared between its files:
 *
 * - email.c   the app's screen: the set-up form until there is an account, then the
 *             account; the account kept in /var/lib/app-data/email/account.json
 * - setup.c   setting up the account: the address and password, the servers from
 *             Thunderbird's database or asked, the password tried on the IMAP server
 * - plugin.c  what phone-ui looks for in email.so
 */

#ifndef PHONE_UI_EMAIL_H
#define PHONE_UI_EMAIL_H

#include <stdbool.h>
#include <stddef.h>

#include "i18n.h"
#include "json.h"
#include "ui.h"

#define EMAIL_DIR "/var/lib/app-data/email"
#define ACCOUNT_FILE EMAIL_DIR "/account.json"

/* The account (one): its address, the login on both servers, the servers as URLs
 * libcurl takes (imaps://host:port/, smtps://host:port/; imap://, smtp:// with STARTTLS) */
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

void email_open(void);
/* The screen's content for the state: the form, or the account */
void email_show(void);

void setup_create(lv_obj_t *body);

#endif
