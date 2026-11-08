/*
 * The inbox: its last INBOX_MAX messages, newest first, each row the sender, the
 * time and the subject (unread: the time in the accent color, the subject not dim).
 * Loaded when the screen opens and when the list is pulled down (mail_list_inbox()).
 * What came last is kept in inbox.json and shown first when opened again, offline
 * too. A row opens its message (message.c).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "email.h"

#define ROW_H 120

static struct {
	struct mail mails[INBOX_MAX]; /* newest first */
	unsigned count;
	bool loaded; /* from the cache */
} inbox;

/* The screen, while open */
static struct {
	lv_obj_t *list, *note, *card, *empty;
	bool loading;
} v;

static void fill(void);

/* The cache */

static void cache_load(void)
{
	if (inbox.loaded)
		return;
	inbox.loaded = true;
	FILE *f = fopen(INBOX_FILE, "r");
	if (!f)
		return;
	char *buf = malloc(65536);
	size_t n = fread(buf, 1, 65535, f);
	buf[n] = '\0';
	fclose(f);
	struct json *list = json_parse(buf);
	free(buf);
	if (list && list->kind == JSON_ARRAY)
		for (struct json *j = list->child; j && inbox.count < INBOX_MAX; j = j->next) {
			struct mail *m = &inbox.mails[inbox.count++];
			m->uid = (unsigned long)json_int(json_get(j, "uid"), 0);
			m->date = (time_t)json_int(json_get(j, "date"), 0);
			m->seen = json_bool(json_get(j, "seen"));
			snprintf(m->from, sizeof(m->from), "%s", json_str(json_get(j, "from"), ""));
			snprintf(m->address, sizeof(m->address), "%s", json_str(json_get(j, "address"), ""));
			snprintf(m->subject, sizeof(m->subject), "%s", json_str(json_get(j, "subject"), ""));
		}
	json_free(list);
}

static void cache_save(void)
{
	FILE *f = fopen(INBOX_FILE ".new", "w");
	if (!f)
		return;
	fputs("[", f);
	for (unsigned i = 0; i < inbox.count; i++) {
		const struct mail *m = &inbox.mails[i];
		char from[6 * sizeof(m->from) + 1], address[6 * sizeof(m->address) + 1], subject[6 * sizeof(m->subject) + 1];
		json_escape(from, sizeof(from), m->from);
		json_escape(address, sizeof(address), m->address);
		json_escape(subject, sizeof(subject), m->subject);
		fprintf(f, "%s{\"uid\": %lu, \"date\": %lld, \"seen\": %s, \"from\": \"%s\", \"address\": \"%s\", \"subject\": \"%s\"}",
		        i ? ",\n " : "", m->uid, (long long)m->date, m->seen ? "true" : "false", from, address, subject);
	}
	fputs("]\n", f);
	if (fclose(f) == 0)
		rename(INBOX_FILE ".new", INBOX_FILE);
	/* The bodies kept of messages no longer listed go */
	DIR *dir = opendir(BODIES_DIR);
	struct dirent *d;
	while (dir && (d = readdir(dir))) {
		unsigned long uid = strtoul(d->d_name, NULL, 10);
		bool listed = false;
		for (unsigned i = 0; i < inbox.count && !listed; i++)
			listed = inbox.mails[i].uid == uid;
		if (!listed && uid) {
			char path[sizeof(BODIES_DIR) + 257];
			snprintf(path, sizeof(path), BODIES_DIR "/%s", d->d_name);
			unlink(path);
		}
	}
	if (dir)
		closedir(dir);
}

void inbox_forget(void)
{
	unlink(INBOX_FILE);
	memset(&inbox, 0, sizeof(inbox));
	cache_save(); /* the bodies too */
	unlink(INBOX_FILE);
}

void inbox_mark_seen(unsigned long uid)
{
	for (unsigned i = 0; i < inbox.count; i++)
		if (inbox.mails[i].uid == uid && !inbox.mails[i].seen) {
			inbox.mails[i].seen = true;
			cache_save();
			fill();
		}
}

/* The list */

static void row_cb(lv_event_t *e)
{
	message_open(lv_event_get_user_data(e));
}

