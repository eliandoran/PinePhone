/*
 * A message opened from the inbox: its subject, sender and date, then its body on a
 * card: the HTML part laid out as a page (ui_html_create(), in the UI's colors, its
 * images fetched), or the plain text one. The part is found in the message's
 * BODYSTRUCTURE and fetched alone (BODY.PEEK of its section), in a session of the
 * screen's own, decoded (mime_decode_body()) and kept in BODIES_DIR/<uid>.html or
 * .txt, shown from there the next time and offline. Shown, an unread message is
 * marked read on the server (\Seen) and in the inbox.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "email.h"

#define MAX_HTML_H 16384 /* a message as a page: cut there */
#define MAX_TEXT 65536   /* bytes of plain text shown */

static struct {
	lv_obj_t *scr, *list, *note;
	struct mail mail;
	struct imap_session session;
	struct imap_part html, plain, *part; /* the body's parts, the one fetched */
	bool shown;
} msg;

static void note(const char *text)
{
	if (!msg.note)
		return;
	ui_set_text(msg.note, text);
	lv_obj_set_hidden(msg.note, false);
}

/* The body on its card */
static void show(const char *text, bool html)
{
	if (!msg.list || msg.shown)
		return;
	msg.shown = true;
	lv_obj_set_hidden(msg.note, true);
	lv_obj_t *card = ui_card_create(msg.list);
	lv_obj_set_style_pad_all(card, 24, 0);
	int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * 16 - 2 * 24;
	if (html) {
		/* The light page mail assumes (it sets its own colors for it), drawn dark by
		 * phone-ui in the dark theme */
		char css[1024];
		ui_html_css(css, sizeof(css), lv_color_black(), lv_color_white(), lv_color_hex(UI_COLOR_ACCENT));
		ui_html_create(card, text, NULL, w, MAX_HTML_H, css);
		return;
	}
	lv_obj_t *label = lv_label_create(card);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	if (strlen(text) > MAX_TEXT) {
		char *cut = strndup(text, MAX_TEXT);
		lv_label_set_text(label, cut ? cut : "");
		free(cut);
	} else {
		lv_label_set_text(label, text);
	}
}

/* The kept copy */

static void body_path(char *out, size_t size, unsigned long uid, bool html)
{
	snprintf(out, size, BODIES_DIR "/%lu.%s", uid, html ? "html" : "txt");
}

/* The body kept for the message, NULL if none; *html says which */
static char *body_load(unsigned long uid, bool *html)
{
	char path[128];
	for (int i = 1; i >= 0; i--) {
		body_path(path, sizeof(path), uid, i);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
		if (text) {
			size_t got = fread(text, 1, (size_t)n, f);
			text[got] = '\0';
		}
		fclose(f);
		*html = i;
		return text;
	}
	return NULL;
}

static void body_save(unsigned long uid, bool html, const char *text)
{
	mkdir(BODIES_DIR, 0700);
	char path[128], tmp[136];
	body_path(path, sizeof(path), uid, html);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(text, f);
	if (fclose(f) == 0)
		rename(tmp, path);
}

/* The session */

static void failed(int status)
{
	imap_close(&msg.session);
	if (msg.shown)
		return; /* read from the copy kept: the mark can wait */
	char host[128], text[256];
	url_host(account.imap, host, sizeof(host));
	const char *reason = status == -CURLE_LOGIN_DENIED ? _("%s refused the password.") :
	                     status == -CURLE_PEER_FAILED_VERIFICATION ? _("The certificate of %s cannot be trusted.") :
	                     _("Cannot reach %s.");
	snprintf(text, sizeof(text), reason, host);
	note(text);
}

static void marked(int result, void *user)
{
	LV_UNUSED(user);
	imap_close(&msg.session);
	if (result == IMAP_OK) {
		msg.mail.seen = true;
		inbox_mark_seen(msg.mail.uid);
	}
}

static void mark_read(void)
{
	if (msg.mail.seen) {
		imap_close(&msg.session);
		return;
	}
	char command[64];
	snprintf(command, sizeof(command), "UID STORE %lu +FLAGS (\\Seen)", msg.mail.uid); /* NON-NLS */
	imap_command(&msg.session, command, NULL, marked, NULL);
}

static void body_untagged(const char *response, size_t len, void *user)
{
	LV_UNUSED(user);
	const char *data;
	size_t size;
	if (msg.shown || !imap_body_literal(response, len, &data, &size))
		return;
	char *text = mime_decode_body(data, size, msg.part->encoding, msg.part->charset);
	if (!text)
		return;
	bool html = msg.part == &msg.html;
	body_save(msg.mail.uid, html, text);
	show(text, html);
	free(text);
}

