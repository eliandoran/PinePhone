/*
 * The account's folders: listed from the server (mail_list_folders()) each time their
 * screen opens, kept in folders.json and shown from there first, offline too. The
 * inbox first, then the special ones (sent, drafts, trash, junk, archive: told by the
 * server's attributes or their usual names, client.c), then the rest by name, each
 * with its unread count. The one open (FOLDER_FILE: its name; the inbox at first) is
 * what inbox.c lists; tapping one makes it that and goes back to its messages.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "email.h"
#include "ui_symbols.h"

#define ROW_H 96

static struct {
	struct folder *folders;
	unsigned count;
	bool loaded;           /* from the cache */
	struct folder current; /* the one open */
	bool current_known;
} fl;

/* The screen, while open */
static struct {
	lv_obj_t *list, *note, *card;
	bool loading;
} v;

static void fill(void);

void folder_file_name(const char *folder, char *out, size_t size)
{
	size_t o = 0;
	for (const char *p = folder; *p && o + 4 < size; p++) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
		    c == '_')
			out[o++] = (char)c;
		else
			o += (size_t)snprintf(out + o, size - o, "%%%02X", c);
	}
	out[o] = '\0';
}

const char *folder_title(const struct folder *f)
{
	switch (f->role) {
	case FOLDER_INBOX:
		return _("Inbox");
	case FOLDER_SENT:
		return _("Sent");
	case FOLDER_DRAFTS:
		return _("Drafts");
	case FOLDER_TRASH:
		return _("Trash");
	case FOLDER_JUNK:
		return _("Junk");
	case FOLDER_ARCHIVE:
		return _("Archive");
	case FOLDER_OTHER:
		break;
	}
	return f->title;
}

/* The cache */

static void cache_load(void)
{
	if (fl.loaded)
		return;
	fl.loaded = true;
	FILE *f = fopen(FOLDERS_FILE, "r");
	if (!f)
		return;
	char *buf = malloc(65536);
	size_t n = buf ? fread(buf, 1, 65535, f) : 0;
	fclose(f);
	if (!buf)
		return;
	buf[n] = '\0';
	struct json *list = json_parse(buf);
	free(buf);
	if (list && list->kind == JSON_ARRAY) {
		unsigned count = 0;
		for (struct json *j = list->child; j; j = j->next)
			count++;
		fl.folders = calloc(count ? count : 1, sizeof(*fl.folders));
		for (struct json *j = list->child; j && fl.folders; j = j->next) {
			struct folder *fo = &fl.folders[fl.count++];
			snprintf(fo->name, sizeof(fo->name), "%s", json_str(json_get(j, "name"), ""));
			snprintf(fo->title, sizeof(fo->title), "%s", json_str(json_get(j, "title"), ""));
			fo->role = (enum folder_role)json_int(json_get(j, "role"), FOLDER_OTHER);
			if (fo->role < FOLDER_OTHER || fo->role > FOLDER_ARCHIVE)
				fo->role = FOLDER_OTHER;
			fo->unseen = (unsigned)json_int(json_get(j, "unseen"), 0);
		}
	}
	json_free(list);
}

static void cache_save(void)
{
	FILE *f = fopen(FOLDERS_FILE ".new", "w");
	if (!f)
		return;
	fputs("[", f);
	for (unsigned i = 0; i < fl.count; i++) {
		const struct folder *fo = &fl.folders[i];
		char name[6 * sizeof(fo->name) + 1], title[6 * sizeof(fo->title) + 1];
		json_escape(name, sizeof(name), fo->name);
		json_escape(title, sizeof(title), fo->title);
		fprintf(f, "%s{\"name\": \"%s\", \"title\": \"%s\", \"role\": %d, \"unseen\": %u}", i ? ",\n " : "", name, title,
		        (int)fo->role, fo->unseen);
	}
	fputs("]\n", f);
	if (fclose(f) == 0)
		rename(FOLDERS_FILE ".new", FOLDERS_FILE);
}

/* The one open */

static void current_set(const struct folder *fo)
{
	fl.current = *fo;
	fl.current_known = true;
	FILE *f = fopen(FOLDER_FILE, "w");
	if (f) {
		fputs(fo->name, f);
		fclose(f);
	}
}

const struct folder *folder_current(void)
{
	if (fl.current_known)
		return &fl.current;
	fl.current_known = true;
	snprintf(fl.current.name, sizeof(fl.current.name), "INBOX"); /* NON-NLS */
	fl.current.role = FOLDER_INBOX;
	char name[128] = "";
	FILE *f = fopen(FOLDER_FILE, "r");
	if (f) {
		if (!fgets(name, sizeof(name), f))
			name[0] = '\0';
		fclose(f);
	}
	name[strcspn(name, "\r\n")] = '\0';
	cache_load();
	for (unsigned i = 0; name[0] && i < fl.count; i++)
		if (strcmp(fl.folders[i].name, name) == 0)
			fl.current = fl.folders[i];
	return &fl.current;
}

