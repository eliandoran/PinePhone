/*
 * Tasks: a list of things to do, typed into the field at the bottom (Enter adds one and
 * leaves the keyboard up for the next). The circle before a task ticks it off or back;
 * the task itself opens its screen: its title, a due date (and then a time) and delete.
 * Those to do come first, the dated ones by when they are due, then the others in the
 * order added; then those done, the latest first. A due date passed shows in red.
 * Saved in /var/lib/tasks/tasks.json: {"tasks": [{"title": ..., "created": time,
 * "done": time or 0, "due": "YYYY-MM-DD", "time": "HH:MM"}, ...]} ("due" and "time"
 * only when set: local, so that a time zone change keeps 09:00 at 09:00), in the order
 * they were added or last marked done.
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

#define APP "Tasks" /* NON-NLS: its name in ui_apps */
#define TASKS "/var/lib/tasks"
#define TASKS_FILE TASKS "/tasks.json"
#define MAX_FILE (256 * 1024)
#define MAX_TITLE 200 /* characters typed */
#define ROW_H 112
#define CHECK_SIZE 44
#define FIELD_H 80
#define PILL_H 80
#define STEP 15 /* minutes, the time's - and + */
#define TIME_DEFAULT (12 * 60)
#define CHIP_H 56
#define CHIPS 4 /* No date, Today, Tomorrow, Pick… (or the day picked) */
#define LINGER_MS 3000 /* a task ticked off or back stays in place, for a mistake to be seen */

struct task {
	char *title;
	long long created, done; /* times; done 0: still to do */
	char due[11];            /* "YYYY-MM-DD", "" if none */
	char time[6];            /* "HH:MM" on that day, "" if none */
	unsigned id;             /* its reminder's key, its notification's */
	bool reminded;           /* notified of its time (or set to a time gone): not again */
};

/* Pointers: a task's screen keeps its own while the list changes */
static struct task **tasks;
static unsigned task_count, task_size;
static bool tasks_loaded;
static unsigned last_id;
/* Bumped on every change: the list fills again when it comes back */
static unsigned tasks_version;

static struct {
	lv_obj_t *list, *ta, *kb;
	lv_obj_t *done_card, *done_count; /* the row to the Done screen, NULL if none */
	lv_obj_t *chips, *chip[CHIPS]; /* while typing: the due date of the tasks added */
	long due;                      /* that day, -1: none */
	lv_timer_t *timer, *linger; /* the minute; a task just ticked off or back */
	unsigned version;
	int minute; /* of the day, when last filled: what is late changes with it */
} tk;

/* The Done screen, while open */
static struct {
	lv_obj_t *scr, *list, *confirm;
	lv_timer_t *linger;
	unsigned version;
} dn;

/* Dates */

/* Its day number, false if it has no due date */
static bool due_day(const struct task *t, long *day)
{
	int y, m, d;
	if (!t->due[0] || sscanf(t->due, "%4d-%2d-%2d", &y, &m, &d) != 3)
		return false;
	*day = ui_days_from_civil(y, m - 1, d);
	return true;
}

static int minute_now(void)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return tm.tm_hour * 60 + tm.tm_min;
}

static int time_minutes(const char *time)
{
	int h, m;
	return sscanf(time, "%2d:%2d", &h, &m) == 2 ? h * 60 + m : -1;
}

/* Late: not done, its day gone, or today and its time gone */
static bool overdue(const struct task *t)
{
	long day, today = ui_today();
	if (t->done || !due_day(t, &day))
		return false;
	return day < today || (day == today && t->time[0] && time_minutes(t->time) <= minute_now());
}

/* "Today", "Tomorrow 18:00", "Friday" (this week), "Fri 27 Mar", with the year when another */
static void due_format(const struct task *t, char *buf, size_t size)
{
	long day;
	if (!due_day(t, &day)) {
		snprintf(buf, size, "%s", _("None"));
		return;
	}
	struct tm tm, now;
	time_t n = time(NULL);
	localtime_r(&n, &now);
	ui_civil_from_days(day, &tm);
	long diff = day - ui_today();
	int len;
	if (diff == 0)
		len = snprintf(buf, size, "%s", _("Today"));
	else if (diff == 1)
		len = snprintf(buf, size, "%s", _("Tomorrow"));
	else if (diff == -1)
		len = snprintf(buf, size, "%s", _("Yesterday"));
	else if (diff > 1 && diff < 7)
		len = snprintf(buf, size, "%s", ui_day_name(tm.tm_wday, false));
	else if (tm.tm_year == now.tm_year)
		len = snprintf(buf, size, "%s %d %s", ui_day_name(tm.tm_wday, true), tm.tm_mday,
		               ui_month_name(tm.tm_mon, true));
	else
		len = snprintf(buf, size, "%s %d %s %d", ui_day_name(tm.tm_wday, true), tm.tm_mday,
		               ui_month_name(tm.tm_mon, true), tm.tm_year + 1900);
	if (t->time[0] && len > 0 && (size_t)len < size)
		snprintf(buf + len, size - len, " %s", t->time);
}

