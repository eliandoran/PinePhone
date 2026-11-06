/*
 * Telegram's chat lists: the main list's chats, newest first, with avatar, name,
 * last message, time and unread count; the next page loads near the end. Rows
 * are reused and only change what changed. The main list starts with a row for
 * the archived chats (if any), which opens them as the same list on a screen
 * of its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define ROW_H 120
#define ARCHIVE_COLOR 0x9ea9b4 /* its avatar without GL, and its badge */
#define PIN 28 /* a pinned chat's mark, instead of the badge */

struct row {
	lv_obj_t *obj, *avatar, *title, *time, *preview, *badge, *count;
	lv_obj_t *pin; /* NULL without GL */
	long long chat_id;
	unsigned tex; /* shown by the avatar */
};

/* A list, while its screen is open */
static struct view {
	lv_obj_t *list;
	lv_obj_t *card;
	lv_obj_t *empty;
	struct row *rows;
	unsigned row_count;
	struct row archive; /* main list: the row opening the archive */
} views[LISTS];

static const uint32_t avatar_colors[] = {
	0xff6b6b, 0xff9f43, 0xa66cff, 0x30d158, 0x64d2ff, 0x0a84ff, 0xff375f,
};

/* Rows are passed by index and list: the array moves when it grows */
static void row_cb(lv_event_t *e)
{
	uintptr_t data = (uintptr_t)lv_event_get_user_data(e);
	struct view *v = &views[data % LISTS];
	unsigned i = data / LISTS;
	if (i < v->row_count)
		chat_open(v->rows[i].chat_id);
}

static void archive_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	archive_open();
}