static void body_fetched(int result, void *user)
{
	LV_UNUSED(user);
	if (result != IMAP_OK) {
		failed(-CURLE_RECV_ERROR);
		return;
	}
	if (!msg.shown)
		note(_("This message has no text."));
	mark_read();
}

static void structure_untagged(const char *response, size_t len, void *user)
{
	LV_UNUSED(user);
	struct imap_part html, plain;
	if (imap_bodystructure_parse(response, len, &html, &plain)) {
		msg.html = html;
		msg.plain = plain;
	}
}

static void structure_fetched(int result, void *user)
{
	LV_UNUSED(user);
	if (result != IMAP_OK) {
		failed(-CURLE_RECV_ERROR);
		return;
	}
	/* The page when there is one, the text otherwise */
	msg.part = msg.html.section[0] ? &msg.html : msg.plain.section[0] ? &msg.plain : NULL;
	if (!msg.part) {
		note(_("This message has no text."));
		mark_read();
		return;
	}
	char command[96];
	snprintf(command, sizeof(command), "UID FETCH %lu (BODY.PEEK[%s])", msg.mail.uid, msg.part->section); /* NON-NLS */
	imap_command(&msg.session, command, body_untagged, body_fetched, NULL);
}

static void selected(int result, void *user)
{
	LV_UNUSED(user);
	if (result != IMAP_OK) {
		failed(-CURLE_RECV_ERROR);
		return;
	}
	if (msg.shown) { /* only the mark is left to do */
		mark_read();
		return;
	}
	char command[64];
	snprintf(command, sizeof(command), "UID FETCH %lu (BODYSTRUCTURE)", msg.mail.uid); /* NON-NLS */
	imap_command(&msg.session, command, structure_untagged, structure_fetched, NULL);
}

static void connected(int status, void *user)
{
	LV_UNUSED(user);
	if (status) {
		failed(status);
		return;
	}
	/* UIDs mean nothing before a mailbox is selected */
	imap_command(&msg.session, "SELECT INBOX", NULL, selected, NULL); /* NON-NLS */
}

/* The screen */

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_label_set_text(label, text);
	return label;
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	imap_close(&msg.session);
	memset(&msg, 0, sizeof(msg));
}

void message_open(const struct mail *m)
{
	if (msg.scr)
		return;
	msg.mail = *m;
	lv_obj_t *body;
	msg.scr = ui_app_screen_create(_("Message"), NULL, &body);
	lv_obj_add_event_cb(msg.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	msg.list = ui_box(body);
	lv_obj_set_width(msg.list, LV_PCT(100));
	lv_obj_set_flex_grow(msg.list, 1);
	lv_obj_set_flex_flow(msg.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(msg.list, 16, 0);
	lv_obj_set_style_pad_row(msg.list, 12, 0);
	lv_obj_set_scrollable(msg.list, true);
	lv_obj_set_scroll_dir(msg.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(msg.list, LV_SCROLLBAR_MODE_OFF);

	/* Who, when, what */
	lv_obj_t *head = ui_card_create(msg.list);
	lv_obj_set_style_pad_all(head, 24, 0);
	lv_obj_set_style_pad_row(head, 8, 0);
	text_label(head, UI_FONT_TITLE, UI_COLOR_TEXT, m->subject[0] ? m->subject : _("(no subject)"));
	char line[300];
	if (strcmp(m->from, m->address) != 0)
		snprintf(line, sizeof(line), "%s <%s>", m->from, m->address);
	else
		snprintf(line, sizeof(line), "%s", m->address);
	text_label(head, UI_FONT_BODY, UI_COLOR_DIM, line);
	struct tm tt;
	localtime_r(&m->date, &tt);
	snprintf(line, sizeof(line), "%d %s %d, %02d:%02d", tt.tm_mday, ui_month_name(tt.tm_mon, true), tt.tm_year + 1900,
	         tt.tm_hour, tt.tm_min);
	text_label(head, UI_FONT_CAPTION, UI_COLOR_DIM, line);

	msg.note = ui_empty_label(msg.list, "");
	lv_obj_set_hidden(msg.note, true);

	bool html;
	char *kept = body_load(m->uid, &html);
	if (kept) {
		show(kept, html);
		free(kept);
	} else {
		note(_("Loading..."));
	}
	if (!kept || !m->seen)
		imap_connect(&msg.session, account.imap, account.user, account.password, connected, NULL);
	ui_push(msg.scr);
}