static void due_set(struct task *t, long day)
{
	struct tm tm;
	ui_civil_from_days(day, &tm);
	/* Bounded, for the compiler: it fits */
	snprintf(t->due, sizeof(t->due), "%04u-%02u-%02u", (unsigned)(tm.tm_year + 1900) % 10000,
	         (unsigned)(tm.tm_mon + 1) % 100, (unsigned)tm.tm_mday % 100);
}

/* File */

static struct task *task_append(const char *title, long long created, long long done)
{
	if (task_count == task_size) {
		task_size = task_size ? task_size * 2 : 16;
		tasks = realloc(tasks, task_size * sizeof(*tasks));
	}
	struct task *t = calloc(1, sizeof(*t));
	t->title = strdup(title);
	t->created = created;
	t->done = done;
	t->id = ++last_id;
	tasks[task_count++] = t;
	return t;
}

static int task_index(const struct task *t)
{
	for (unsigned i = 0; i < task_count; i++)
		if (tasks[i] == t)
			return (int)i;
	return -1;
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
	if (!list || list->kind != JSON_ARRAY) {
		printf("phone-ui: tasks: " TASKS_FILE " has no list of tasks, ignored\n");
	} else {
		for (struct json *v = list->child; v; v = v->next) {
			const char *title = json_str(json_get(v, "title"), "");
			if (!*title)
				continue;
			struct task *t = task_append(title, json_int(json_get(v, "created"), 0),
			                             json_int(json_get(v, "done"), 0));
			long day;
			snprintf(t->due, sizeof(t->due), "%s", json_str(json_get(v, "due"), ""));
			if (!due_day(t, &day))
				t->due[0] = '\0';
			snprintf(t->time, sizeof(t->time), "%s", json_str(json_get(v, "time"), ""));
			if (!t->due[0] || time_minutes(t->time) < 0)
				t->time[0] = '\0';
			t->id = (unsigned)json_int(json_get(v, "id"), 0);
			t->reminded = json_bool(json_get(v, "reminded"));
		}
		/* Ids for those without (an earlier version's), after the others' */
		last_id = 0;
		for (unsigned i = 0; i < task_count; i++)
			if (tasks[i]->id > last_id)
				last_id = tasks[i]->id;
		for (unsigned i = 0; i < task_count; i++)
			if (!tasks[i]->id)
				tasks[i]->id = ++last_id;
	}
	json_free(root);
}

