/*
 * Telegram's contacts, the bar's second tab: by name, each with their photo and
 * when they were last seen, loaded the first time the tab shows after the app
 * opens. Tapping one opens the chat with them (made if there is none yet). Rows
 * are reused and follow the users' updates (names, statuses, photos).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "telegram.h"

#define ROW_H 120

/* The contacts' user ids: outlive the screen */
static struct {
	long long *ids;
	unsigned count;
	bool loading, loaded;
} contacts;

struct row {
	lv_obj_t *obj, *avatar, *name, *seen;
	long long id;
	unsigned tex;
};

/* The tab, while the screen is open */
static struct {
	lv_obj_t *list, *card, *empty;
	struct row *rows;
	unsigned row_count;
	bool asked; /* getContacts sent since the screen opened */
} view;

static const uint32_t avatar_colors[] = {
	0xff6b6b, 0xff9f43, 0xa66cff, 0x30d158, 0x64d2ff, 0x0a84ff, 0xff375f,
};

void contact_list_clear(void)
{
	free(contacts.ids);
	memset(&contacts, 0, sizeof(contacts));
	view.asked = false;
	tg.users_changed = true;
}

static void load(void)
{
	if (contacts.loading || tg.state != ST_READY)
		return;
	contacts.loading = true;
	view.asked = true;
	td_request("{\"@type\":\"getContacts\",\"@extra\":\"contacts\"}");
}

void contact_list_loaded(const struct json *answer)
{
	contacts.loading = false;
	if (strcmp(json_type(answer), "users")) {
		printf("telegram: getContacts: %s\n", json_str(json_get(answer, "message"), ""));
		tg.users_changed = true;
		return;
	}
	const struct json *ids = json_get(answer, "user_ids");
	unsigned n = 0;
	for (const struct json *i = ids ? ids->child : NULL; i; i = i->next)
		n++;
	long long *list = n ? malloc(n * sizeof(*list)) : NULL;
	if (n && !list)
		return;
	n = 0;
	for (const struct json *i = ids ? ids->child : NULL; i; i = i->next)
		list[n++] = json_int(i, 0);
	free(contacts.ids);
	contacts.ids = list;
	contacts.count = n;
	contacts.loaded = true;
	tg.users_changed = true;
}

/* Opening a contact's chat */

void contact_chat(const struct json *answer)
{
	if (strcmp(json_type(answer), "chat")) {
		printf("telegram: createPrivateChat: %s\n", json_str(json_get(answer, "message"), ""));
		return;
	}
	chat_open(json_int(json_get(answer, "id"), 0));
}

static void row_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i >= view.row_count)
		return;
	long long id = view.rows[i].id;
	/* A private chat's id is the user's */
	if (chat_find(id)) {
		chat_open(id);
		return;
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"@type\":\"createPrivateChat\",\"user_id\":%lld,\"force\":false,"
		"\"@extra\":\"contact chat\"}", id); /* NON-NLS */
	td_request(buf);
}

/* The list */

static const char *name_of(const struct user *u)
{
	return u->name ? u->name : "";
}

static int name_cmp(const void *a, const void *b)
{
	const struct user *x = *(struct user *const *)a, *y = *(struct user *const *)b;
	int c = strcasecmp(name_of(x), name_of(y));
	return c ? c : (x->id > y->id) - (x->id < y->id);
}

static void seen_text(char *buf, size_t size, const struct user *u)
{
	char when[32];
	switch (u->seen) {
	case SEEN_ONLINE:
		snprintf(buf, size, "%s", _("online"));
		break;
	case SEEN_AT:
		ui_when_format(when, sizeof(when), u->was_online);
		snprintf(buf, size, _("last seen %s"), when);
		break;
	case SEEN_RECENTLY:
		snprintf(buf, size, "%s", _("last seen recently"));
		break;
	case SEEN_LAST_WEEK:
		snprintf(buf, size, "%s", _("last seen within a week"));
		break;
	case SEEN_LAST_MONTH:
		snprintf(buf, size, "%s", _("last seen within a month"));
		break;
	default:
		snprintf(buf, size, "%s", _("last seen a long time ago"));
		break;
	}
}

