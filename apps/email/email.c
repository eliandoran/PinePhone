/*
 * Email's screen: the set-up form (setup.c) until there is an account, then the
 * inbox (inbox.c), with the account's screen from the header (its address, removing
 * it). The account is kept in /var/lib/app-data/email/account.json on the data
 * partition, the password in it as GitHub's token is: the phone has no key store,
 * and the file is root's alone.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "email.h"
#include "ui_symbols.h"

struct account account;

static struct {
	lv_obj_t *scr, *header, *body;
	lv_obj_t *account_button, *folder_button; /* the header's, with an account */
	lv_obj_t *confirm;        /* the account screen's bar confirming the removal */
} screen;

static void field(const struct json *j, const char *key, char *out, size_t size)
{
	snprintf(out, size, "%s", json_str(json_get(j, key), ""));
}

bool account_load(void)
{
	static bool loaded;
	if (loaded)
		return account.address[0];
	loaded = true;
	FILE *f = fopen(ACCOUNT_FILE, "r");
	if (!f)
		return false;
	char buf[2048];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	struct json *j = json_parse(buf);
	field(j, "address", account.address, sizeof(account.address));
	field(j, "user", account.user, sizeof(account.user));
	field(j, "password", account.password, sizeof(account.password));
	field(j, "imap", account.imap, sizeof(account.imap));
	field(j, "smtp", account.smtp, sizeof(account.smtp));
	json_free(j);
	/* Without its servers it is of no use: set up again */
	if (!account.address[0] || !account.imap[0])
		memset(&account, 0, sizeof(account));
	return account.address[0];
}

bool account_save(void)
{
	mkdir("/var/lib/app-data", 0755);
	mkdir(EMAIL_DIR, 0700);
	int fd = open(ACCOUNT_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("email: %s: %s\n", ACCOUNT_FILE, strerror(errno));
		return false;
	}
	FILE *f = fdopen(fd, "w");
	char address[6 * sizeof(account.address) + 1], user[6 * sizeof(account.user) + 1],
		password[6 * sizeof(account.password) + 1], imap[6 * sizeof(account.imap) + 1],
		smtp[6 * sizeof(account.smtp) + 1];
	json_escape(address, sizeof(address), account.address);
	json_escape(user, sizeof(user), account.user);
	json_escape(password, sizeof(password), account.password);
	json_escape(imap, sizeof(imap), account.imap);
	json_escape(smtp, sizeof(smtp), account.smtp);
	fprintf(f, "{\"address\": \"%s\", \"user\": \"%s\", \"password\": \"%s\", \"imap\": \"%s\", \"smtp\": \"%s\"}\n",
	        address, user, password, imap, smtp);
	bool ok = fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f) != 0 || !ok || rename(ACCOUNT_FILE ".new", ACCOUNT_FILE) != 0) {
		printf("email: %s: %s\n", ACCOUNT_FILE, strerror(errno));
		return false;
	}
	return true;
}

void account_remove(void)
{
	unlink(ACCOUNT_FILE);
	memset(&account, 0, sizeof(account));
	inbox_forget();
	folders_forget();
}

void url_host(const char *url, char *out, size_t size)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t n = strcspn(p, ":/");
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), p);
}

/* The account's screen */

static void remove_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(screen.confirm, false);
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(screen.confirm, true);
}

static void confirm_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	account_remove();
	email_show(); /* the main screen below: the form again */
	ui_pop();
}

static void account_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	screen.confirm = NULL;
}

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void account_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Account"), &header, &body);
	lv_obj_add_event_cb(scr, account_delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *box = ui_box(body);
	lv_obj_set_width(box, LV_PCT(100));
	lv_obj_set_flex_grow(box, 1);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(box, 32, 0);
	lv_obj_set_style_pad_ver(box, 48, 0);
	lv_obj_set_style_pad_row(box, 24, 0);

	lv_obj_t *address = text_label(box, UI_FONT_HEADLINE, UI_COLOR_TEXT);
	lv_label_set_text(address, account.address);
	char host[128];
	url_host(account.imap, host, sizeof(host));
	lv_obj_t *server = text_label(box, UI_FONT_BODY, UI_COLOR_DIM);
	lv_label_set_text(server, host);

	lv_obj_t *remove = ui_button(box, LV_PCT(100), 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                             _("Remove account"), remove_cb, NULL);
	lv_obj_set_style_text_color(lv_obj_get_child(remove, 0), lv_color_hex(UI_COLOR_RED), 0);

	screen.confirm = ui_confirm_bar(body, _("Remove"), confirm_cb, cancel_cb, NULL);
	ui_push(scr);
}

static void account_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	account_open();
}

static void folders_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	folders_open();
}

/* The screen */

void email_show(void)
{
	lv_obj_clean(screen.body);
	bool signed_in = account_load();
	lv_obj_set_hidden(screen.account_button, !signed_in);
	lv_obj_set_hidden(screen.folder_button, !signed_in);
	ui_screen_set_title(screen.header, signed_in ? folder_title(folder_current()) : _("Email"));
	if (signed_in)
		inbox_create(screen.body);
	else
		setup_create(screen.body);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&screen, 0, sizeof(screen));
}

void email_open(void)
{
	screen.scr = ui_app_screen_create(_("Email"), &screen.header, &screen.body);
	lv_obj_add_event_cb(screen.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(screen.body, false);
	screen.account_button = ui_header_button(screen.header, UI_SYMBOL_USER, account_cb, NULL);
	screen.folder_button = ui_header_button(screen.header, LV_SYMBOL_DIRECTORY, folders_cb, NULL);
	email_show();
	ui_push(screen.scr);
}