/* Through a temporary file, synced: the battery can cut power at any time */
static bool tasks_save(void)
{
	tasks_version++;
	mkdir(TASKS, 0755);
	FILE *f = fopen(TASKS_FILE ".new", "w");
	if (!f) {
		perror("phone-ui: tasks");
		return false;
	}
	fputs("{\"tasks\": [", f);
	for (unsigned i = 0; i < task_count; i++) {
		const struct task *t = tasks[i];
		size_t size = 6 * strlen(t->title) + 1;
		char *title = malloc(size);
		json_escape(title, size, t->title);
		fprintf(f, "%s\n\t{\"id\": %u, \"title\": \"%s\", \"created\": %lld, \"done\": %lld", i ? "," : "",
		        t->id, title, t->created, t->done);
		free(title);
		if (t->due[0])
			fprintf(f, ", \"due\": \"%s\"", t->due);
		if (t->time[0])
			fprintf(f, ", \"time\": \"%s\"", t->time);
		if (t->reminded)
			fputs(", \"reminded\": true", f);
		fputs("}", f);
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

static void reminder_cancel(const struct task *t);

static void task_delete(struct task *t)
{
	int i = task_index(t);
	if (i < 0)
		return;
	reminder_cancel(t);
	memmove(&tasks[i], &tasks[i + 1], (task_count - i - 1) * sizeof(*tasks));
	task_count--;
	free(t->title);
	free(t);
	tasks_save();
}

/* Reminders: at a task's date and time, a notification (alarms.c, notify.c) */

static void reminder_cancel(const struct task *t)
{
	char key[16];
	snprintf(key, sizeof(key), "%u", t->id);
	ui_alarm_cancel(APP, key);
	ui_notify(APP, key, NULL, NULL, 0, 0, false, NULL);
}

/* Its date and time, false if it has none */
static bool task_when(const struct task *t, struct tm *tm)
{
	int y, mo, d, h, mi;
	if (!t->time[0] || sscanf(t->due, "%4d-%2d-%2d", &y, &mo, &d) != 3 || sscanf(t->time, "%2d:%2d", &h, &mi) != 2)
		return false;
	*tm = (struct tm){ .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi,
	                   .tm_isdst = -1 };
	return true;
}

static struct task *task_by_id(unsigned id)
{
	for (unsigned i = 0; i < task_count; i++)
		if (tasks[i]->id == id)
			return tasks[i];
	return NULL;
}

static void editor_open(struct task *t);

/* Tapped: the task; seen, the notification goes */
static void reminder_open(const char *key)
{
	struct task *t = task_by_id((unsigned)strtoul(key, NULL, 10));
	if (!t)
		return;
	ui_notify(APP, key, NULL, NULL, 0, 0, false, NULL);
	/* Over Tasks' list, opened unless it is already */
	if (!tk.list)
		tasks_open();
	editor_open(t);
}

static void reminder_fire(const char *key, void *data)
{
	LV_UNUSED(data);
	struct task *t = task_by_id((unsigned)strtoul(key, NULL, 10));
	struct tm tm;
	if (!t || t->done || !task_when(t, &tm))
		return;
	t->reminded = true;
	tasks_save();
	char when[64];
	due_format(t, when, sizeof(when));
	printf("phone-ui: tasks: reminder of task %u, due %s %s\n", t->id, t->due, t->time);
	ui_notify(APP, key, t->title, when, 1, mktime(&tm), true, reminder_open);
}

/* Its reminder set, unless done or already given */
static void reminder_set(const struct task *t)
{
	struct tm tm;
	reminder_cancel(t);
	if (t->done || t->reminded || !task_when(t, &tm))
		return;
	char key[16];
	snprintf(key, sizeof(key), "%u", t->id);
	ui_alarm_at(APP, key, &tm, reminder_fire, NULL);
}

/* Its date or time changed: reminded again, unless that time is gone already */
static void reminder_reset(struct task *t)
{
	struct tm tm;
	t->reminded = task_when(t, &tm) && mktime(&tm) <= time(NULL);
	reminder_set(t);
}

void tasks_start(void)
{
	tasks_load();
	for (unsigned i = 0; i < task_count; i++)
		reminder_set(tasks[i]);
}

/* A row of label and value on a card, the value dim, an arrow after it */
static lv_obj_t *value_row(lv_obj_t *card, const char *label, lv_event_cb_t cb, void *user_data,
                           lv_obj_t **value)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, false, cb, user_data);
	lv_obj_t *name = ui_one_line_label(row, label, UI_FONT_TITLE);
	lv_obj_set_flex_grow(name, 1);
	*value = ui_dim_label(row, "", UI_FONT_BODY);
	ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
	return row;
}

