/*
 * Tasks: a list of things to do, typed into the field at the bottom (Enter adds one and
 * leaves the keyboard up for the next), tapped to mark them done or not. Those to do in
 * the order added, then those done, the latest first. Saved in /var/lib/tasks/tasks.json:
 * {"tasks": [{"title": ..., "created": time, "done": time or 0}, ...]}, in the order they
 * were added or last marked done.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "json.h"
#include "ui.h"

#define TASKS "/var/lib/tasks"
#define TASKS_FILE TASKS "/tasks.json"
#define MAX_FILE (256 * 1024)
#define MAX_TITLE 200 /* characters typed */
#define ROW_H 112
#define CHECK_SIZE 44
#define FIELD_H 80

struct task {
	char *title;
	long long created, done; /* times; done 0: still to do */
};

static struct task *tasks;
static unsigned task_count, task_size;
static bool tasks_loaded;

static struct {
	lv_obj_t *list, *ta, *kb;
} tk;

/* File */

static void task_append(const char *title, long long created, long long done)
{
	if (task_count == task_size) {
		task_size = task_size ? task_size * 2 : 16;
		tasks = realloc(tasks, task_size * sizeof(*tasks));
	}
	tasks[task_count++] = (struct task){ strdup(title), created, done };
}

static void tasks_load(void)
{
	if (tasks_loaded)
		return;
	tasks_loaded = true;

	FILE *f = fopen(TASKS_FILE, "r");
	if (!f)
		return;
	char *text = malloc(MAX_FILE + 1);
	size_t n = fread(text, 1, MAX_FILE, f);
	fclose(f);
	text[n] = '\0';

	struct json *root = json_parse(text);
	free(text);
	struct json *list = json_get(root, "tasks");
	if (!list || list->kind != JSON_ARRAY)
		printf("phone-ui: tasks: " TASKS_FILE " has no list of tasks, ignored\n");
	else
		for (struct json *v = list->child; v; v = v->next) {
			const char *title = json_str(json_get(v, "title"), "");
			if (*title)
				task_append(title, json_int(json_get(v, "created"), 0), json_int(json_get(v, "done"), 0));
		}
	json_free(root);
}

/* Through a temporary file, synced: the battery can cut power at any time */
static bool tasks_save(void)
{
	mkdir(TASKS, 0755);
	FILE *f = fopen(TASKS_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: tasks");
		return false;
	}
	fputs("{\"tasks\": [", f);
	for (unsigned i = 0; i < task_count; i++) {
		size_t size = 6 * strlen(tasks[i].title) + 1;
		char *title = malloc(size);
		json_escape(title, size, tasks[i].title);
		fprintf(f, "%s\n\t{\"title\": \"%s\", \"created\": %lld, \"done\": %lld}", i ? "," : "", title,
		        tasks[i].created, tasks[i].done);
		free(title);
	}
	fputs("\n]}\n", f);
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(TASKS_FILE ".new", TASKS_FILE) != 0) {
		perror("phone-ui: tasks");
		unlink(TASKS_FILE ".new");
		return false;
	}
	return true;
}

/* List */

static void list_fill(void);

/* Filled again where it was scrolled to, if still open */
static void refill_async(void *data)
{
	LV_UNUSED(data);
	if (!tk.list)
		return;
	int32_t y = lv_obj_get_scroll_y(tk.list);
	list_fill();
	lv_obj_update_layout(tk.list);
	lv_obj_scroll_to_y(tk.list, y, LV_ANIM_OFF);
}

/* Done goes last in the file, so the done ones are listed from its end */
static void row_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	struct task t = tasks[i];
	if (t.done) {
		t.done = 0;
		tasks[i] = t;
	} else {
		t.done = time(NULL);
		memmove(&tasks[i], &tasks[i + 1], (task_count - i - 1) * sizeof(*tasks));
		tasks[task_count - 1] = t;
	}
	ui_vibrate(UI_VIBRATE_TAP);
	tasks_save();
	/* Not while the row's event is being sent: it goes with the list */
	lv_async_call(refill_async, NULL);
}

/* A round box, ticked when done */
static void check_create(lv_obj_t *row, bool done)
{
	lv_obj_t *check = ui_box(row);
	lv_obj_set_size(check, CHECK_SIZE, CHECK_SIZE);
	lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
	if (!done) {
		lv_obj_set_style_border_width(check, 3, 0);
		lv_obj_set_style_border_color(check, lv_color_hex(UI_COLOR_DIM), 0);
		return;
	}
	lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_t *tick = lv_label_create(check);
	lv_label_set_text_static(tick, LV_SYMBOL_OK);
	lv_obj_set_style_text_font(tick, &ui_font_24, 0);
	lv_obj_set_style_text_color(tick, lv_color_hex(0xffffff), 0);
	lv_obj_center(tick);
}