void folders_forget(void)
{
	unlink(FOLDERS_FILE);
	unlink(FOLDER_FILE);
	free(fl.folders);
	memset(&fl, 0, sizeof(fl));
}

/* The screen */

static const char *role_symbol(enum folder_role role)
{
	switch (role) {
	case FOLDER_INBOX:
		return LV_SYMBOL_ENVELOPE;
	case FOLDER_SENT:
		return LV_SYMBOL_UPLOAD;
	case FOLDER_DRAFTS:
		return LV_SYMBOL_EDIT;
	case FOLDER_TRASH:
		return LV_SYMBOL_TRASH;
	case FOLDER_JUNK:
		return UI_SYMBOL_BAN;
	case FOLDER_ARCHIVE:
		return LV_SYMBOL_SAVE;
	case FOLDER_OTHER:
		break;
	}
	return LV_SYMBOL_DIRECTORY;
}

static void row_cb(lv_event_t *e)
{
	const struct folder *fo = lv_event_get_user_data(e);
	current_set(fo);
	ui_pop();
	email_show();
}

static void fill(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.card);
	for (unsigned i = 0; i < fl.count; i++) {
		const struct folder *fo = &fl.folders[i];
		lv_obj_t *row = ui_row_create(v.card, ROW_H, i + 1 < fl.count, row_cb, &fl.folders[i]);
		lv_obj_t *symbol = lv_label_create(row);
		lv_obj_set_width(symbol, 56);
		lv_label_set_text_static(symbol, role_symbol(fo->role));
		lv_obj_set_style_text_font(symbol, &ui_font_24, 0);
		lv_obj_set_style_text_color(symbol, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *line = ui_row_line(text);
		lv_obj_t *title = lv_label_create(line);
		lv_obj_set_flex_grow(title, 1);
		lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
		lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_label_set_text(title, folder_title(fo));
		if (fo->unseen)
			ui_count_badge(line, fo->unseen);
	}
	lv_obj_set_hidden(v.card, !fl.count);
}

static void note(const char *text, uint32_t color)
{
	if (!v.list)
		return;
	if (text) {
		lv_label_set_text(v.note, text);
		lv_obj_set_style_text_color(v.note, lv_color_hex(color), 0);
	}
	lv_obj_set_hidden(v.note, !text);
}

static void listed(int status, const struct folder *folders, unsigned count, void *user)
{
	LV_UNUSED(user);
	v.loading = false;
	if (status != MAIL_OK) {
		char host[128], text[256];
		url_host(account.imap, host, sizeof(host));
		mail_error_text(status, host, text, sizeof(text));
		note(text, UI_COLOR_RED);
		ui_pull_refresh_done(v.list);
		return;
	}
	struct folder *copy = calloc(count ? count : 1, sizeof(*copy));
	if (!copy)
		return;
	if (count)
		memcpy(copy, folders, count * sizeof(*copy));
	free(fl.folders);
	fl.folders = copy;
	fl.count = count;
	cache_save();
	/* The one open, as the server has it now */
	for (unsigned i = 0; fl.current_known && i < fl.count; i++)
		if (strcmp(fl.folders[i].name, fl.current.name) == 0)
			fl.current = fl.folders[i];
	fill();
	note(NULL, 0);
	ui_pull_refresh_done(v.list);
}

static void refresh(void *unused)
{
	LV_UNUSED(unused);
	if (v.loading)
		return;
	v.loading = true;
	note(fl.count ? NULL : _("Loading..."), UI_COLOR_DIM);
	mail_list_folders(listed, &v);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	mail_cancel(&v);
	memset(&v, 0, sizeof(v));
}

void folders_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Folders"), NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	v.list = ui_box(body);
	lv_obj_set_width(v.list, LV_PCT(100));
	lv_obj_set_flex_grow(v.list, 1);
	lv_obj_set_flex_flow(v.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(v.list, 16, 0);
	lv_obj_set_style_pad_top(v.list, 8, 0);
	lv_obj_set_style_pad_bottom(v.list, 32, 0);
	lv_obj_set_style_pad_row(v.list, 12, 0);
	lv_obj_set_scrollable(v.list, true);
	lv_obj_set_clickable(v.list, true);
	lv_obj_set_scroll_dir(v.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(v.list, LV_SCROLLBAR_MODE_OFF);
	ui_pull_refresh(v.list, refresh, NULL);

	v.note = lv_label_create(v.list);
	lv_obj_set_width(v.note, LV_PCT(100));
	lv_label_set_long_mode(v.note, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(v.note, UI_FONT_BODY, 0);
	lv_obj_set_style_text_align(v.note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_ver(v.note, 16, 0);
	lv_obj_set_hidden(v.note, true);

	v.card = ui_card_create(v.list);

	cache_load();
	fill();
	refresh(NULL);
	ui_push(scr);
}