/* Pills of choices in a row that wraps */
static lv_obj_t *pills_create(lv_obj_t *parent)
{
	lv_obj_t *pills = ui_box(parent);
	lv_obj_set_size(pills, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_gap(pills, 12, 0);
	return pills;
}

static lv_obj_t *pill(lv_obj_t *pills, const char *text, lv_event_cb_t cb, void *user_data)
{
	lv_obj_t *b = ui_button(pills, LV_SIZE_CONTENT, PILL_H, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, text, cb,
	                        user_data);
	lv_obj_set_style_pad_hor(b, 28, 0);
	lv_obj_set_flex_grow(b, 1);
	return b;
}

/* A screen's body padded, scrolling up and down */
static void body_style(lv_obj_t *body)
{
	lv_obj_set_style_pad_hor(body, 16, 0);
	lv_obj_set_style_pad_row(body, 16, 0);
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
}

/* A task's screen */

struct editor {
	struct task *t; /* NULL once deleted */
	lv_obj_t *ta, *kb, *confirm, *due, *time_row, *time;
	int minutes;            /* the time's screen, while open */
	lv_obj_t *minutes_label;
};

static void editor_refresh(struct editor *ed)
{
	char buf[64];
	due_format(ed->t, buf, sizeof(buf));
	/* The date alone here, its time on the next row */
	char *space = ed->t->time[0] ? strrchr(buf, ' ') : NULL;
	if (space)
		*space = '\0';
	lv_label_set_text(ed->due, buf);
	lv_obj_set_hidden(ed->time_row, !ed->t->due[0]);
	ui_row_set_divider(lv_obj_get_parent(ed->due), ed->t->due[0]);
	lv_label_set_text(ed->time, ed->t->time[0] ? ed->t->time : _("None"));
}

static void editor_save(struct editor *ed)
{
	if (!ed->t)
		return;
	const char *text = lv_textarea_get_text(ed->ta);
	while (*text == ' ' || *text == '\n')
		text++;
	size_t len = strlen(text);
	while (len && (text[len - 1] == ' ' || text[len - 1] == '\n'))
		len--;
	/* Emptied: the title it had */
	if (!len || (strlen(ed->t->title) == len && strncmp(ed->t->title, text, len) == 0))
		return;
	free(ed->t->title);
	ed->t->title = strndup(text, len);
	tasks_save();
}

/* Due date's screen */

static void due_pick(struct editor *ed, bool set, long day)
{
	if (set) {
		due_set(ed->t, day);
	} else {
		ed->t->due[0] = '\0';
		ed->t->time[0] = '\0';
	}
	reminder_reset(ed->t);
	tasks_save();
	editor_refresh(ed);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

enum { PICK_TODAY, PICK_TOMORROW, PICK_NEXT_WEEK, PICK_NONE };

static void due_pill_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	int pick = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
	long today = ui_today();
	switch (pick) {
	case PICK_TODAY:
		due_pick(ed, true, today);
		break;
	case PICK_TOMORROW:
		due_pick(ed, true, today + 1);
		break;
	case PICK_NEXT_WEEK: /* its Monday */
		due_pick(ed, true, today + 7 - ui_weekday(today));
		break;
	default:
		due_pick(ed, false, 0);
		break;
	}
}

static void due_day_cb(long day, void *user_data)
{
	due_pick(user_data, true, day);
}

static void due_pill(lv_obj_t *pills, const char *text, int pick, struct editor *ed)
{
	lv_obj_set_user_data(pill(pills, text, due_pill_cb, ed), (void *)(intptr_t)pick);
}

static void due_open(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Due date"), NULL, &body);
	body_style(body);

	lv_obj_t *pills = pills_create(body);
	due_pill(pills, _("Today"), PICK_TODAY, ed);
	due_pill(pills, _("Tomorrow"), PICK_TOMORROW, ed);
	due_pill(pills, _("Next week"), PICK_NEXT_WEEK, ed);
	if (ed->t->due[0])
		due_pill(pills, _("None"), PICK_NONE, ed);

	long day;
	if (!due_day(ed->t, &day))
		day = -1;
	ui_month_create(body, day, due_day_cb, ed);
	ui_push(scr);
}

/* Time's screen */

static void time_pick(struct editor *ed, int minutes)
{
	if (minutes < 0)
		ed->t->time[0] = '\0';
	else
		snprintf(ed->t->time, sizeof(ed->t->time), "%02d:%02d", minutes / 60 % 24, minutes % 60);
	reminder_reset(ed->t);
	tasks_save();
	editor_refresh(ed);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

static void time_pill_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	time_pick(ed, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void time_pill(lv_obj_t *pills, const char *text, int minutes, struct editor *ed)
{
	lv_obj_set_user_data(pill(pills, text, time_pill_cb, ed), (void *)(intptr_t)minutes);
}

static void step_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	int step = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
	/* To the step before or after, then by steps; around midnight */
	int m = ed->minutes + (step > 0 ? STEP - ed->minutes % STEP : -(ed->minutes % STEP ? ed->minutes % STEP : STEP));
	ed->minutes = (m + 24 * 60) % (24 * 60);
	ui_set_text_fmt(ed->minutes_label, "%02d:%02d", ed->minutes / 60, ed->minutes % 60);
	if (lv_event_get_code(e) == LV_EVENT_CLICKED)
		ui_vibrate(UI_VIBRATE_TAP);
}

static void set_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	time_pick(ed, ed->minutes);
}

static lv_obj_t *step_button(lv_obj_t *parent, const char *symbol, int step, struct editor *ed)
{
	lv_obj_t *b = ui_round_button(parent, 112, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, symbol, step_cb, ed);
	lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, ed);
	lv_obj_set_user_data(b, (void *)(intptr_t)step);
	return b;
}

static void time_open(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Time"), NULL, &body);
	body_style(body);

	lv_obj_t *pills = pills_create(body);
	time_pill(pills, "09:00", 9 * 60, ed);  /* NON-NLS */
	time_pill(pills, "13:00", 13 * 60, ed); /* NON-NLS */
	time_pill(pills, "18:00", 18 * 60, ed); /* NON-NLS */
	if (ed->t->time[0])
		time_pill(pills, _("None"), -1, ed);

	/* Or any, by steps */
	lv_obj_t *stepper = ui_box(body);
	lv_obj_set_size(stepper, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_ver(stepper, 32, 0);
	step_button(stepper, LV_SYMBOL_MINUS, -1, ed);
	ed->minutes = ed->t->time[0] ? time_minutes(ed->t->time) : TIME_DEFAULT;
	ed->minutes_label = lv_label_create(stepper);
	lv_obj_set_style_text_font(ed->minutes_label, UI_FONT_DISPLAY, 0);
	ui_set_text_fmt(ed->minutes_label, "%02d:%02d", ed->minutes / 60, ed->minutes % 60);
	step_button(stepper, LV_SYMBOL_PLUS, 1, ed);

	lv_obj_t *set = ui_button(body, LV_PCT(100), 96, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Set"), set_cb, ed);
	lv_obj_set_style_text_font(lv_obj_get_child(set, 0), UI_FONT_TITLE, 0);
	ui_push(scr);
}

/* The task's screen itself */

static void ta_editor_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		lv_obj_set_hidden(ed->confirm, true);
		ui_keyboard_show(ed->kb, true);
		break;
	case LV_EVENT_READY: /* Enter: a title is one paragraph */
	case LV_EVENT_CANCEL:
		ui_keyboard_show(ed->kb, false);
		editor_save(ed);
		break;
	default:
		break;
	}
}

