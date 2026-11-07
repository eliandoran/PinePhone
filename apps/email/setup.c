/*
 * Setting up the account: the address and password; the provider's servers from
 * Thunderbird's database (autoconfig.thunderbird.net, which knows most providers),
 * or asked when it has none (host, or host:port; IMAP on 993 and SMTP on 465 over TLS
 * otherwise); then the password tried on the IMAP server (a NOOP once logged in), and
 * the account kept. The form shrinks above the keyboard; Enter moves on to what is
 * missing, then signs in.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "email.h"

#define AUTOCONFIG "https://autoconfig.thunderbird.net/v1.1/" /* NON-NLS */
#define FIELD_H 88
#define IMAP_PORT 993
#define SMTP_PORT 465

static struct {
	lv_obj_t *form, *hint, *address, *password, *imap, *smtp, *error, *button, *kb;
	bool servers; /* asked for: the provider unknown */
	bool busy;    /* a request on its way */
	char domain[128];
	struct account draft; /* being tried */
	struct ui_fetch fetch;
} setup;

static void type_into(lv_obj_t *ta)
{
	lv_obj_t *fields[] = {setup.address, setup.password, setup.imap, setup.smtp};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		if (fields[i] != ta)
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
	lv_keyboard_set_textarea(setup.kb, ta);
	ui_keyboard_show(setup.kb, true);
}

static void error(const char *text)
{
	lv_label_set_text(setup.error, text);
	lv_obj_set_hidden(setup.error, !text[0]);
}

static void busy(bool on)
{
	setup.busy = on;
	lv_obj_set_state(setup.button, LV_STATE_DISABLED, on);
	lv_label_set_text_static(lv_obj_get_child(setup.button, 0), on ? _("Wait...") : _("Sign in"));
}

/* What is missing, pointed at */
static void fail(const char *text, lv_obj_t *ta)
{
	ui_vibrate(UI_VIBRATE_TAP);
	error(text);
	type_into(ta);
}

/* The host of a server's URL, for messages */
static void url_host(const char *url, char *out, size_t size)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t n = strcspn(p, ":/");
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), p);
}

/* "host" or "host:port" typed as a server's URL */
static void server_url(char *out, size_t size, const char *scheme, const char *typed, int port)
{
	char host[160];
	size_t n = 0;
	for (; *typed && n < sizeof(host) - 1; typed++)
		if (!isspace((unsigned char)*typed))
			host[n++] = *typed;
	host[n] = '\0';
	if (strchr(host, ':'))
		snprintf(out, size, "%s://%s/", scheme, host);
	else
		snprintf(out, size, "%s://%s:%d/", scheme, host, port);
}

/* Thunderbird's XML: the text of <tag> between from and to, "" if none */
static void tag_text(const char *from, const char *to, const char *tag, char *out, size_t size)
{
	char open[64];
	snprintf(open, sizeof(open), "<%s>", tag);
	const char *p = strstr(from, open);
	out[0] = '\0';
	if (!p || p >= to)
		return;
	p += strlen(open);
	while (*p && isspace((unsigned char)*p))
		p++;
	size_t n = strcspn(p, "<");
	while (n && isspace((unsigned char)p[n - 1]))
		n--;
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), p);
}

/* The first <...Server type="kind"> block's server as a URL, false if none */
static bool server_parse(const char *xml, const char *element, const char *kind, const char *tls,
                         const char *plain, int port, char *url, size_t size, char *user, size_t user_size)
{
	char open[64], close[64];
	snprintf(open, sizeof(open), "<%s type=\"%s\">", element, kind); /* NON-NLS */
	snprintf(close, sizeof(close), "</%s>", element);
	const char *from = strstr(xml, open);
	const char *to = from ? strstr(from, close) : NULL;
	if (!to)
		return false;
	char host[128], port_text[8], socket[16], name[64];
	tag_text(from, to, "hostname", host, sizeof(host));
	tag_text(from, to, "port", port_text, sizeof(port_text));
	tag_text(from, to, "socketType", socket, sizeof(socket));
	tag_text(from, to, "username", name, sizeof(name));
	if (!host[0])
		return false;
	/* STARTTLS and plain take the plain scheme: libcurl upgrades to TLS or fails */
	snprintf(url, size, "%s://%s:%d/", strcmp(socket, "SSL") == 0 ? tls : plain, host,
	         atoi(port_text) > 0 ? atoi(port_text) : port);
	const char *at = strchr(setup.draft.address, '@');
	if (strcmp(name, "%EMAILLOCALPART%") == 0 && at)
		snprintf(user, user_size, "%.*s", (int)(at - setup.draft.address), setup.draft.address);
	else
		snprintf(user, user_size, "%s", setup.draft.address);
	return true;
}

static bool autoconfig_parse(const char *xml)
{
	char smtp_user[sizeof(setup.draft.user)];
	if (!server_parse(xml, "incomingServer", "imap", "imaps", "imap", IMAP_PORT, setup.draft.imap,
	                  sizeof(setup.draft.imap), setup.draft.user, sizeof(setup.draft.user)))
		return false;
	/* Sending can wait: a provider without SMTP here still reads its mail */
	if (!server_parse(xml, "outgoingServer", "smtp", "smtps", "smtp", SMTP_PORT, setup.draft.smtp,
	                  sizeof(setup.draft.smtp), smtp_user, sizeof(smtp_user)))
		setup.draft.smtp[0] = '\0';
	return true;
}

