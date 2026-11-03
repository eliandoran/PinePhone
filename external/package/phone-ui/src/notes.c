/*
 * Notes: list of notes (newest first) and an editor. Each note is a plain text
 * file in /var/lib/notes; its first line is the title.
 */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#define NOTES "/var/lib/notes"
#define ROW_H 120
#define MAX_SIZE (64 * 1024) /* bytes read from a note */
#define COLOR_RED 0xff453a

/* Bumped when a note is saved or deleted: the list reloads */
static unsigned notes_version;

struct note {
	char name[64]; /* file name in NOTES */
	time_t mtime;
	char title[96];
	char preview[128];
};

/* Files */

/* Copies at most size - 1 bytes of s[0..n) to buf, cut at a character boundary */
static void copy_utf8(char *buf, size_t size, const char *s, size_t n)
{
	if (n >= size) {
		n = size - 1;
		while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(buf, s, n);
	buf[n] = '\0';
}

/* Title: first non-blank line; preview: the rest, with whitespace runs as one space */
static void note_summary(struct note *n, const char *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
		text++;
	size_t len = strcspn(text, "\r\n");
	while (len && (text[len - 1] == ' ' || text[len - 1] == '\t'))
		len--;
	if (len)
		copy_utf8(n->title, sizeof(n->title), text, len);
	else
		strcpy(n->title, "Untitled");

	char buf[sizeof(n->preview) * 2];
	size_t out = 0;
	bool space = false;
	for (const char *c = text + strcspn(text, "\r\n"); *c && out < sizeof(buf) - 1; c++) {
		if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
			space = out > 0;
			continue;
		}
		if (space && out < sizeof(buf) - 2)
			buf[out++] = ' ';
		space = false;
		buf[out++] = *c;
	}
	copy_utf8(n->preview, sizeof(n->preview), buf, out);
}

static int note_cmp(const void *a, const void *b)
{
	const struct note *x = a, *y = b;
	return (x->mtime < y->mtime) - (x->mtime > y->mtime);
}

/* Notes newest first; returns how many, *notes is to be freed */
static unsigned notes_load(struct note **notes)
{
	unsigned count = 0, size = 0;
	*notes = NULL;
	DIR *dir = opendir(NOTES);
	if (!dir)
		return 0;

	struct dirent *d;
	while ((d = readdir(dir))) {
		size_t len = strlen(d->d_name);
		if (len < 5 || len >= sizeof((*notes)->name) || strcmp(d->d_name + len - 4, ".txt") != 0)
			continue;
		char path[sizeof(NOTES) + sizeof((*notes)->name)];
		snprintf(path, sizeof(path), NOTES "/%s", d->d_name);
		struct stat st;
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
			fclose(f);
			continue;
		}
		char text[1024];
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[n] = '\0';

		if (count == size) {
			size = size ? size * 2 : 16;
			*notes = realloc(*notes, size * sizeof(**notes));
		}
		struct note *note = &(*notes)[count++];
		memcpy(note->name, d->d_name, len + 1);
		note->mtime = st.st_mtime;
		note_summary(note, text);
	}
	closedir(dir);
	qsort(*notes, count, sizeof(**notes), note_cmp);
	return count;
}

/* The whole note (up to MAX_SIZE), to be freed; "" if it cannot be read */
static char *note_read(const char *path)
{
	char *text = malloc(MAX_SIZE + 1);
	size_t n = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		n = fread(text, 1, MAX_SIZE, f);
		fclose(f);
	}
	text[n] = '\0';
	return text;
}

/* Through a temporary file, synced: the battery can cut power at any time */
static bool note_write(const char *path, const char *text)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		perror("phone-ui: notes");
		return false;
	}
	bool ok = fputs(text, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		perror("phone-ui: notes");
		unlink(tmp);
		return false;
	}
	return true;
}

/* A file name for a new note, from the current time */
static void note_new_path(char *path, size_t size)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

	snprintf(path, size, NOTES "/%s.txt", stamp);
	for (int i = 2; access(path, F_OK) == 0; i++)
		snprintf(path, size, NOTES "/%s-%d.txt", stamp, i);
}

/* Editor */

struct editor {
	char path[256]; /* empty: new note, not saved yet */
	char *saved;    /* text as last saved */
	lv_obj_t *ta, *kb, *confirm;
};

static bool blank(const char *s)
{
	return s[strspn(s, " \t\r\n")] == '\0';
}