static void trash_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	ui_keyboard_show(ed->kb, false);
	lv_obj_set_hidden(ed->confirm, !lv_obj_is_hidden(ed->confirm));
}

static void cancel_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	lv_obj_set_hidden(ed->confirm, true);
}

static void delete_task_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	task_delete(ed->t);
	ed->t = NULL;
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

/* Saved as the screen goes, before the list's load start */
static void editor_unload_cb(lv_event_t *e)
{
	editor_save(lv_event_get_user_data(e));
}

static void editor_delete_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	editor_save(ed);
	free(ed);
}

static void editor_open(struct task *t)
{
	struct editor *ed = calloc(1, sizeof(*ed));
	ed->t = t;

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Task"), &header, &body);
	lv_obj_add_event_cb(scr, editor_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, ed);
	lv_obj_add_event_cb(scr, editor_delete_cb, LV_EVENT_DELETE, ed);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, ed);
	lv_obj_set_scrollable(body, false);

	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(content, true);
	body_style(content);

	/* The title, wrapped */
	ed->ta = ui_textarea_create(content, ta_editor_cb, ed);
	lv_textarea_set_max_length(ed->ta, MAX_TITLE);
	lv_textarea_set_text(ed->ta, t->title);
	lv_obj_set_size(ed->ta, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_text_font(ed->ta, UI_FONT_TITLE, 0);
	lv_obj_set_style_pad_all(ed->ta, 28, 0);
	lv_obj_set_style_radius(ed->ta, 24, 0);
	lv_obj_set_style_bg_opa(ed->ta, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ed->ta, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_scrollbar_mode(ed->ta, LV_SCROLLBAR_MODE_OFF);
	ui_textarea_set_prose(ed->ta);
	ui_textarea_set_enter_sends(ed->ta);

	lv_obj_t *card = ui_card_create(content);
	value_row(card, _("Due"), due_open, ed, &ed->due);
	ed->time_row = value_row(card, _("Time"), time_open, ed, &ed->time);
	editor_refresh(ed);

	ed->confirm = ui_confirm_bar(body, _("Delete task"), delete_task_cb, cancel_cb, ed);
	ed->kb = ui_keyboard_create(body, ed->ta);
	ui_push(scr);
}

/* List */

static void list_fill(void);
static void done_fill(void);
static void done_open(lv_event_t *e);

/* Filled again where it was scrolled to */
static void refill(lv_obj_t *list, void (*fill)(void))
{
	int32_t y = lv_obj_get_scroll_y(list);
	fill();
	lv_obj_update_layout(list);
	lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
}

/* The list, if still open */
static void refill_async(void *data)
{
	LV_UNUSED(data);
	if (tk.list)
		refill(tk.list, list_fill);
}

static void linger_cb(lv_timer_t *timer)
{
	bool done_screen = timer == dn.linger;
	lv_timer_delete(timer);
	if (done_screen) {
		dn.linger = NULL;
		refill(dn.list, done_fill);
	} else {
		tk.linger = NULL;
		refill(tk.list, list_fill);
	}
}

/* The pause before the list changes, from the last tick */
static void linger(lv_timer_t **timer)
{
	if (*timer)
		lv_timer_reset(*timer);
	else
		*timer = lv_timer_create(linger_cb, LINGER_MS, NULL);
}

static void check_style(lv_obj_t *check, bool done);
static void done_row_update(void);

/* Done goes last in the file, so the done ones are listed from its end */
static void check_cb(lv_event_t *e)
{
	struct task *t = lv_event_get_user_data(e);
	int i = task_index(t);
	if (i < 0)
		return;
	if (t->done) {
		t->done = 0;
	} else {
		t->done = time(NULL);
		memmove(&tasks[i], &tasks[i + 1], (task_count - i - 1) * sizeof(*tasks));
		tasks[task_count - 1] = t;
	}
	reminder_set(t);
	ui_vibrate(UI_VIBRATE_TAP);
	tasks_save();
	/* Shown so in place; the list changes after a moment */
	lv_obj_t *check = lv_event_get_target_obj(e);
	check_style(check, t->done);
	lv_obj_t *title = lv_obj_get_child(lv_obj_get_child(lv_obj_get_parent(check), 1), 0);
	lv_obj_set_style_text_color(title, lv_color_hex(t->done ? UI_COLOR_DIM : UI_COLOR_TEXT), 0);
	if (lv_obj_get_screen(check) == dn.scr) {
		dn.version = tasks_version;
		linger(&dn.linger);
	} else {
		tk.version = tasks_version;
		linger(&tk.linger);
		done_row_update();
	}
}

static void row_cb(lv_event_t *e)
{
	editor_open(lv_event_get_user_data(e));
}

/* A round box, ticked when done; it takes taps around it too */
static void check_create(lv_obj_t *row, struct task *t)
{
	lv_obj_t *check = ui_box(row);
	lv_obj_set_size(check, CHECK_SIZE, CHECK_SIZE);
	lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_clickable(check, true);
	lv_obj_set_ext_click_area(check, 28);
	lv_obj_set_user_data(check, t);
	lv_obj_add_event_cb(check, check_cb, LV_EVENT_CLICKED, t);
	check_style(check, t->done);
}

static void check_style(lv_obj_t *check, bool done)
{
	lv_obj_clean(check);
	lv_obj_set_style_border_width(check, done ? 0 : 3, 0);
	lv_obj_set_style_bg_opa(check, done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (!done) {
		lv_obj_set_style_border_color(check, lv_color_hex(UI_COLOR_DIM), 0);
		return;
	}
	lv_obj_set_style_bg_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_t *tick = lv_label_create(check);
	lv_label_set_text_static(tick, LV_SYMBOL_OK);
	lv_obj_set_style_text_font(tick, &ui_font_24, 0);
	lv_obj_set_style_text_color(tick, lv_color_hex(0xffffff), 0);
	lv_obj_center(tick);
}

/* Sections of the tasks to do, by when they are due */
enum section { OVERDUE, TODAY, TOMORROW, LATER, UNDATED };
static const char *const section_names[] = { N_("Overdue"), N_("Today"), N_("Tomorrow"), N_("Later"), N_("No date") };

static enum section section_of(const struct task *t)
{
	long day;
	if (!due_day(t, &day))
		return UNDATED;
	if (overdue(t))
		return OVERDUE;
	long diff = day - ui_today();
	return diff == 0 ? TODAY : diff == 1 ? TOMORROW : LATER;
}

/* The line under a title: the due date; in Today and Tomorrow, whose heading says the
 * day, its time alone; on the Done screen, when it was done */
enum line { LINE_DUE, LINE_TIME, LINE_DONE };

static void row_create(lv_obj_t *card, struct task *t, bool divider, enum line line)
{
	lv_obj_t *row = ui_row_create(card, LV_SIZE_CONTENT, divider, row_cb, t);
	/* ROW_H high on one line, taller wrapped (a minimum height would leave it at the top) */
	lv_obj_set_style_pad_ver(row, (ROW_H - CHECK_SIZE) / 2, 0);
	check_create(row, t);

	lv_obj_t *text = ui_row_text(row);
	/* Wrapped: all of it is read. A copy: its task's screen may change it */
	lv_obj_t *title = lv_label_create(text);
	lv_label_set_text(title, t->title);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(title, LV_PCT(100));
	lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
	if (t->done)
		lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_DIM), 0);

	char buf[64];
	buf[0] = '\0';
	if (line == LINE_DONE)
		ui_when_format(buf, sizeof(buf), (time_t)t->done);
	else if (line == LINE_TIME)
		snprintf(buf, sizeof(buf), "%s", t->time);
	else if (t->due[0])
		due_format(t, buf, sizeof(buf));
	if (buf[0]) {
		lv_obj_t *due = ui_dim_label(text, "", UI_FONT_CAPTION);
		lv_label_set_text(due, buf);
		if (overdue(t))
			lv_obj_set_style_text_color(due, lv_color_hex(UI_COLOR_RED), 0);
	}
}

