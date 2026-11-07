/*
 * Assistant: chats with a language model, see assistant.h. This is the app's
 * screen, the conversations newest first.
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "../json.h"
#include "assistant.h"

#define PREVIEW_MAX 120

struct item {
	char path[PATH_MAX];
	time_t time;   /* of the last message */
	char *title;   /* the first question */
	char *preview; /* the last message */
	char when[24];
};

static struct {
	lv_obj_t *body;
	struct item *items;
	unsigned count;
	unsigned version;
} list;

static void items_free(void)
{
	for (unsigned i = 0; i < list.count; i++) {
		free(list.items[i].title);
		free(list.items[i].preview);
	}
	free(list.items);
	list.items = NULL;
	list.count = 0;
}

static int item_cmp(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	return (y->time > x->time) - (y->time < x->time);
}

static void items_load(void)
{
	items_free();
	DIR *d = opendir(DATA_DIR);
	if (!d)
		return;
	unsigned cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 6 || strcmp(de->d_name + n - 5, ".json"))
			continue;
		if (list.count == cap) {
			cap = cap ? cap * 2 : 16;
			struct item *items = realloc(list.items, cap * sizeof(*items));
			if (!items)
				break;
			list.items = items;
		}
		struct item *it = &list.items[list.count];
		memset(it, 0, sizeof(*it));
		snprintf(it->path, sizeof(it->path), DATA_DIR "/%s", de->d_name);
		char *text = ai_file_read(it->path);
		struct json *root = json_parse(text);
		free(text);
		const struct json *messages = json_get(root, "messages");
		/* The last message, not a search */
		const struct json *first = messages ? messages->child : NULL, *last = first;
		for (const struct json *m = first; m; m = m->next)
			if (strcmp(json_str(json_get(m, "role"), ""), "step"))
				last = m;
		if (first) {
			it->time = json_int(json_get(last, "time"), 0);
			it->title = ui_one_line("", json_str(json_get(first, "content"), ""), PREVIEW_MAX);
			it->preview = ui_one_line(strcmp(json_str(json_get(last, "role"), ""), "user") ? "" : _("You: "),
			                          json_str(json_get(last, "content"), ""), PREVIEW_MAX);
			ui_when_format(it->when, sizeof(it->when), it->time);
			list.count++;
		}
		json_free(root);
	}
	closedir(d);
	if (list.count) /* none: no array */
		qsort(list.items, list.count, sizeof(*list.items), item_cmp);
}

static void row_cb(lv_event_t *e)
{
	const struct item *it = lv_event_get_user_data(e);
	ai_conversation_open(it->path);
}

static void list_fill(void)
{
	lv_obj_clean(list.body);
	items_load();
	list.version = ai_version;
	if (!list.count) {
		ui_empty_label(list.body, _("No chats"));
		return;
	}
	lv_obj_t *card = ui_card_create(list.body);
	for (unsigned i = 0; i < list.count; i++) {
		struct item *it = &list.items[i];
		lv_obj_t *row = ui_row_create(card, ROW_H, i + 1 < list.count, row_cb, it);
		ui_row_lines(row, it->title ? it->title : "", it->when, it->preview ? it->preview : "");
	}
}

/* Back from a conversation: it may have changed */
static void list_load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (list.version != ai_version)
		list_fill();
}

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	items_free();
	memset(&list, 0, sizeof(list));
}

static void settings_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ai_settings_open();
}

static void new_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ai_conversation_open(NULL);
}

void assistant_open(void)
{
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create(_("Assistant"), &header, &list.body);
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(scr, list_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	ui_header_button(header, LV_SYMBOL_SETTINGS, settings_cb, NULL);
	lv_obj_set_style_pad_hor(list.body, 16, 0);
	lv_obj_set_style_pad_top(list.body, 8, 0);
	lv_obj_set_scroll_dir(list.body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list.body, LV_SCROLLBAR_MODE_OFF);
	list_fill();
	ui_action_button(list.body, LV_SYMBOL_EDIT, new_cb, NULL);
	ui_push(scr);
}