static void login_done(const char *data, long status)
{
	char host[128], text[256];
	if (data && status == 0) {
		account = setup.draft;
		if (!account_save()) {
			busy(false);
			error(_("The account could not be saved."));
			return;
		}
		email_show(); /* this form goes */
		return;
	}
	busy(false);
	url_host(setup.draft.imap, host, sizeof(host));
	/* A certificate for another name, or from an issuer the phone does not know */
	const char *reason = status == -CURLE_LOGIN_DENIED ? _("%s refused this password.") :
	                     status == -CURLE_PEER_FAILED_VERIFICATION ? _("The certificate of %s cannot be trusted.") :
	                     _("Cannot reach %s.");
	snprintf(text, sizeof(text), reason, host);
	error(text);
}

static void login(void)
{
	busy(true);
	error("");
	ui_fetch_mail(&setup.fetch, setup.draft.imap, setup.draft.user, setup.draft.password, "NOOP", login_done);
}

static void autoconfig_done(const char *data, long status)
{
	if (!status) {
		busy(false);
		error(_("No internet connection."));
		return;
	}
	if (status == 200 && data && autoconfig_parse(data)) {
		login();
		return;
	}
	/* Unknown there: its servers from the user */
	busy(false);
	setup.servers = true;
	char text[256];
	snprintf(text, sizeof(text), _("No settings are known for %s. Enter its IMAP and SMTP servers."), setup.domain);
	lv_label_set_text(setup.hint, text);
	lv_obj_set_hidden(setup.imap, false);
	lv_obj_set_hidden(setup.smtp, false);
	type_into(setup.imap);
}

static bool blank(const char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	return !*s;
}

static void submit(void)
{
	if (setup.busy)
		return;
	const char *address = lv_textarea_get_text(setup.address);
	const char *password = lv_textarea_get_text(setup.password);
	const char *at = strchr(address, '@');
	if (!at || at == address || !at[1] || strchr(address, ' ') || strchr(at + 1, '@')) {
		fail(_("Enter your email address."), setup.address);
		return;
	}
	if (!password[0]) {
		fail(_("Enter your password."), setup.password);
		return;
	}
	if (setup.servers) {
		if (blank(lv_textarea_get_text(setup.imap))) {
			fail(_("Enter the mail servers."), setup.imap);
			return;
		}
		if (blank(lv_textarea_get_text(setup.smtp))) {
			fail(_("Enter the mail servers."), setup.smtp);
			return;
		}
	}
	ui_vibrate(UI_VIBRATE_TAP);
	ui_keyboard_show(setup.kb, false);
	memset(&setup.draft, 0, sizeof(setup.draft));
	snprintf(setup.draft.address, sizeof(setup.draft.address), "%s", address);
	snprintf(setup.draft.user, sizeof(setup.draft.user), "%s", address);
	snprintf(setup.draft.password, sizeof(setup.draft.password), "%s", password);
	snprintf(setup.domain, sizeof(setup.domain), "%s", at + 1);
	if (setup.servers) {
		server_url(setup.draft.imap, sizeof(setup.draft.imap), "imaps", lv_textarea_get_text(setup.imap), IMAP_PORT);
		server_url(setup.draft.smtp, sizeof(setup.draft.smtp), "smtps", lv_textarea_get_text(setup.smtp), SMTP_PORT);
		login();
		return;
	}
	busy(true);
	error("");
	char url[sizeof(AUTOCONFIG) + sizeof(setup.domain)];
	snprintf(url, sizeof(url), AUTOCONFIG "%s", setup.domain);
	ui_fetch_start(&setup.fetch, url, autoconfig_done);
}

static void button_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	submit();
}

static void field_cb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target_obj(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		type_into(ta);
		break;
	case LV_EVENT_READY: /* Enter: on to what is missing, or sign in */
		submit();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(setup.kb, false);
		break;
	default:
		break;
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&setup.fetch);
	memset(&setup, 0, sizeof(setup));
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

static lv_obj_t *field_create(const char *placeholder, uint32_t max_length, bool password)
{
	lv_obj_t *ta = ui_text_field(setup.form, FIELD_H, max_length, field_cb, NULL);
	lv_obj_set_width(ta, LV_PCT(100));
	lv_textarea_set_placeholder_text(ta, placeholder);
	lv_textarea_set_password_mode(ta, password);
	return ta;
}

void setup_create(lv_obj_t *body)
{
	/* Shrinks above the keyboard */
	setup.form = lv_obj_create(body);
	lv_obj_remove_style_all(setup.form);
	lv_obj_set_width(setup.form, LV_PCT(100));
	lv_obj_set_flex_grow(setup.form, 1);
	lv_obj_set_flex_flow(setup.form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(setup.form, 32, 0);
	lv_obj_set_style_pad_ver(setup.form, 48, 0);
	lv_obj_set_style_pad_row(setup.form, 24, 0);
	lv_obj_set_scroll_dir(setup.form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(setup.form, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(setup.form, delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = text_label(setup.form, UI_FONT_HEADLINE, UI_COLOR_TEXT);
	lv_label_set_text_static(title, _("Your email account"));
	setup.hint = text_label(setup.form, UI_FONT_BODY, UI_COLOR_DIM);
	lv_label_set_text_static(setup.hint, _("Sign in with your address and password. The phone finds the mail "
	                                       "servers of most providers, and asks for them otherwise."));

	setup.address = field_create(_("Email address"), sizeof(account.address) - 1, false);
	setup.password = field_create(_("Password"), sizeof(account.password) - 1, true);
	setup.imap = field_create(_("IMAP server"), 160, false);
	lv_obj_set_hidden(setup.imap, true);
	setup.smtp = field_create(_("SMTP server"), 160, false);
	lv_obj_set_hidden(setup.smtp, true);

	setup.error = text_label(setup.form, UI_FONT_BODY, UI_COLOR_RED);
	lv_obj_set_hidden(setup.error, true);

	setup.button = ui_button(setup.form, LV_PCT(100), 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Sign in"),
	                         button_cb, NULL);

	setup.kb = ui_keyboard_create(body, setup.address);
}