/* Those to do: by section, dated ones by when they are due (a day without a time first),
 * then in the order added */
static int todo_cmp(const void *a, const void *b)
{
	const struct task *x = *(struct task *const *)a, *y = *(struct task *const *)b;
	enum section sx = section_of(x), sy = section_of(y);
	if (sx != sy)
		return sx < sy ? -1 : 1;
	if (!x->due[0] != !y->due[0])
		return x->due[0] ? -1 : 1;
	int c = strcmp(x->due, y->due);
	if (!c)
		c = strcmp(x->time, y->time);
	if (!c && x->created != y->created)
		c = x->created < y->created ? -1 : 1;
	if (!c)
		c = (task_index(x) > task_index(y)) - (task_index(x) < task_index(y));
	return c;
}

static void list_fill(void)
{
	lv_obj_clean(tk.list);
	tk.version = tasks_version;
	tk.minute = minute_now();
	if (!task_count) {
		ui_empty_label(tk.list, _("No tasks"));
		return;
	}

	struct task **todo = malloc(task_count * sizeof(*todo));
	unsigned n = 0;
	for (unsigned i = 0; i < task_count; i++)
		if (!tasks[i]->done)
			todo[n++] = tasks[i];
	qsort(todo, n, sizeof(*todo), todo_cmp);
	/* Headings once some have a date: undated ones last */
	bool headings = n && section_of(todo[0]) != UNDATED;
	lv_obj_t *card = NULL;
	for (unsigned j = 0; j < n; j++) {
		enum section s = section_of(todo[j]);
		if (!card || (headings && s != section_of(todo[j - 1]))) {
			if (headings) {
				lv_obj_t *heading = ui_section_label(tk.list, _(section_names[s]));
				if (s == OVERDUE)
					lv_obj_set_style_text_color(heading, lv_color_hex(UI_COLOR_RED), 0);
			}
			card = ui_card_create(tk.list);
		}
		bool last = j + 1 == n || (headings && section_of(todo[j + 1]) != s);
		row_create(card, todo[j], !last, s == TODAY || s == TOMORROW ? LINE_TIME : LINE_DUE);
	}
	free(todo);
	if (!n)
		ui_empty_label(tk.list, _("Nothing to do"));

	tk.done_card = NULL;
	done_row_update();
}

