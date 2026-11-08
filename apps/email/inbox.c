/*
 * The folder open (folder_current()): its last INBOX_PAGE messages, newest first,
 * each row the sender, the time and the subject (unread: the time in the accent
 * color, the subject not dim). Loaded when the screen opens and when the list is
 * pulled down (mail_list()), which lists the newest again and keeps the older ones
 * loaded below them. A Load more row at the end asks for the INBOX_PAGE before the
 * oldest shown (mail_list_older()), gone once the folder's first message is shown,
 * with INBOX_MAX kept at most. What came is kept in folders/<name>.json, with the
 * folder's UIDVALIDITY (dropped when the server renumbered its messages), and shown
 * first when opened again, offline too. A row opens its message (message.c).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "email.h"

#define ROW_H 120
#define OLD_INBOX_FILE EMAIL_DIR "/inbox.json" /* the version before folders: the inbox's */

static struct {
	char folder[128];             /* whose */
	struct mail mails[INBOX_MAX]; /* newest first */
	unsigned count;
	unsigned long uidvalidity; /* 0 unknown */
	bool loaded;               /* from the cache */
	bool more;                 /* the folder may have older messages than the last one here */
} inbox;

/* The screen, while open */
static struct {
	lv_obj_t *list, *note, *card, *empty, *more;
	bool loading;      /* the newest */
	bool loading_more; /* the older */
} v;

static void fill(void);
static void more_show(void);

/* The cache */

static void cache_path(char *out, size_t size)
{
	char name[4 * sizeof(inbox.folder)];
	folder_file_name(inbox.folder, name, sizeof(name));
	snprintf(out, size, FOLDERS_DIR "/%s.json", name);
}

static void bodies_path(char *out, size_t size)
{
	char name[4 * sizeof(inbox.folder)];
	folder_file_name(inbox.folder, name, sizeof(name));
	snprintf(out, size, BODIES_DIR "/%s", name);
}

/* Every file in `dir` whose name `keep` does not want gone; `rmdir` the directory too */
static void dir_clean(const char *dir, bool (*keep)(const char *name), bool rmdir_too)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || (keep && keep(e->d_name)))
			continue;
		char path[600];
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) < (int)sizeof(path))
			unlink(path); /* not another file a path cut short names */
	}
	if (d)
		closedir(d);
	if (rmdir_too)
		rmdir(dir);
}

/* What the version before folders kept of the inbox: moved to where it is looked for */
static void cache_migrate(void)
{
	char path[600];
	cache_path(path, sizeof(path));
	mkdir(FOLDERS_DIR, 0700);
	rename(OLD_INBOX_FILE, path);
	bodies_path(path, sizeof(path));
	mkdir(BODIES_DIR, 0700);
	mkdir(path, 0700);
	DIR *d = opendir(BODIES_DIR);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char from[600], to[900];
		struct stat fs;
		snprintf(from, sizeof(from), BODIES_DIR "/%s", e->d_name);
		if (e->d_name[0] == '.' || stat(from, &fs) != 0 || !S_ISREG(fs.st_mode))
			continue; /* the folders' directories */
		snprintf(to, sizeof(to), "%s/%s", path, e->d_name);
		rename(from, to);
	}
	if (d)
		closedir(d);
}

static void cache_load(void)
{
	const struct folder *current = folder_current();
	if (inbox.loaded && strcmp(inbox.folder, current->name) == 0)
		return;
	memset(&inbox, 0, sizeof(inbox));
	snprintf(inbox.folder, sizeof(inbox.folder), "%s", current->name);
	inbox.loaded = true;
	char path[600];
	cache_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f && current->role == FOLDER_INBOX && access(OLD_INBOX_FILE, F_OK) == 0) {
		cache_migrate();
		f = fopen(path, "r");
	}
	if (!f)
		return;
	struct stat st;
	size_t size = fstat(fileno(f), &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
	char *buf = malloc(size + 1);
	size_t n = buf ? fread(buf, 1, size, f) : 0;
	fclose(f);
	if (!buf)
		return;
	buf[n] = '\0';
	struct json *root = json_parse(buf), *list = root;
	free(buf);
	/* {"uidvalidity": ..., "more": ..., "mails": [...]}; an earlier version kept the array alone */
	if (root && root->kind == JSON_OBJECT) {
		list = json_get(root, "mails");
		inbox.more = json_bool(json_get(root, "more"));
		inbox.uidvalidity = (unsigned long)json_int(json_get(root, "uidvalidity"), 0);
	} else {
		inbox.more = true;
	}
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
	json_free(root);
	inbox.more = inbox.more && inbox.count > 0;
}