/* As Messages' conversations show theirs (message_time_text(), not shared) */
static const char *time_text(time_t t)
{
	static char buf[64];
	time_t now = time(NULL);
	struct tm tn, tt;
	localtime_r(&now, &tn);
	localtime_r(&t, &tt);
	tn.tm_hour = tn.tm_min = tn.tm_sec = 0;
	time_t today = mktime(&tn);
	if (t >= today)
		strftime(buf, sizeof(buf), "%H:%M", &tt);
	else if (t >= today - 86400)
		snprintf(buf, sizeof(buf), "%s", _("Yesterday"));
	else if (t >= today - 6 * 86400)
		snprintf(buf, sizeof(buf), "%s", ui_day_name(tt.tm_wday, true));
	else if (tt.tm_year == tn.tm_year)
		snprintf(buf, sizeof(buf), "%d %s", tt.tm_mday, ui_month_name(tt.tm_mon, true));
	else
		snprintf(buf, sizeof(buf), "%d %s %d", tt.tm_mday, ui_month_name(tt.tm_mon, true), tt.tm_year + 1900);
	return buf;
}

static void fill(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.card);
	for (unsigned i = 0; i < inbox.count; i++) {
		const struct mail *m = &inbox.mails[i];
		lv_obj_t *row = ui_row_create(v.card, ROW_H, i + 1 < inbox.count, row_cb, &inbox.mails[i]);
		struct ui_row_lines l = ui_row_lines(row, "", "", "");
		lv_label_set_text(l.title, m->from);
		lv_label_set_text(l.time, time_text(m->date));
		lv_label_set_text(l.preview, m->subject[0] ? m->subject : _("(no subject)"));
		if (!m->seen) {
			lv_obj_set_style_text_color(l.time, lv_color_hex(UI_COLOR_ACCENT), 0);
			lv_obj_set_style_text_color(l.preview, lv_color_hex(UI_COLOR_TEXT), 0);
		}
	}
	lv_obj_set_hidden(v.card, !inbox.count);
}

/* A line above the list: loading, or why nothing came; NULL none */
static void note(const char *text)
{
	if (!v.list)
		return;
	if (text)
		lv_label_set_text(v.note, text);
	lv_obj_set_hidden(v.note, !text);
}

/* Loading */

static void listed(int status, const struct mail *mails, unsigned count, void *user)
{
	LV_UNUSED(user);
	v.loading = false;
	if (status != MAIL_OK) {
		char host[128], text[256];
		url_host(account.imap, host, sizeof(host));
		mail_error_text(status, host, text, sizeof(text));
		lv_obj_set_style_text_color(v.note, lv_color_hex(UI_COLOR_RED), 0);
		note(text);
		ui_pull_refresh_done(v.list);
		return;
	}
	inbox.count = count < INBOX_MAX ? count : INBOX_MAX;
	if (inbox.count)
		memcpy(inbox.mails, mails, inbox.count * sizeof(*mails));
	cache_save();
	fill();
	note(NULL);
	lv_obj_set_hidden(v.empty, inbox.count > 0);
	ui_pull_refresh_done(v.list);
}

static void refresh(void *unused)
{
	LV_UNUSED(unused);
	if (v.loading)
		return;
	v.loading = true;
	lv_obj_set_style_text_color(v.note, lv_color_hex(UI_COLOR_DIM), 0);
	note(inbox.count ? NULL : _("Loading..."));
	mail_list_inbox(INBOX_MAX, listed, &v);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	mail_cancel(&v);
	memset(&v, 0, sizeof(v));
}

void inbox_create(lv_obj_t *body)
{
	v.list = ui_box(body);
	lv_obj_set_width(v.list, LV_PCT(100));
	lv_obj_set_flex_grow(v.list, 1);
	lv_obj_set_flex_flow(v.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(v.list, 16, 0);
	lv_obj_set_style_pad_top(v.list, 8, 0);
	lv_obj_set_style_pad_bottom(v.list, 32, 0);
	lv_obj_set_style_pad_row(v.list, 12, 0);
	lv_obj_set_scrollable(v.list, true);
	lv_obj_set_clickable(v.list, true); /* dragged beside the rows too */
	lv_obj_set_scroll_dir(v.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(v.list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(v.list, delete_cb, LV_EVENT_DELETE, NULL);
	ui_pull_refresh(v.list, refresh, NULL);

	v.note = lv_label_create(v.list);
	lv_obj_set_width(v.note, LV_PCT(100));
	lv_label_set_long_mode(v.note, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(v.note, UI_FONT_BODY, 0);
	lv_obj_set_style_text_align(v.note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_ver(v.note, 16, 0);
	lv_obj_set_hidden(v.note, true);

	v.card = ui_card_create(v.list);
	v.empty = ui_empty_label(v.list, _("No messages"));
	lv_obj_set_hidden(v.empty, true);

	cache_load();
	fill();
	refresh(NULL);
}