static void editor_save(struct editor *ed)
{
	const char *text = lv_textarea_get_text(ed->ta);
	if (strcmp(text, ed->saved) == 0)
		return;

	/* Emptied: nothing to keep */
	if (blank(text)) {
		if (ed->path[0] && unlink(ed->path) == 0)
			notes_version++;
		ed->path[0] = '\0';
	} else {
		if (!ed->path[0]) {
			mkdir(NOTES, 0755);
			note_new_path(ed->path, sizeof(ed->path));
		}
		if (!note_write(ed->path, text))
			return;
		notes_version++;
	}
	free(ed->saved);
	ed->saved = strdup(text);
}

static void keyboard_show(struct editor *ed, bool show)
{
	ui_keyboard_show(ed->kb, show);
	if (show) {
		lv_obj_set_hidden(ed->confirm, true);
		/* The text area shrank: scroll the cursor back into view */
		lv_obj_update_layout(ed->ta);
		lv_textarea_set_cursor_pos(ed->ta, lv_textarea_get_cursor_pos(ed->ta));
	} else {
		editor_save(ed);
	}
}

static void ta_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		keyboard_show(ed, true);
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		keyboard_show(ed, false);
		break;
	default:
		break;
	}
}

static void trash_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	keyboard_show(ed, false);
	lv_obj_set_hidden(ed->confirm, !lv_obj_is_hidden(ed->confirm));
}

static void cancel_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	lv_obj_set_hidden(ed->confirm, true);
}

static void delete_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	if (ed->path[0] && unlink(ed->path) == 0)
		notes_version++;
	ed->path[0] = '\0';
	/* Nothing left to save when the screen goes */
	lv_textarea_set_text(ed->ta, "");
	free(ed->saved);
	ed->saved = strdup("");
	ui_vibrate(15);
	ui_pop();
}

/* Leaving the editor saves the note. The screen is deleted at the end of the
 * transition, after the list reloaded: save when it starts (sent before the
 * list's LV_EVENT_SCREEN_LOAD_START). */
static void editor_unload_cb(lv_event_t *e)
{
	editor_save(lv_event_get_user_data(e));
}

static void editor_delete_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	editor_save(ed);
	free(ed->saved);
	free(ed);
}

static lv_obj_t *confirm_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t color,
                                lv_event_cb_t cb, struct editor *ed)
{
	lv_obj_t *button = lv_button_create(parent);
	lv_obj_set_height(button, 80);
	lv_obj_set_flex_grow(button, 1);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(button, lv_color_hex(bg), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, ed);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_center(label);
	return button;
}