/* The bodies kept of messages no longer listed go */
static bool body_listed(const char *name)
{
	unsigned long uid = strtoul(name, NULL, 10);
	for (unsigned i = 0; i < inbox.count; i++)
		if (inbox.mails[i].uid == uid)
			return true;
	return !uid;
}

static void cache_save(void)
{
	char path[600];
	cache_path(path, sizeof(path));
	mkdir(FOLDERS_DIR, 0700);
	char tmp[608];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "{\"uidvalidity\": %lu, \"more\": %s, \"mails\": [", inbox.uidvalidity, inbox.more ? "true" : "false");
	for (unsigned i = 0; i < inbox.count; i++) {
		const struct mail *m = &inbox.mails[i];
		char from[6 * sizeof(m->from) + 1], address[6 * sizeof(m->address) + 1], subject[6 * sizeof(m->subject) + 1];
		json_escape(from, sizeof(from), m->from);
		json_escape(address, sizeof(address), m->address);
		json_escape(subject, sizeof(subject), m->subject);
		fprintf(f, "%s{\"uid\": %lu, \"date\": %lld, \"seen\": %s, \"from\": \"%s\", \"address\": \"%s\", \"subject\": \"%s\"}",
		        i ? ",\n " : "", m->uid, (long long)m->date, m->seen ? "true" : "false", from, address, subject);
	}
	fputs("]}\n", f);
	if (fclose(f) == 0)
		rename(tmp, path);
	bodies_path(path, sizeof(path));
	dir_clean(path, body_listed, false);
}

void inbox_forget(void)
{
	/* Every folder's list and bodies */
	dir_clean(FOLDERS_DIR, NULL, true);
	DIR *d = opendir(BODIES_DIR);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[600];
		snprintf(path, sizeof(path), BODIES_DIR "/%s", e->d_name);
		dir_clean(path, NULL, true);
		unlink(path); /* an earlier version's body */
	}
	if (d)
		closedir(d);
	rmdir(BODIES_DIR);
	unlink(OLD_INBOX_FILE);
	memset(&inbox, 0, sizeof(inbox));
}

void inbox_mark_seen(const char *folder, unsigned long uid)
{
	if (strcmp(folder, inbox.folder) != 0)
		return;
	for (unsigned i = 0; i < inbox.count; i++)
		if (inbox.mails[i].uid == uid && !inbox.mails[i].seen) {
			inbox.mails[i].seen = true;
			cache_save();
			if (v.card)
				ui_list_refresh(v.card, i);
		}
}

/* The server renumbered the folder's messages: what was kept is of others */
static void uids_check(unsigned long uidvalidity)
{
	if (inbox.uidvalidity && uidvalidity && uidvalidity != inbox.uidvalidity) {
		inbox.count = 0;
		inbox.more = false;
	}
	if (uidvalidity)
		inbox.uidvalidity = uidvalidity;
}

/* The newest `count` messages listed again (`max` asked for): they replace what was
 * newer than their oldest, the older ones loaded stay below them; the folder listed
 * whole (fewer than asked) replaces everything */
static void merge_newest(const struct mail *mails, unsigned count, unsigned max)
{
	struct mail older[INBOX_MAX];
	unsigned kept = 0;
	if (count == max && count) {
		unsigned long oldest = mails[count - 1].uid;
		for (unsigned i = 0; i < inbox.count && kept + count < INBOX_MAX; i++)
			if (inbox.mails[i].uid < oldest)
				older[kept++] = inbox.mails[i];
	} else {
		inbox.more = false;
	}
	if (count)
		memcpy(inbox.mails, mails, count * sizeof(*mails));
	if (kept)
		memcpy(inbox.mails + count, older, kept * sizeof(*older));
	else if (count == max && count)
		inbox.more = true; /* nothing loaded below: there may be */
	inbox.count = count + kept;
}

/* Older messages than the last one here, appended; INBOX_MAX kept */
static void append_older(const struct mail *mails, unsigned count, unsigned max)
{
	unsigned long oldest = inbox.count ? inbox.mails[inbox.count - 1].uid : ~0UL;
	for (unsigned i = 0; i < count; i++)
		if (mails[i].uid < oldest) {
			if (inbox.count == INBOX_MAX) {
				inbox.more = true;
				return;
			}
			inbox.mails[inbox.count++] = mails[i];
			oldest = mails[i].uid;
		}
	inbox.more = count == max;
}

/* The list: a virtual one (ui_list_create()), its rows recycled as it scrolls */