static void row_create(struct view *v, struct row *r, lv_event_cb_t cb, uintptr_t data)
{
	/* The divider is set with the chat (row_update) */
	r->obj = ui_row_create(v->card, ROW_H, false, cb, (void *)data);

	r->avatar = ui_avatar_create(r->obj, AVATAR);
	/* One letter, larger than initials */
	lv_obj_set_style_text_font(r->avatar, &ui_font_32, 0);
	r->tex = 0;

	lv_obj_t *text = ui_row_text(r->obj);

	lv_obj_t *top = ui_row_line(text);
	r->title = ui_one_line_label(top, "", &ui_font_26);
	lv_obj_set_flex_grow(r->title, 1);
	r->time = ui_dim_label(top, "", &ui_font_20);

	lv_obj_t *bottom = ui_row_line(text);
	r->preview = ui_one_line_label(bottom, "", &ui_font_20);
	lv_obj_set_style_text_color(r->preview, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_flex_grow(r->preview, 1);
	r->badge = ui_box(bottom);
	lv_obj_set_size(r->badge, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_min_width(r->badge, 36, 0);
	lv_obj_set_style_pad_hor(r->badge, 10, 0);
	lv_obj_set_style_radius(r->badge, 18, 0);
	lv_obj_set_style_bg_opa(r->badge, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(r->badge, lv_color_hex(UI_COLOR_ACCENT), 0);
	r->count = lv_label_create(r->badge);
	lv_obj_set_style_text_font(r->count, &ui_font_20, 0);
	lv_obj_set_style_text_color(r->count, lv_color_white(), 0);
	lv_obj_center(r->count);
	r->pin = NULL;
#if LV_USE_3DTEXTURE
	r->pin = lv_3dtexture_create(bottom);
	lv_obj_set_size(r->pin, PIN, PIN);
	lv_obj_set_clickable(r->pin, false);
	lv_obj_set_hidden(r->pin, true);
#endif

	r->chat_id = 0;
}

#if LV_USE_3DTEXTURE
/* The pin in the theme's dim color: one texture per theme, kept for the
 * process (rows of both themes may be shown while switching) */
static unsigned pin_tex(void)
{
	static struct {
		uint32_t color;
		unsigned tex;
	} made[2];
	uint32_t color = UI_COLOR_DIM;
	unsigned i = 0;
	while (i < 2 && made[i].tex && made[i].color != color)
		i++;
	if (i == 2)
		i = 1; /* a third color: replaces the last */
	if (!made[i].tex || made[i].color != color) {
		made[i].color = color;
		made[i].tex = pin_texture(PIN, color);
	}
	return made[i].tex;
}
#endif

/* Copies text into the label if it changed (titles and messages: any text) */
static void label_update(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

/* Time, unread count or pin, divider: chats and the archive */
static void row_details(struct row *r, long long date, int unread, bool pinned, bool divider)
{
	char when[32] = "";
	if (date)
		ui_when_format(when, sizeof(when), date);
	ui_set_text(r->time, when);

	lv_obj_set_hidden(r->badge, !unread);
	if (unread)
		ui_set_text_fmt(r->count, "%d", unread);
#if LV_USE_3DTEXTURE
	unsigned tex = pinned && !unread ? pin_tex() : 0;
	lv_obj_set_hidden(r->pin, !tex);
	if (tex)
		lv_3dtexture_set_src(r->pin, tex);
#else
	LV_UNUSED(pinned);
#endif
	ui_row_set_divider(r->obj, divider);
}

static void row_update(struct row *r, const struct chat *c, enum list list, bool divider)
{
	const char *title = chat_title(c);
	unsigned tex = chat_avatar_tex(c);
	/* First character of the title, upper case if ASCII */
	char initial[5] = "";
	size_t n = 1;
	while (title[n] && ((unsigned char)title[n] & 0xc0) == 0x80 && n < 4)
		n++;
	if (title[0]) {
		memcpy(initial, title, n);
		initial[n] = '\0';
		if (initial[0] >= 'a' && initial[0] <= 'z')
			initial[0] -= 'a' - 'A';
	}
	/* Another chat, or its title came later */
	if (r->chat_id != c->id || strcmp(lv_label_get_text(lv_obj_get_child(r->avatar, 0)), initial)) {
		r->chat_id = c->id;
		unsigned long long id = c->id < 0 ? -c->id : c->id;
		ui_avatar_set(r->avatar, avatar_colors[id % 7], initial);
	}
	if (r->tex != tex) {
		r->tex = tex;
		ui_avatar_set_photo(r->avatar, r->tex);
	}

	label_update(r->title, title);
	label_update(r->preview, c->preview);
	row_details(r, c->date, c->unread, c->pinned[list], divider);
}

/* The archive's row: the names of its first chats, its last message's time,
 * how many of its chats (loaded) are unread */
static void archive_update(struct row *r, struct chat **chats, unsigned count, bool divider)
{
	char names[256] = "";
	size_t len = 0;
	int unread = 0;
	for (unsigned i = 0; i < count; i++) {
		if (len + 2 < sizeof(names))
			len += snprintf(names + len, sizeof(names) - len, "%s%s", len ? ", " : "", chat_title(chats[i]));
		unread += chats[i]->unread > 0;
	}
	/* Cut by snprintf, maybe in a character: without the last one */
	if (len >= sizeof(names)) {
		len = sizeof(names) - 1;
		while (len && ((unsigned char)names[len - 1] & 0xc0) == 0x80)
			len--;
		names[len ? len - 1 : 0] = '\0';
	}
	label_update(r->preview, names);
	row_details(r, count ? chats[0]->date : 0, unread, false, divider);
}

/* The chats of a list, in its order */
static enum list sort_list;

static int chat_cmp(const void *a, const void *b)
{
	const struct chat *x = *(const struct chat **)a, *y = *(const struct chat **)b;
	long long xo = x->order[sort_list], yo = y->order[sort_list];
	return (xo < yo) - (xo > yo);
}

static struct chat **sorted_chats(enum list list, unsigned *count)
{
	struct chat **sorted = malloc((tg.chat_count + 1) * sizeof(*sorted));
	*count = 0;
	if (!sorted)
		return NULL;
	for (unsigned i = 0; i < tg.chat_count; i++)
		if (tg.chats[i].order[list])
			sorted[(*count)++] = &tg.chats[i];
	sort_list = list;
	qsort(sorted, *count, sizeof(*sorted), chat_cmp);
	return sorted;
}

static void list_refresh(struct view *v)
{
	enum list list = v - views;
	unsigned count, archived = 0;
	struct chat **sorted = sorted_chats(list, &count);
	struct chat **chats = v->archive.obj ? sorted_chats(LIST_ARCHIVE, &archived) : NULL;
	if (!sorted || (v->archive.obj && !chats)) {
		free(sorted);
		free(chats);
		return;
	}
	/* The main list waits for the archive's first page: its row would come
	 * later, above the chats, and move them */
	bool wait = v->archive.obj && !archived && tg.loading[LIST_ARCHIVE];
	if (wait)
		count = 0;

	if (count > v->row_count) {
		struct row *rows = realloc(v->rows, count * sizeof(*rows));
		if (!rows) {
			free(sorted);
			free(chats);
			return;
		}
		v->rows = rows;
		for (; v->row_count < count; v->row_count++)
			row_create(v, &v->rows[v->row_count], row_cb, v->row_count * LISTS + list);
	}
	for (unsigned i = 0; i < v->row_count; i++) {
		lv_obj_set_hidden(v->rows[i].obj, i >= count);
		if (i < count)
			row_update(&v->rows[i], sorted[i], list, i + 1 < count);
	}
	free(sorted);

	bool archive = archived;
	if (v->archive.obj) {
		lv_obj_set_hidden(v->archive.obj, !archive);
		if (archive)
			archive_update(&v->archive, chats, archived, count);
	}
	free(chats);

	lv_obj_set_hidden(v->card, !count && !archive);
	lv_obj_set_hidden(v->empty, count || archive);
	lv_label_set_text_static(v->empty, tg.all_loaded[list] && !wait ? _("No chats") : _("Loading chats..."));
}

void list_show(bool shown)
{
	struct view *v = &views[LIST_MAIN];
	if (!v->list)
		return;
	lv_obj_set_hidden(v->list, !shown);
	if (shown)
		list_refresh(v);
}

void list_poll(void)
{
	if (!tg.chats_changed)
		return;
	tg.chats_changed = false;
	/* A hidden list is refreshed when shown */
	for (int l = 0; l < LISTS; l++)
		if (views[l].list && !lv_obj_is_hidden(views[l].list))
			list_refresh(&views[l]);
}

/* Near the end of the list: the next page */
static void scroll_cb(lv_event_t *e)
{
	struct view *v = lv_event_get_user_data(e);
	if (lv_obj_get_scroll_bottom(v->list) < 4 * ROW_H)
		load_chats(v - views);
}

static void delete_cb(lv_event_t *e)
{
	struct view *v = lv_event_get_user_data(e);
	free(v->rows);
	memset(v, 0, sizeof(*v));
}

static void view_create(struct view *v, lv_obj_t *body)
{
	v->list = lv_obj_create(body);
	lv_obj_remove_style_all(v->list);
	lv_obj_set_width(v->list, LV_PCT(100));
	lv_obj_set_flex_grow(v->list, 1);
	lv_obj_set_flex_flow(v->list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(v->list, 16, 0);
	lv_obj_set_style_pad_bottom(v->list, 16, 0);
	lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(v->list, scroll_cb, LV_EVENT_SCROLL_END, v);
	lv_obj_add_event_cb(v->list, delete_cb, LV_EVENT_DELETE, v);

	v->empty = ui_empty_label(v->list, "");

	v->card = ui_card_create(v->list);
}

void list_create(lv_obj_t *body)
{
	struct view *v = &views[LIST_MAIN];
	view_create(v, body);

	/* First in the card, hidden while the archive is empty */
	struct row *r = &v->archive;
	row_create(v, r, archive_cb, 0);
	ui_avatar_set(r->avatar, ARCHIVE_COLOR, "A");
	/* Made once, kept for the process (0 without GL) */
	static unsigned tex;
	static bool made;
	if (!made) {
		tex = archive_texture(AVATAR);
		made = true;
	}
	r->tex = tex;
	ui_avatar_set_photo(r->avatar, tex);
	lv_label_set_text_static(r->title, _("Archived chats"));
	/* Muted, as in Telegram */
	lv_obj_set_style_bg_color(r->badge, lv_color_hex(ARCHIVE_COLOR), 0);
	lv_obj_set_hidden(r->obj, true);
}

void archive_open(void)
{
	struct view *v = &views[LIST_ARCHIVE];
	if (v->list)
		return;
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Archived chats"), NULL, &body);
	lv_obj_set_scrollable(body, false);
	view_create(v, body);
	list_refresh(v);
	ui_push(scr);
}