static void row_create(lv_obj_t *card, unsigned i, bool divider)
{
	const struct task *t = &tasks[i];
	lv_obj_t *row = ui_row_create(card, LV_SIZE_CONTENT, divider, row_cb, (void *)(uintptr_t)i);
	/* ROW_H high on one line, taller wrapped (a minimum height would leave it at the top) */
	lv_obj_set_style_pad_ver(row, (ROW_H - CHECK_SIZE) / 2, 0);
	check_create(row, t->done);

	/* Wrapped: all of it is read */
	lv_obj_t *title = lv_label_create(row);
	lv_label_set_text_static(title, t->title);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
	if (t->done)
		lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_flex_grow(title, 1);
}

/* Those to do, oldest first; the same second, as in the file */
static int todo_cmp(const void *a, const void *b)
{
	unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
	if (tasks[x].created != tasks[y].created)
		return tasks[x].created < tasks[y].created ? -1 : 1;
	return (x > y) - (x < y);
}

static void list_fill(void)
{
	lv_obj_clean(tk.list);
	if (!task_count) {
		ui_empty_label(tk.list, _("No tasks"));
		return;
	}

	unsigned *todo = malloc(task_count * sizeof(*todo)), n = 0, done = 0;
	for (unsigned i = 0; i < task_count; i++) {
		if (tasks[i].done)
			done++;
		else
			todo[n++] = i;
	}
	qsort(todo, n, sizeof(*todo), todo_cmp);
	if (n) {
		lv_obj_t *card = ui_card_create(tk.list);
		for (unsigned j = 0; j < n; j++)
			row_create(card, todo[j], j + 1 < n);
	}
	free(todo);

	if (!done)
		return;
	ui_section_label(tk.list, C_("tasks", "Done"));
	lv_obj_t *card = ui_card_create(tk.list);
	for (unsigned i = task_count, j = 0; i-- > 0;)
		if (tasks[i].done)
			row_create(card, i, ++j < done);
}

/* Field */

static void add(void)
{
	const char *text = lv_textarea_get_text(tk.ta);
	while (*text == ' ')
		text++;
	size_t len = strlen(text);
	while (len && text[len - 1] == ' ')
		len--;
	if (len) {
		char *title = strndup(text, len);
		task_append(title, time(NULL), 0);
		free(title);
		tasks_save();
		list_fill();
		/* The new one is the last to do */
		lv_obj_update_layout(tk.list);
		lv_obj_t *card = lv_obj_get_child(tk.list, 0);
		lv_obj_scroll_to_view_recursive(lv_obj_get_child(card, -1), LV_ANIM_ON);
	}
	lv_textarea_set_text(tk.ta, "");
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(tk.kb, true);
		break;
	case LV_EVENT_READY: /* Enter: the next one can be typed at once */
		add();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(tk.kb, false);
		break;
	default:
		break;
	}
}

static void add_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	add();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&tk, 0, sizeof(tk));
}

void tasks_open(void)
{
	tasks_load();
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Tasks"), &header, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	tk.list = ui_box(body);
	lv_obj_set_width(tk.list, LV_PCT(100));
	lv_obj_set_flex_grow(tk.list, 1);
	lv_obj_set_flex_flow(tk.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(tk.list, 16, 0);
	lv_obj_set_style_pad_bottom(tk.list, 16, 0);
	lv_obj_set_scrollable(tk.list, true);
	lv_obj_set_scroll_dir(tk.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(tk.list, LV_SCROLLBAR_MODE_OFF);
	list_fill();

	lv_obj_t *bar = ui_bar_create(body);
	tk.ta = ui_text_field(bar, FIELD_H, MAX_TITLE, ta_cb, NULL);
	lv_textarea_set_placeholder_text(tk.ta, _("New task"));
	lv_obj_set_flex_grow(tk.ta, 1);
	ui_textarea_set_prose(tk.ta);
	lv_obj_t *plus = ui_button(bar, FIELD_H, FIELD_H, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, LV_SYMBOL_PLUS,
	                           add_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), &ui_font_32, 0);

	/* Under the field, shown while typing; the list shrinks above it */
	tk.kb = ui_keyboard_create(body, tk.ta);
	ui_push(scr);
}
