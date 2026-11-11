/*
 * Files: browses the file system (read-only). It starts at its places: the apps'
 * folders, those a computer sees over MTP (folders.c), each with its app's icon, then
 * phone-ui's logs, then All files, from /. A folder is a list of its entries, folders
 * first (a virtual list: rows made for those in view only, so /proc and /sys open at
 * once); each folder opened is a screen of its own, so back goes up; pulled down, a folder
 * is read again. A file shows its size, date and, for text files, the start of it, and
 * the whole of it on a screen of its own; a log (in Logs) opens there at once, at its end.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "system/folders.h"
#include "ui_symbols.h"
#include "ui.h"

#define ROW_H 112
#define PREVIEW_SIZE 2048 /* bytes of a text file shown */
#define PREVIEW_LINES 40
#define PLACE_ICON 64 /* an app's icon on a place's row */

struct entry {
	char name[NAME_MAX + 1];
	bool dir;
	off_t size;
	time_t mtime;
	char info[56]; /* "12.3 kB, 3 Nov" */
};

/* Formatting */

static const char *file_symbol(const char *name)
{
	static const struct { const char *ext, *symbol; } kinds[] = {
		{ "png", LV_SYMBOL_IMAGE }, { "jpg", LV_SYMBOL_IMAGE }, { "jpeg", LV_SYMBOL_IMAGE },
		{ "ppm", LV_SYMBOL_IMAGE }, { "bmp", LV_SYMBOL_IMAGE },
		{ "mp3", LV_SYMBOL_AUDIO }, { "ogg", LV_SYMBOL_AUDIO }, { "wav", LV_SYMBOL_AUDIO },
		{ "flac", LV_SYMBOL_AUDIO },
		{ "mp4", LV_SYMBOL_VIDEO }, { "mkv", LV_SYMBOL_VIDEO }, { "webm", LV_SYMBOL_VIDEO },
		{ "txt", LV_SYMBOL_EDIT },
	};
	const char *dot = strrchr(name, '.');
	if (dot && dot != name)
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
			if (strcasecmp(dot + 1, kinds[i].ext) == 0)
				return kinds[i].symbol;
	return LV_SYMBOL_FILE;
}

/* Reading */

/* `dir`/`name` into buf; false if it does not fit */
static bool path_join(char *buf, size_t size, const char *dir, const char *name)
{
	size_t d = strcmp(dir, "/") ? strlen(dir) : 0, n = strlen(name);
	if (d + 1 + n >= size)
		return false;
	memcpy(buf, dir, d);
	buf[d] = '/';
	memcpy(buf + d + 1, name, n + 1);
	return true;
}

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}