/* The done ones behind a row at the end, with how many: at once when one is ticked */
static void done_row_update(void)
{
	unsigned done = 0;
	for (unsigned i = 0; i < task_count; i++)
		done += tasks[i]->done != 0;
	if (!done) {
		if (tk.done_card)
			lv_obj_delete(tk.done_card);
		tk.done_card = NULL;
		return;
	}
	if (!tk.done_card) {
		tk.done_card = ui_card_create(tk.list);
		lv_obj_set_style_margin_top(tk.done_card, 16, 0);
		value_row(tk.done_card, C_("tasks", "Done"), done_open, NULL, &tk.done_count);
	}
	ui_set_text_fmt(tk.done_count, "%u", done);
}

/* Back from a task: it may have changed */
static void list_load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (tk.version != tasks_version)
		refill(tk.list, list_fill);
	/* Back from picking a day: typing goes on */
	if (!lv_obj_is_hidden(tk.chips))
		ui_keyboard_show(tk.kb, true);
}

/* What is late, and which day is today, change with the time */
static void timer_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (minute_now() != tk.minute && !tk.linger && lv_screen_active() == lv_obj_get_screen(tk.list))
		refill_async(NULL);
}

/* Done screen: the tasks done, the latest first, with when; ticked back, they are to do */

static void done_fill(void)
{
	lv_obj_clean(dn.list);
	dn.version = tasks_version;
	unsigned done = 0;
	for (unsigned i = 0; i < task_count; i++)
		done += tasks[i]->done != 0;
	if (!done) {
		ui_empty_label(dn.list, _("No done tasks"));
		return;
	}
	/* Done goes last in the file */
	lv_obj_t *card = ui_card_create(dn.list);
	for (unsigned i = task_count, j = 0; i-- > 0;)
		if (tasks[i]->done)
			row_create(card, tasks[i], ++j < done, LINE_DONE);
}

/* Back from a task: it may have changed */
static void done_load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (dn.version != tasks_version)
		refill(dn.list, done_fill);
}

static void done_trash_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(dn.confirm, !lv_obj_is_hidden(dn.confirm));
}

static void done_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_set_hidden(dn.confirm, true);
}

static void done_delete_all_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	unsigned kept = 0;
	for (unsigned i = 0; i < task_count; i++) {
		if (tasks[i]->done) {
			reminder_cancel(tasks[i]);
			free(tasks[i]->title);
			free(tasks[i]);
		} else {
			tasks[kept++] = tasks[i];
		}
	}
	task_count = kept;
	tasks_save();
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

static void done_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (dn.linger)
		lv_timer_delete(dn.linger);
	memset(&dn, 0, sizeof(dn));
}

