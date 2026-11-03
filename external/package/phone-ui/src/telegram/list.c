/*
 * Telegram's chat list: the main list's chats, newest first, with avatar, name,
 * last message, time and unread count; the next page loads near the end. Rows
 * are reused and only change what changed.
 */

#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define ROW_H 120

struct row {
	lv_obj_t *obj, *avatar, *initial, *photo, *title, *time, *preview, *badge, *count;
	long long chat_id;
	unsigned tex; /* shown by photo */
};

/* When the screen is open */
static struct {
	lv_obj_t *list;
	lv_obj_t *card;
	lv_obj_t *empty;
	struct row *rows;
	unsigned row_count;
} lst;

static const uint32_t avatar_colors[] = {
	0xff6b6b, 0xff9f43, 0xa66cff, 0x30d158, 0x64d2ff, 0x0a84ff, 0xff375f,
};

/* Rows are passed by index: the array moves when it grows */
static void row_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i < lst.row_count)
		chat_open(lst.rows[i].chat_id);
}

static void row_create(struct row *r, unsigned index)
{
	/* The divider is set with the chat (row_update) */
	r->obj = ui_row_create(lst.card, ROW_H, false, row_cb, (void *)(uintptr_t)index);

	r->avatar = lv_obj_create(r->obj);
	lv_obj_remove_style_all(r->avatar);
	lv_obj_set_size(r->avatar, AVATAR, AVATAR);
	lv_obj_set_style_radius(r->avatar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(r->avatar, LV_OPA_COVER, 0);
	lv_obj_set_clickable(r->avatar, false);
	lv_obj_set_scrollable(r->avatar, false);
	r->initial = lv_label_create(r->avatar);
	lv_obj_set_style_text_font(r->initial, &ui_font_32, 0);
	lv_obj_set_style_text_color(r->initial, lv_color_white(), 0);
	lv_obj_center(r->initial);
	/* The photo over the initial, once decoded */
#if LV_USE_3DTEXTURE
	r->photo = lv_3dtexture_create(r->avatar);
	lv_obj_set_size(r->photo, AVATAR, AVATAR);
	lv_obj_set_clickable(r->photo, false);
	lv_obj_set_hidden(r->photo, true);
#endif
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
	r->badge = lv_obj_create(bottom);
	lv_obj_remove_style_all(r->badge);
	lv_obj_set_size(r->badge, LV_SIZE_CONTENT, 36);
	lv_obj_set_style_min_width(r->badge, 36, 0);
	lv_obj_set_style_pad_hor(r->badge, 10, 0);
	lv_obj_set_style_radius(r->badge, 18, 0);
	lv_obj_set_style_bg_opa(r->badge, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(r->badge, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_clickable(r->badge, false);
	lv_obj_set_scrollable(r->badge, false);
	r->count = lv_label_create(r->badge);
	lv_obj_set_style_text_font(r->count, &ui_font_20, 0);
	lv_obj_set_style_text_color(r->count, lv_color_white(), 0);
	lv_obj_center(r->count);

	r->chat_id = 0;
}

/* Copies text into the label if it changed (titles and messages: any text) */
static void label_update(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void row_update(struct row *r, const struct chat *c, bool divider)
{
	if (r->chat_id != c->id) {
		r->chat_id = c->id;
		unsigned long long id = c->id < 0 ? -c->id : c->id;
		lv_obj_set_style_bg_color(r->avatar, lv_color_hex(avatar_colors[id % 7]), 0);
	}
#if LV_USE_3DTEXTURE
	if (r->tex != c->avatar_tex) {
		r->tex = c->avatar_tex;
		lv_obj_set_hidden(r->photo, !r->tex);
		/* Behind the photo's soft edge: no colored fringe */
		lv_obj_set_hidden(r->initial, r->tex);
		lv_obj_set_style_bg_opa(r->avatar, r->tex ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
		if (r->tex)
			lv_3dtexture_set_src(r->photo, r->tex);
	}
#endif

	/* First character of the title, upper case if ASCII */
	char initial[5] = "";
	size_t n = 1;
	while (c->title[n] && ((unsigned char)c->title[n] & 0xc0) == 0x80 && n < 4)
		n++;
	if (c->title[0]) {
		memcpy(initial, c->title, n);
		initial[n] = '\0';
		if (initial[0] >= 'a' && initial[0] <= 'z')
			initial[0] -= 'a' - 'A';
	}
	ui_set_text(r->initial, initial);

	label_update(r->title, c->title);
	label_update(r->preview, c->preview);
	char when[32] = "";
	if (c->date)
		ui_when_format(when, sizeof(when), c->date);
	ui_set_text(r->time, when);

	lv_obj_set_hidden(r->badge, !c->unread);
	if (c->unread)
		ui_set_text_fmt(r->count, "%d", c->unread);
	ui_row_set_divider(r->obj, divider);
}

static int chat_cmp(const void *a, const void *b)
{
	const struct chat *x = *(const struct chat **)a, *y = *(const struct chat **)b;
	return (x->order < y->order) - (x->order > y->order);
}

static void list_refresh(void)
{
	tg.chats_changed = false;

	struct chat **sorted = malloc((tg.chat_count + 1) * sizeof(*sorted));
	if (!sorted)
		return;
	unsigned count = 0;
	for (unsigned i = 0; i < tg.chat_count; i++)
		if (tg.chats[i].order)
			sorted[count++] = &tg.chats[i];
	qsort(sorted, count, sizeof(*sorted), chat_cmp);

	if (count > lst.row_count) {
		struct row *rows = realloc(lst.rows, count * sizeof(*rows));
		if (!rows) {
			free(sorted);
			return;
		}
		lst.rows = rows;
		for (; lst.row_count < count; lst.row_count++)
			row_create(&lst.rows[lst.row_count], lst.row_count);
	}
	for (unsigned i = 0; i < lst.row_count; i++) {
		lv_obj_set_hidden(lst.rows[i].obj, i >= count);
		if (i < count)
			row_update(&lst.rows[i], sorted[i], i + 1 < count);
	}
	free(sorted);

	lv_obj_set_hidden(lst.card, !count);
	lv_obj_set_hidden(lst.empty, count);
	lv_label_set_text_static(lst.empty, tg.all_loaded ? "No chats" : "Loading chats...");
}

void list_show(bool shown)
{
	if (!lst.list)
		return;
	lv_obj_set_hidden(lst.list, !shown);
	if (shown)
		list_refresh();
}

void list_poll(void)
{
	if (tg.chats_changed && lst.list && !lv_obj_is_hidden(lst.list))
		list_refresh();
}

/* Near the end of the list: the next page */
static void scroll_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (lv_obj_get_scroll_bottom(lst.list) < 4 * ROW_H)
		load_chats();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(lst.rows);
	memset(&lst, 0, sizeof(lst));
}

void list_create(lv_obj_t *body)
{
	lst.list = lv_obj_create(body);
	lv_obj_remove_style_all(lst.list);
	lv_obj_set_width(lst.list, LV_PCT(100));
	lv_obj_set_flex_grow(lst.list, 1);
	lv_obj_set_flex_flow(lst.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(lst.list, 16, 0);
	lv_obj_set_style_pad_bottom(lst.list, 16, 0);
	lv_obj_set_scroll_dir(lst.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lst.list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(lst.list, scroll_cb, LV_EVENT_SCROLL_END, NULL);
	lv_obj_add_event_cb(lst.list, delete_cb, LV_EVENT_DELETE, NULL);

	lst.empty = ui_empty_label(lst.list, "");

	lst.card = ui_card_create(lst.list);
}