static void row_create(struct row *r, unsigned i)
{
	r->obj = ui_row_create(view.card, ROW_H, false, row_cb, (void *)(uintptr_t)i);
	r->avatar = ui_avatar_create(r->obj, AVATAR);
	lv_obj_set_style_text_font(r->avatar, &ui_font_32, 0);
	lv_obj_t *text = ui_row_text(r->obj);
	r->name = ui_one_line_label(text, "", UI_FONT_TITLE);
	lv_obj_set_width(r->name, LV_PCT(100));
	r->seen = ui_one_line_label(text, "", UI_FONT_CAPTION);
	lv_obj_set_width(r->seen, LV_PCT(100));
	r->id = 0;
	r->tex = 0;
}

static void row_update(struct row *r, struct user *u, bool divider)
{
	const char *name = name_of(u);
	/* The first character, upper case if ASCII */
	char initial[5] = "";
	size_t n = 1;
	while (name[0] && name[n] && ((unsigned char)name[n] & 0xc0) == 0x80 && n < 4)
		n++;
	if (name[0]) {
		memcpy(initial, name, n);
		initial[n] = '\0';
		if (initial[0] >= 'a' && initial[0] <= 'z')
			initial[0] -= 'a' - 'A';
	}
	if (r->id != u->id || strcmp(lv_label_get_text(lv_obj_get_child(r->avatar, 0)), initial)) {
		r->id = u->id;
		ui_avatar_set(r->avatar, avatar_colors[(unsigned long long)u->id % 7], initial);
	}
	user_avatar_want(u);
	if (r->tex != u->avatar_tex) {
		r->tex = u->avatar_tex;
		ui_avatar_set_photo(r->avatar, r->tex);
	}
	if (strcmp(lv_label_get_text(r->name), name))
		lv_label_set_text(r->name, name);

	char seen[96];
	seen_text(seen, sizeof(seen), u);
	ui_set_text(r->seen, seen);
	lv_obj_set_style_text_color(r->seen, lv_color_hex(u->seen == SEEN_ONLINE ? UI_COLOR_ACCENT : UI_COLOR_DIM), 0);
	ui_row_set_divider(r->obj, divider);
}

static void refresh(void)
{
	struct user **sorted = contacts.count ? malloc(contacts.count * sizeof(*sorted)) : NULL;
	unsigned count = 0;
	for (unsigned i = 0; sorted && i < contacts.count; i++) {
		struct user *u = user_find(contacts.ids[i]);
		if (u)
			sorted[count++] = u;
	}
	if (count)
		qsort(sorted, count, sizeof(*sorted), name_cmp);

	if (count > view.row_count) {
		struct row *rows = realloc(view.rows, count * sizeof(*rows));
		if (!rows) {
			free(sorted);
			return;
		}
		view.rows = rows;
		for (; view.row_count < count; view.row_count++)
			row_create(&view.rows[view.row_count], view.row_count);
	}
	for (unsigned i = 0; i < view.row_count; i++) {
		lv_obj_set_hidden(view.rows[i].obj, i >= count);
		if (i < count)
			row_update(&view.rows[i], sorted[i], i + 1 < count);
	}
	free(sorted);

	lv_obj_set_hidden(view.card, !count);
	lv_obj_set_hidden(view.empty, count);
	lv_label_set_text_static(view.empty, contacts.loaded && !contacts.loading ? _("No contacts") :
				 _("Loading contacts..."));
}

void contact_list_show(bool shown)
{
	if (!view.list)
		return;
	lv_obj_set_hidden(view.list, !shown);
	if (!shown)
		return;
	/* Again each time the app opens: TDLib has no update for them */
	if (!view.asked)
		load();
	refresh();
}

void contact_list_poll(void)
{
	if (!tg.users_changed)
		return;
	tg.users_changed = false;
	if (view.list && !lv_obj_is_hidden(view.list))
		refresh();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(view.rows);
	memset(&view, 0, sizeof(view));
}

void contact_list_create(lv_obj_t *body)
{
	view.list = lv_obj_create(body);
	lv_obj_remove_style_all(view.list);
	lv_obj_set_width(view.list, LV_PCT(100));
	lv_obj_set_flex_grow(view.list, 1);
	lv_obj_set_flex_flow(view.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(view.list, 16, 0);
	lv_obj_set_style_pad_bottom(view.list, 16, 0);
	lv_obj_set_scroll_dir(view.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(view.list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(view.list, delete_cb, LV_EVENT_DELETE, NULL);

	view.empty = ui_empty_label(view.list, "");
	view.card = ui_card_create(view.list);
	lv_obj_set_hidden(view.list, true);
}