/* `name`: file name in NOTES, NULL for a new note */
static void editor_open(const char *name)
{
	struct editor *ed = calloc(1, sizeof(*ed));
	if (name) {
		snprintf(ed->path, sizeof(ed->path), NOTES "/%s", name);
		ed->saved = note_read(ed->path);
	} else {
		ed->saved = strdup("");
	}

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(name ? "Note" : "New note", &header, &body);
	lv_obj_add_event_cb(scr, editor_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, ed);
	lv_obj_add_event_cb(scr, editor_delete_cb, LV_EVENT_DELETE, ed);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, ed);
	lv_obj_set_scrollable(body, false);

	ed->ta = lv_textarea_create(body);
	lv_textarea_set_placeholder_text(ed->ta, "Note");
	lv_textarea_set_text(ed->ta, ed->saved);
	lv_obj_set_width(ed->ta, LV_PCT(100));
	lv_obj_set_flex_grow(ed->ta, 1);
	lv_obj_set_style_pad_hor(ed->ta, 28, 0);
	lv_obj_set_style_pad_ver(ed->ta, 16, 0);
	lv_obj_set_style_bg_opa(ed->ta, LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(ed->ta, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_border_width(ed->ta, 0, 0);
	lv_obj_set_style_border_width(ed->ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ed->ta, 0, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(ed->ta, 0, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_shadow_width(ed->ta, 0, 0);
	lv_obj_set_scrollbar_mode(ed->ta, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(ed->ta, ta_cb, LV_EVENT_CLICKED, ed);
	lv_obj_add_event_cb(ed->ta, ta_cb, LV_EVENT_READY, ed);
	lv_obj_add_event_cb(ed->ta, ta_cb, LV_EVENT_CANCEL, ed);

	/* Delete confirmation, shown by the trash button */
	ed->confirm = lv_obj_create(body);
	lv_obj_remove_style_all(ed->confirm);
	lv_obj_set_size(ed->confirm, LV_PCT(100), 112);
	lv_obj_set_flex_flow(ed->confirm, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ed->confirm, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(ed->confirm, 16, 0);
	lv_obj_set_style_pad_column(ed->confirm, 12, 0);
	lv_obj_set_style_bg_opa(ed->confirm, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ed->confirm, lv_color_hex(UI_COLOR_BAR), 0);
	lv_obj_set_clickable(ed->confirm, false);
	lv_obj_set_scrollable(ed->confirm, false);
	confirm_button(ed->confirm, "Cancel", UI_COLOR_BUTTON, UI_COLOR_TEXT, cancel_cb, ed);
	confirm_button(ed->confirm, LV_SYMBOL_TRASH "  Delete note", COLOR_RED, 0xffffff, delete_cb, ed);
	lv_obj_set_hidden(ed->confirm, true);

	/* Under the text, shown while typing; the text shrinks above it */
	ed->kb = ui_keyboard_create(body, ed->ta);

	/* A new note starts with the keyboard, an existing one reads first */
	lv_textarea_set_cursor_pos(ed->ta, name ? 0 : LV_TEXTAREA_CURSOR_LAST);
	if (!name)
		ui_keyboard_show(ed->kb, true);
	ui_push(scr);
}

/* List */

struct list {
	lv_obj_t *body;
	struct note *notes; /* the rows' labels point into it */
	unsigned count, version;
};

static lv_obj_t *one_line_label(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	/* Clipped to one line: no dots, they would be written into the static text */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

/* Time today, else the day (and the year if it is not this one) */
static void when_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	const char *fmt = tm.tm_year != today.tm_year ? "%-d %b %Y" :
	                  tm.tm_yday != today.tm_yday ? "%-d %b" : "%H:%M";
	strftime(buf, size, fmt, &tm);
}

static void row_cb(lv_event_t *e)
{
	const struct note *n = lv_event_get_user_data(e);
	editor_open(n->name);
}

static void row_create(lv_obj_t *card, const struct note *n, bool divider)
{
	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_hor(row, 28, 0);
	lv_obj_set_style_pad_row(row, 8, 0);
	lv_obj_set_style_radius(row, 24, 0);
	lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	if (divider) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(row, 1, 0);
		lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_LINE), 0);
	}
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)n);

	lv_obj_t *title = one_line_label(row, n->title, &lv_font_montserrat_26);
	lv_obj_set_width(title, LV_PCT(100));

	lv_obj_t *bottom = lv_obj_create(row);
	lv_obj_remove_style_all(bottom);
	lv_obj_set_size(bottom, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(bottom, 16, 0);
	lv_obj_set_clickable(bottom, false);
	lv_obj_set_scrollable(bottom, false);

	char buf[32];
	when_format(buf, sizeof(buf), n->mtime);
	lv_obj_t *when = lv_label_create(bottom);
	ui_set_text(when, buf);
	lv_obj_set_style_text_font(when, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(when, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *preview = one_line_label(bottom, n->preview, &lv_font_montserrat_20);
	lv_obj_set_flex_grow(preview, 1);
	lv_obj_set_style_text_color(preview, lv_color_hex(UI_COLOR_DIM), 0);
}

static void list_fill(struct list *l)
{
	lv_obj_clean(l->body);
	free(l->notes);
	l->count = notes_load(&l->notes);
	l->version = notes_version;

	if (!l->count) {
		lv_obj_t *label = lv_label_create(l->body);
		lv_label_set_text_static(label, "No notes");
		lv_obj_set_width(label, LV_PCT(100));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_margin_top(label, 200, 0);
		return;
	}

	lv_obj_t *card = lv_obj_create(l->body);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_clickable(card, false);
	lv_obj_set_scrollable(card, false);

	for (unsigned i = 0; i < l->count; i++)
		row_create(card, &l->notes[i], i + 1 < l->count);
}

/* Back from the editor: a note may have changed */
static void list_load_cb(lv_event_t *e)
{
	struct list *l = lv_event_get_user_data(e);
	if (l->version != notes_version)
		list_fill(l);
}

static void list_delete_cb(lv_event_t *e)
{
	struct list *l = lv_event_get_user_data(e);
	free(l->notes);
	free(l);
}

static void new_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	editor_open(NULL);
}

void notes_open(void)
{
	struct list *l = calloc(1, sizeof(*l));
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create("Notes", &header, &l->body);
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, l);
	ui_header_button(header, LV_SYMBOL_PLUS, new_cb, NULL);
	lv_obj_set_style_pad_hor(l->body, 16, 0);
	lv_obj_set_style_pad_top(l->body, 8, 0);
	lv_obj_set_style_pad_bottom(l->body, 32, 0);
	lv_obj_set_scroll_dir(l->body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(l->body, LV_SCROLLBAR_MODE_OFF);
	list_fill(l);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, l);
	ui_push(scr);
}