static void row_click(unsigned index, void *user)
{
	LV_UNUSED(user);
	if (index < inbox.count)
		message_open(inbox.folder, &inbox.mails[index]);
}

static void row_free_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

/* A row's lines, kept in its user data */
static void row_make(lv_obj_t *row, void *user)
{
	LV_UNUSED(user);
	struct ui_row_lines *l = malloc(sizeof(*l));
	if (!l)
		return;
	*l = ui_row_lines(row, "", "", "");
	lv_obj_set_user_data(row, l);
	lv_obj_add_event_cb(row, row_free_cb, LV_EVENT_DELETE, l);
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

static void row_bind(lv_obj_t *row, unsigned index, void *user)
{
	LV_UNUSED(user);
	struct ui_row_lines *l = lv_obj_get_user_data(row);
	const struct mail *m = &inbox.mails[index];
	if (!l)
		return;
	lv_label_set_text(l->title, m->from);
	lv_label_set_text(l->time, time_text(m->date));
	lv_label_set_text(l->preview, m->subject[0] ? m->subject : _("(no subject)"));
	lv_obj_set_style_text_color(l->time, lv_color_hex(m->seen ? UI_COLOR_DIM : UI_COLOR_ACCENT), 0);
	lv_obj_set_style_text_color(l->preview, lv_color_hex(m->seen ? UI_COLOR_DIM : UI_COLOR_TEXT), 0);
}

static void fill(void)
{
	if (!v.list)
		return;
	ui_list_set_count(v.card, inbox.count);
	lv_obj_set_hidden(v.card, !inbox.count);
	more_show();
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

static void listed(int status, const struct mail *mails, unsigned count, unsigned long uidvalidity, void *user)
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
	uids_check(uidvalidity);
	merge_newest(mails, count, INBOX_PAGE);
	uint32_t t0 = lv_tick_get();
	cache_save();
	uint32_t t1 = lv_tick_get();
	fill();
	printf("email: inbox: cache %u ms, rows %u ms\n", (unsigned)(t1 - t0), (unsigned)(lv_tick_get() - t1));
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
	mail_list(inbox.folder, INBOX_PAGE, listed, &v);
}

/* The Load more row: what it says, if shown */
static void more_show(void)
{
	if (!v.more)
		return;
	bool shown = inbox.count && (inbox.more || v.loading_more);
	lv_obj_set_hidden(v.more, !shown);
	if (!shown)
		return;
	if (v.loading_more) {
		lv_label_set_text(v.more, _("Loading..."));
		lv_obj_set_style_text_color(v.more, lv_color_hex(UI_COLOR_DIM), 0);
	} else {
		lv_label_set_text(v.more, _("Load more"));
		lv_obj_set_style_text_color(v.more, lv_color_hex(UI_COLOR_ACCENT), 0);
	}
}

static void listed_older(int status, const struct mail *mails, unsigned count, unsigned long uidvalidity, void *user)
{
	LV_UNUSED(user);
	v.loading_more = false;
	if (status != MAIL_OK) {
		/* Said in the row, which asks again when tapped */
		char host[128], text[256];
		url_host(account.imap, host, sizeof(host));
		mail_error_text(status, host, text, sizeof(text));
		lv_obj_set_hidden(v.more, false);
		lv_label_set_text(v.more, text);
		lv_obj_set_style_text_color(v.more, lv_color_hex(UI_COLOR_RED), 0);
		return;
	}
	uids_check(uidvalidity);
	append_older(mails, count, INBOX_PAGE);
	cache_save();
	fill();
}

static void more_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (v.loading_more || !inbox.count)
		return;
	v.loading_more = true;
	more_show();
	mail_list_older(inbox.folder, inbox.mails[inbox.count - 1].uid, INBOX_PAGE, listed_older, &v);
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

	v.card = ui_list_create(v.list, ROW_H, row_make, row_bind, row_click, NULL);
	v.empty = ui_empty_label(v.list, _("No messages"));
	lv_obj_set_hidden(v.empty, true);

	/* Load more, below the list: a line of text in the accent color, tapped anywhere
	 * on its width */
	v.more = lv_label_create(v.list);
	lv_obj_set_width(v.more, LV_PCT(100));
	lv_label_set_long_mode(v.more, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(v.more, UI_FONT_BODY, 0);
	lv_obj_set_style_text_align(v.more, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_ver(v.more, 24, 0);
	lv_obj_set_clickable(v.more, true);
	lv_obj_add_event_cb(v.more, more_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(v.more, true);

	cache_load();
	fill();
	refresh(NULL);
}