static void done_open(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *header, *body;
	dn.scr = ui_app_screen_create(C_("tasks", "Done"), &header, &body);
	lv_obj_add_event_cb(dn.scr, done_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(dn.scr, done_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	ui_header_button(header, LV_SYMBOL_TRASH, done_trash_cb, NULL);
	lv_obj_set_scrollable(body, false);

	dn.list = ui_box(body);
	lv_obj_set_width(dn.list, LV_PCT(100));
	lv_obj_set_flex_grow(dn.list, 1);
	lv_obj_set_flex_flow(dn.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(dn.list, true);
	body_style(dn.list);
	lv_obj_set_style_pad_bottom(dn.list, 16, 0);
	done_fill();

	dn.confirm = ui_confirm_bar(body, _("Delete done tasks"), done_delete_all_cb, done_cancel_cb, NULL);
	ui_push(dn.scr);
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
		struct task *t = task_append(title, time(NULL), 0);
		free(title);
		if (tk.due >= 0)
			due_set(t, tk.due);
		tasks_save();
		list_fill();
		/* Where the new one is */
		lv_obj_update_layout(tk.list);
		lv_obj_t *card = lv_obj_get_child(tk.list, 0);
		for (unsigned i = 0; i < lv_obj_get_child_count(card); i++) {
			lv_obj_t *row = lv_obj_get_child(card, i);
			if (lv_obj_get_user_data(lv_obj_get_child(row, 0)) == t)
				lv_obj_scroll_to_view_recursive(row, LV_ANIM_ON);
		}
	}
	lv_textarea_set_text(tk.ta, "");
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(tk.kb, true);
		lv_obj_set_hidden(tk.chips, false);
		break;
	case LV_EVENT_READY: /* Enter: the next one can be typed at once */
		add();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(tk.kb, false);
		lv_obj_set_hidden(tk.chips, true);
		break;
	default:
		break;
	}
}

/* Chips: the due date of the tasks added, until changed or Tasks is left */

/* Highlights the chip of `day`; the last one shows a day picked other than today and tomorrow */
static void chips_select(long day)
{
	long today = ui_today();
	unsigned sel = day < 0 ? 0 : day == today ? 1 : day == today + 1 ? 2 : 3;
	tk.due = day;
	for (unsigned i = 0; i < CHIPS; i++)
		ui_button_set_color(tk.chip[i], i == sel ? UI_COLOR_ACCENT : UI_COLOR_BUTTON,
		                    i == sel ? UI_COLOR_ACCENT_PRESSED : UI_COLOR_BUTTON_PRESSED);
	lv_obj_t *pick = lv_obj_get_child(tk.chip[3], 0);
	if (sel == 3) {
		struct task t = { 0 };
		char buf[64];
		due_set(&t, day);
		due_format(&t, buf, sizeof(buf));
		lv_label_set_text(pick, buf);
	} else {
		lv_label_set_text_static(pick, _("Pick\xe2\x80\xa6"));
	}
}

static void chip_day_cb(long day, void *user_data)
{
	LV_UNUSED(user_data);
	if (tk.chips)
		chips_select(day);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

static void chip_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	long today = ui_today();
	if (i < 3) {
		ui_vibrate(UI_VIBRATE_TAP);
		chips_select(i == 0 ? -1 : today + i - 1);
		return;
	}
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Due date"), NULL, &body);
	body_style(body);
	ui_month_create(body, tk.due, chip_day_cb, NULL);
	ui_push(scr);
}

static void chips_create(lv_obj_t *parent)
{
	static const char *const names[CHIPS] = { N_("No date"), N_("Today"), N_("Tomorrow"), N_("Pick\xe2\x80\xa6") };
	tk.chips = ui_box(parent);
	lv_obj_set_size(tk.chips, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(tk.chips, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_hor(tk.chips, 16, 0);
	lv_obj_set_style_pad_top(tk.chips, 12, 0);
	lv_obj_set_style_pad_column(tk.chips, 12, 0);
	lv_obj_set_style_bg_opa(tk.chips, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tk.chips, lv_color_hex(UI_COLOR_BAR), 0);
	for (unsigned i = 0; i < CHIPS; i++) {
		tk.chip[i] = ui_button(tk.chips, LV_SIZE_CONTENT, CHIP_H, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
		                       _(names[i]), chip_cb, (void *)(uintptr_t)i);
		lv_obj_set_style_pad_hor(tk.chip[i], 20, 0);
		lv_obj_set_flex_grow(tk.chip[i], 1);
		lv_obj_set_ext_click_area(tk.chip[i], 8);
		/* Typing goes on: the field keeps the focus */
		lv_obj_set_click_focusable(tk.chip[i], false);
		lv_obj_set_style_text_font(lv_obj_get_child(tk.chip[i], 0), UI_FONT_BODY, 0);
	}
	lv_obj_set_hidden(tk.chips, true);
	chips_select(-1);
}

static void add_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	add();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (tk.timer)
		lv_timer_delete(tk.timer);
	if (tk.linger)
		lv_timer_delete(tk.linger);
	memset(&tk, 0, sizeof(tk));
}

void tasks_open(void)
{
	tasks_load();
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Tasks"), &header, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
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

	chips_create(body);
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
	tk.timer = lv_timer_create(timer_cb, 1000, NULL);
	ui_push(scr);
}
