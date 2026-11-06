/*
 * Files: browses the file system from / (read-only). A folder is a list of its
 * entries, folders first; each folder opened is a screen of its own, so back goes
 * up. A file shows its size, date and, for small text files, the start of it.
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
#include "ui.h"

#define ROW_H 112
#define MAX_ENTRIES 500  /* rows in a folder (/proc, /sys can have many) */
#define PREVIEW_SIZE 2048 /* bytes of a text file shown */
#define PREVIEW_LINES 40

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

/* Entries of `path`, folders first, at most MAX_ENTRIES (*more: how many were left
 * out). Returns how many, -1 if it cannot be read; *entries is to be freed. */
static int entries_load(const char *path, struct entry **entries, unsigned *more)
{
	*entries = NULL;
	*more = 0;
	DIR *dir = opendir(path);
	if (!dir)
		return -1;

	unsigned count = 0, size = 0;
	struct dirent *d;
	while ((d = readdir(dir))) {
		if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
			continue;
		if (count == MAX_ENTRIES) {
			(*more)++;
			continue;
		}
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

static lv_obj_t *dim_label(lv_obj_t *parent, const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
	return label;
}

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

static void file_open(const char *dir, const struct entry *entry)
{
	struct file *f = calloc(1, sizeof(*f));
	snprintf(f->dir, sizeof(f->dir), "%s", dir);
	path_join(f->path, sizeof(f->path), dir, entry->name);
	f->name = strrchr(f->path, '/') + 1;
	ui_size_format(f->size, sizeof(f->size), entry->size);
	ui_when_format(f->when, sizeof(f->when), entry->mtime);

	struct stat st;
	if (stat(f->path, &st) == 0 && S_ISREG(st.st_mode))
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
	}
	ui_push(scr);
}

/* Folders */

struct folder {
	char path[PATH_MAX];
	struct entry *entries; /* the rows' labels point into it */
};

static void folder_open(const char *path);

static void row_cb(lv_event_t *e)
{
	const struct entry *entry = lv_event_get_user_data(e);
	const struct folder *f = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
	if (entry->dir) {
		char path[PATH_MAX];
		if (path_join(path, sizeof(path), f->path, entry->name))
			folder_open(path);
	} else {
		file_open(f->path, entry);
	}
}

static void row_create(lv_obj_t *card, struct folder *f, const struct entry *entry, bool divider)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, (void *)entry);
	lv_obj_set_user_data(row, f);

	lv_obj_t *icon = lv_label_create(row);
	lv_label_set_text_static(icon, entry->dir ? LV_SYMBOL_DIRECTORY : file_symbol(entry->name));
	lv_obj_set_width(icon, 48);
	lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(icon, entry->dir ? lv_color_hex(UI_COLOR_ACCENT) :
	                            lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *text = ui_row_text(row);

	lv_obj_t *name = ui_one_line_label(text, entry->name, UI_FONT_TITLE);
	lv_obj_set_width(name, LV_PCT(100));
	if (!entry->dir) {
		lv_obj_t *info = ui_one_line_label(text, entry->info, UI_FONT_CAPTION);
		lv_obj_set_width(info, LV_PCT(100));
		lv_obj_set_style_text_color(info, lv_color_hex(UI_COLOR_DIM), 0);
	} else {
		lv_obj_t *arrow = lv_label_create(row);
		lv_label_set_text_static(arrow, LV_SYMBOL_RIGHT);
		lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
	}
}

static void folder_delete_cb(lv_event_t *e)
{
	struct folder *f = lv_event_get_user_data(e);
	free(f->entries);
	free(f);
}

static void folder_open(const char *path)
{
	struct folder *f = calloc(1, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	const char *title = strcmp(f->path, "/") ? strrchr(f->path, '/') + 1 : _("Files");

	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(title, NULL, &body);
	lv_obj_add_event_cb(scr, folder_delete_cb, LV_EVENT_DELETE, f);
	body_setup(body);

	unsigned more;
	int count = entries_load(f->path, &f->entries, &more);
	if (count <= 0) {
		if (count < 0)
			printf("files: %s: %s\n", f->path, strerror(errno));
		lv_obj_t *label = ui_empty_label(body, count < 0 ? _("Cannot open this folder") : _("Empty folder"));
		lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
		ui_push(scr);
		return;
	}

	lv_obj_t *card = ui_card_create(body);
	for (int i = 0; i < count; i++)
		row_create(card, f, &f->entries[i], i + 1 < count);
	if (more) {
		lv_obj_t *label = dim_label(body, "");
		ui_set_text_fmt(label, P_("%u more not shown", "%u more not shown", more), more);
	}
	ui_push(scr);
}

void files_open(void)
{
	folder_open("/");
}