/* Entries of `path`, folders first. Returns how many, -1 if it cannot be read; *entries is to be freed. */
static int entries_load(const char *path, struct entry **entries)
{
	*entries = NULL;
	DIR *dir = opendir(path);
	if (!dir)
		return -1;

	unsigned count = 0, size = 0;
	struct dirent *d;
	while ((d = readdir(dir))) {
		if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
			continue;
		if (count == size) {
			size = size ? size * 2 : 32;
			*entries = realloc(*entries, size * sizeof(**entries));
		}
		struct entry *e = &(*entries)[count++];
		snprintf(e->name, sizeof(e->name), "%s", d->d_name);

		/* Links as what they point to; broken ones as themselves */
		struct stat st;
		if (fstatat(dirfd(dir), d->d_name, &st, 0) != 0 &&
		    fstatat(dirfd(dir), d->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			memset(&st, 0, sizeof(st));
		e->dir = S_ISDIR(st.st_mode);
		e->size = st.st_size;
		e->mtime = st.st_mtime;

		char size_buf[24], when[24];
		ui_size_format(size_buf, sizeof(size_buf), e->size);
		ui_when_format(when, sizeof(when), e->mtime);
		snprintf(e->info, sizeof(e->info), "%s, %s", size_buf, when);
	}
	closedir(dir);
	if (count) /* none: no array */
		qsort(*entries, count, sizeof(**entries), entry_cmp);
	return (int)count;
}

/* The start of a regular file as text, NULL if it is empty, binary or unreadable.
 * Non-blocking: some files block until there is something to read (/proc/kmsg). */
static char *preview_read(const char *path)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	char *text = malloc(PREVIEW_SIZE + 1);
	ssize_t n = read(fd, text, PREVIEW_SIZE);
	close(fd);
	if (n <= 0 || memchr(text, '\0', n)) {
		free(text);
		return NULL;
	}

	/* Up to PREVIEW_LINES lines, cut at a character boundary, control characters as dots */
	ssize_t end = 0;
	for (int lines = 0; end < n && lines < PREVIEW_LINES; end++) {
		if (text[end] == '\n')
			lines++;
		else if ((unsigned char)text[end] < 0x20 && text[end] != '\t')
			text[end] = '.';
	}
	/* Cut in the middle of a character: drop it */
	if (end == PREVIEW_SIZE) {
		while (end && ((unsigned char)text[end - 1] & 0xc0) == 0x80)
			end--;
		if (end && (unsigned char)text[end - 1] >= 0xc0)
			end--;
	}
	while (end && text[end - 1] == '\n')
		end--;
	text[end] = '\0';
	return text;
}

/* Screens */

static lv_obj_t *body_setup(lv_obj_t *body)
{
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_top(body, 8, 0);
	lv_obj_set_style_pad_bottom(body, 32, 0);
	lv_obj_set_style_pad_row(body, 16, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
	return body;
}

/* A text file whole: its lines in a virtual list (rows made for those in view only, so a
 * log of megabytes opens at once), in the monospace font, a line longer than a row
 * wrapped onto the next ones (rows of TEXT_ROW_H, so the list is a plain virtual list).
 * Its last TEXT_MAX bytes at most, from the line after. Read again each TEXT_POLL_MS:
 * what was added comes in (the last line, unfinished, wrapped again), and while the list is
 * at its end it stays there, as tail -F; shorter than what was read (rotated, replaced),
 * read again from its start */

#define TEXT_ROW_H 28
#define TEXT_PAD 16 /* a row's sides */
#define TEXT_MAX (4 * 1024 * 1024)
#define TEXT_POLL_MS 1000

struct text_row {
	uint32_t start; /* in buf */
	uint32_t len;   /* bytes */
};

struct text_view {
	char path[PATH_MAX], title[NAME_MAX + 1];
	char *buf;
	size_t len, cap;
	off_t read_to;          /* the file's offset buf ends at */
	struct text_row *rows;
	unsigned count, cap_rows;
	size_t done;            /* buf's complete lines end here (after their newline) */
	unsigned done_rows;     /* and their rows */
	unsigned cols;          /* characters a row holds */
	lv_obj_t *body, *card;
	lv_timer_t *timer;
};

static void text_row_add(struct text_view *t, size_t start, size_t len)
{
	if (t->count == t->cap_rows) {
		unsigned cap = t->cap_rows ? t->cap_rows * 2 : 1024;
		struct text_row *rows = realloc(t->rows, cap * sizeof(*rows));
		if (!rows)
			return;
		t->rows = rows;
		t->cap_rows = cap;
	}
	t->rows[t->count++] = (struct text_row){ (uint32_t)start, (uint32_t)len };
}

/* buf's lines from `done` into rows, cols characters each (UTF-8: continuation bytes do
 * not count); complete ones counted as done, the unfinished last one not */
static void text_wrap(struct text_view *t)
{
	t->count = t->done_rows;
	size_t i = t->done;
	while (i < t->len) {
		size_t end = i;
		while (end < t->len && t->buf[end] != '\n')
			end++;
		size_t start = i;
		unsigned chars = 0;
		for (size_t j = i; j < end; j++) {
			if (((unsigned char)t->buf[j] & 0xc0) == 0x80)
				continue;
			if (chars == t->cols) {
				text_row_add(t, start, j - start);
				start = j;
				chars = 0;
			}
			chars++;
		}
		text_row_add(t, start, end - start);
		if (end == t->len)
			break; /* unfinished: wrapped again with what comes */
		i = end + 1;
		t->done = i;
		t->done_rows = t->count;
	}
}

/* What the file has past read_to appended to buf (all of it again when it got shorter),
 * shown as text: tabs as spaces, other control characters as dots. False if unchanged */
static bool text_read(struct text_view *t)
{
	struct stat st;
	if (stat(t->path, &st) != 0 || st.st_size == t->read_to)
		return false;
	bool again = st.st_size < t->read_to;
	off_t from = again || !t->buf ? (st.st_size > TEXT_MAX ? st.st_size - TEXT_MAX : 0) : t->read_to;
	if (again || !t->buf) {
		t->len = t->done = 0;
		t->count = t->done_rows = 0;
	}
	size_t n = (size_t)(st.st_size - from);
	if (t->len + n + 1 > t->cap) {
		char *buf = realloc(t->buf, t->len + n + 1);
		if (!buf)
			return false;
		t->buf = buf;
		t->cap = t->len + n + 1;
	}
	int fd = open(t->path, O_RDONLY);
	ssize_t got = fd >= 0 ? pread(fd, t->buf + t->len, n, from) : -1;
	if (fd >= 0)
		close(fd);
	if (got <= 0)
		return false;
	size_t start = t->len, skip = 0;
	if (from > 0 && t->len == 0) { /* cut: from the line after */
		while (skip < (size_t)got && t->buf[start + skip] != '\n')
			skip++;
		if (skip < (size_t)got)
			skip++;
		memmove(t->buf, t->buf + skip, (size_t)got - skip);
	}
	t->len += (size_t)got - skip;
	for (size_t i = start; i < t->len; i++) {
		unsigned char c = (unsigned char)t->buf[i];
		if (c == '\t')
			t->buf[i] = ' ';
		else if ((c < 0x20 && c != '\n') || c == 0x7f)
			t->buf[i] = '.';
	}
	t->read_to = from + got;
	text_wrap(t);
	return true;
}

static void text_row_make(lv_obj_t *row, void *user_data)
{
	(void)user_data;
	lv_obj_set_clickable(row, false); /* the list scrolls under the finger */
	lv_obj_set_style_pad_hor(row, TEXT_PAD, 0);
	lv_obj_t *label = lv_label_create(row);
	lv_obj_set_style_text_font(label, &ui_font_mono_20, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_user_data(row, label);
}

static void text_row_bind(lv_obj_t *row, unsigned index, void *user_data)
{
	const struct text_view *t = user_data;
	const struct text_row *r = &t->rows[index];
	ui_row_set_divider(row, false);
	lv_label_set_text_fmt(lv_obj_get_user_data(row), "%.*s", (int)r->len, t->buf + r->start);
}

static bool text_at_end(const struct text_view *t)
{
	return lv_obj_get_scroll_bottom(t->body) <= TEXT_ROW_H;
}

static void text_to_end(struct text_view *t)
{
	lv_obj_update_layout(t->body);
	lv_obj_scroll_to_y(t->body, lv_obj_get_scroll_y(t->body) + lv_obj_get_scroll_bottom(t->body), LV_ANIM_OFF);
}

static void text_poll_cb(lv_timer_t *timer)
{
	struct text_view *t = lv_timer_get_user_data(timer);
	bool at_end = text_at_end(t);
	if (!text_read(t))
		return;
	ui_list_set_count(t->card, t->count);
	if (at_end)
		text_to_end(t);
}

static void text_delete_cb(lv_event_t *e)
{
	struct text_view *t = lv_event_get_user_data(e);
	lv_timer_delete(t->timer);
	free(t->buf);
	free(t->rows);
	free(t);
}

/* `at_end`: opened at its end (a log), else at its top */
static void text_open(const char *path, const char *title, bool at_end)
{
	struct text_view *t = calloc(1, sizeof(*t));
	if (!t)
		return;
	snprintf(t->path, sizeof(t->path), "%s", path);
	snprintf(t->title, sizeof(t->title), "%s", title);
	t->read_to = -1;

	lv_obj_t *scr = ui_app_screen_create(t->title, NULL, &t->body);
	lv_obj_add_event_cb(scr, text_delete_cb, LV_EVENT_DELETE, t);
	body_setup(t->body);
	lv_obj_set_clickable(t->body, true); /* dragged beside the rows too */
	t->card = ui_list_create(t->body, TEXT_ROW_H, text_row_make, text_row_bind, NULL, t);
	lv_obj_update_layout(t->body);
	int32_t glyph = lv_font_get_glyph_width(&ui_font_mono_20, 'x', 0);
	int32_t width = lv_obj_get_content_width(t->card) - 2 * TEXT_PAD;
	t->cols = glyph > 0 && width > glyph ? (unsigned)(width / glyph) : 40;
	text_read(t);
	ui_list_set_count(t->card, t->count);
	if (at_end)
		text_to_end(t);
	t->timer = lv_timer_create(text_poll_cb, TEXT_POLL_MS, t);
	ui_push(scr);
}

/* File details */

struct file {
	char path[PATH_MAX], dir[PATH_MAX];
	const char *name; /* in path, the screen's title */
	char size[24], when[24];
	char *preview;
};

static void file_delete_cb(lv_event_t *e)
{
	struct file *f = lv_event_get_user_data(e);
	free(f->preview);
	free(f);
}

/* A row of the details card: title on the left, value on the right */
static void detail_row(lv_obj_t *card, const char *title, const char *value, bool divider)
{
	lv_obj_t *row = ui_row_create(card, 88, divider, NULL, NULL);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, title);
	lv_obj_t *v = ui_one_line_label(row, value, UI_FONT_BODY);
	lv_obj_set_flex_grow(v, 1);
	lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_color(v, lv_color_hex(UI_COLOR_DIM), 0);
}

static void whole_cb(lv_event_t *e)
{
	const struct file *f = lv_event_get_user_data(e);
	text_open(f->path, f->name, false);
}

static void file_open(const char *dir, const struct entry *entry)
{
	struct file *f = calloc(1, sizeof(*f));
	snprintf(f->dir, sizeof(f->dir), "%s", dir);
	path_join(f->path, sizeof(f->path), dir, entry->name);
	f->name = strrchr(f->path, '/') + 1;
	ui_size_format(f->size, sizeof(f->size), entry->size);
	ui_when_format(f->when, sizeof(f->when), entry->mtime);

	/* As it is now: its folder may have been read a while ago */
	struct stat st;
	bool now = stat(f->path, &st) == 0;
	if (now) {
		ui_size_format(f->size, sizeof(f->size), st.st_size);
		ui_when_format(f->when, sizeof(f->when), st.st_mtime);
	}
	if (now && S_ISREG(st.st_mode))
		f->preview = preview_read(f->path);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(f->name, NULL, &body);
	lv_obj_add_event_cb(scr, file_delete_cb, LV_EVENT_DELETE, f);
	body_setup(body);

	lv_obj_t *icon = lv_label_create(body);
	lv_label_set_text_static(icon, file_symbol(f->name));
	lv_obj_set_width(icon, LV_PCT(100));
	lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_ver(icon, 24, 0);

	lv_obj_t *card = ui_card_create(body);
	detail_row(card, _("Size"), f->size, true);
	detail_row(card, _("Modified"), f->when, true);
	detail_row(card, _("Folder"), f->dir, false);

	if (f->preview) {
		lv_obj_t *text = ui_card_create(body);
		lv_obj_set_style_pad_hor(text, 28, 0);
		lv_obj_set_style_pad_ver(text, 24, 0);
		lv_obj_t *label = lv_label_create(text);
		lv_label_set_text_static(label, f->preview);
		lv_obj_set_width(label, LV_PCT(100));
		lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);

		card = ui_card_create(body);
		lv_obj_t *row = ui_row_create(card, 88, false, whole_cb, f);
		lv_obj_t *whole = lv_label_create(row);
		lv_label_set_text_static(whole, _("Show the whole file"));
		lv_obj_set_flex_grow(whole, 1);
		lv_obj_t *arrow = lv_label_create(row);
		lv_label_set_text_static(arrow, LV_SYMBOL_RIGHT);
		lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
	}
	ui_push(scr);
}

/* Folders */

struct folder {
	char path[PATH_MAX];
	char title[NAME_MAX + 1];
	struct entry *entries;
	unsigned count;
	lv_obj_t *body, *card, *empty;
};

static void folder_open(const char *path, const char *title);

/* Its entries in a virtual list (ui_list_create()), rows made for those in view */

static void entry_bind(lv_obj_t *row, unsigned index, void *user_data)
{
	const struct folder *f = user_data;
	const struct entry *e = &f->entries[index];
	ui_list_row_set(row, e->dir ? LV_SYMBOL_DIRECTORY : file_symbol(e->name), e->dir ? UI_COLOR_ACCENT : UI_COLOR_DIM,
	                e->name, e->dir ? NULL : e->info, e->dir);
}

static void entry_click(unsigned index, void *user_data)
{
	const struct folder *f = user_data;
	const struct entry *entry = &f->entries[index];
	if (entry->dir) {
		char path[PATH_MAX];
		if (path_join(path, sizeof(path), f->path, entry->name))
			folder_open(path, entry->name);
	} else if (strcmp(f->path, FOLDERS_LOGS) == 0) {
		char path[PATH_MAX];
		if (path_join(path, sizeof(path), f->path, entry->name))
			text_open(path, entry->name, true); /* the latest lines */
	} else {
		file_open(f->path, entry);
	}
}

static void folder_delete_cb(lv_event_t *e)
{
	struct folder *f = lv_event_get_user_data(e);
	free(f->entries);
	free(f);
}

/* The folder's entries read (again) into its list */
static void folder_fill(struct folder *f)
{
	free(f->entries);
	f->entries = NULL;
	int count = entries_load(f->path, &f->entries);
	if (count < 0)
		printf("files: %s: %s\n", f->path, strerror(errno));
	f->count = count > 0 ? (unsigned)count : 0;
	ui_list_set_count(f->card, f->count);
	lv_obj_set_hidden(f->card, !f->count);
	lv_label_set_text_static(f->empty, count < 0 ? _("Cannot open this folder") : _("Empty folder"));
	lv_obj_set_hidden(f->empty, f->count);
}

static void pull_cb(void *user_data)
{
	struct folder *f = user_data;
	folder_fill(f);
	ui_pull_refresh_done(f->body);
}

/* `title`: the screen's, kept (a row's entry goes when its folder is read again) */
static void folder_open(const char *path, const char *title)
{
	struct folder *f = calloc(1, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	snprintf(f->title, sizeof(f->title), "%s", title);

	lv_obj_t *scr = ui_app_screen_create(f->title, NULL, &f->body);
	lv_obj_add_event_cb(scr, folder_delete_cb, LV_EVENT_DELETE, f);
	body_setup(f->body);
	lv_obj_set_clickable(f->body, true); /* dragged beside the rows too */
	ui_pull_refresh(f->body, pull_cb, f);
	f->card = ui_list_create(f->body, ROW_H, ui_list_row_make, entry_bind, entry_click, f);
	f->empty = ui_empty_label(f->body, "");
	lv_obj_set_style_text_font(f->empty, UI_FONT_BODY, 0);
	folder_fill(f);
	ui_push(scr);
}

/* Places */

/* A folder's name as shown: its app's (translated) when it is named after it */
static const char *place_name(const struct app_folder *f, const struct ui_app *app)
{
	return app && strcmp(f->name, f->app) == 0 ? ui_app_title(app) : _(f->name);
}

static void place_cb(lv_event_t *e)
{
	const struct app_folder *f = lv_event_get_user_data(e);
	folder_open(f->path, place_name(f, ui_app_find(f->app)));
}

static void logs_cb(lv_event_t *e)
{
	(void)e;
	folder_open(FOLDERS_LOGS, _("Logs"));
}

static void root_cb(lv_event_t *e)
{
	(void)e;
	folder_open("/", _("All files"));
}

/* `app`'s icon, else `symbol` */
static void place_row(lv_obj_t *card, const struct ui_app *app, const char *symbol, const char *name,
                      const char *path, bool divider, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, cb, user_data);

	if (app) {
		ui_app_icon_create(row, app, PLACE_ICON);
	} else {
		lv_obj_t *icon = lv_label_create(row);
		lv_label_set_text_static(icon, symbol);
		lv_obj_set_width(icon, PLACE_ICON);
		lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(icon, &ui_font_32, 0);
		lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT), 0);
	}

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *label = ui_one_line_label(text, name, UI_FONT_TITLE);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_t *where = ui_one_line_label(text, path, UI_FONT_CAPTION);
	lv_obj_set_width(where, LV_PCT(100));
	lv_obj_set_style_text_color(where, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *arrow = lv_label_create(row);
	lv_label_set_text_static(arrow, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
}

void files_open(void)
{
	unsigned count;
	const struct app_folder *folders = folders_get(&count);

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Files"), NULL, &body);
	body_setup(body);

	lv_obj_t *card = ui_card_create(body);
	for (unsigned i = 0; i < count; i++) {
		const struct ui_app *app = ui_app_find(folders[i].app);
		place_row(card, app, LV_SYMBOL_DIRECTORY, place_name(&folders[i], app), folders[i].path,
		          true, place_cb, (void *)&folders[i]);
	}
	place_row(card, NULL, UI_SYMBOL_LIST_UL, _("Logs"), FOLDERS_LOGS, false, logs_cb, NULL);
	card = ui_card_create(body);
	place_row(card, NULL, LV_SYMBOL_DRIVE, _("All files"), "/", false, root_cb, NULL);
	ui_push(scr);
}
