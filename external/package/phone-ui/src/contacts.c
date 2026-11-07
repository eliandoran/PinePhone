/*
 * Contacts: alphabetical list, a detail screen with Call and Message, and an
 * editor to add, change or delete people. The address book is in data.c
 * (/var/lib/contacts/contacts.json).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "i18n.h"
#include "ui_symbols.h"

#define ROW_H 112
#define FIELD_H 88

#define STAR_ON UI_SYMBOL_STAR
#define STAR_OFF UI_SYMBOL_STAR_REGULAR
#define COLOR_FAVORITE UI_COLOR_ORANGE

/* Section letters as static texts */
static const char letters[27][2] = {
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
	"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "#",
};

/* Editor */

struct editor {
	const struct contact *contact; /* NULL: a new one */
	lv_obj_t *name, *number, *kb;
};

static bool blank(const char *s)
{
	return s[strspn(s, " \t\r\n")] == '\0';
}

static bool has_digit(const char *s)
{
	return strpbrk(s, "0123456789") != NULL;
}

/* Types into `ta` (the other field loses the cursor) */
static void editor_type(struct editor *ed, lv_obj_t *ta)
{
	lv_obj_t *other = ta == ed->name ? ed->number : ed->name;
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	lv_keyboard_set_textarea(ed->kb, ta);
	ui_keyboard_show(ed->kb, true);
}

static void editor_save(struct editor *ed)
{
	const char *name = lv_textarea_get_text(ed->name);
	const char *number = lv_textarea_get_text(ed->number);
	/* Both are needed: show what is missing */
	if (blank(name) || !has_digit(number)) {
		ui_vibrate(UI_VIBRATE_TAP);
		editor_type(ed, blank(name) ? ed->name : ed->number);
		return;
	}
	if (ed->contact)
		contact_update(ed->contact, name, number);
	else
		contact_add(name, number);
	ui_pop();
}

static void field_cb(lv_event_t *e)
{
	struct editor *ed = lv_event_get_user_data(e);
	lv_obj_t *ta = lv_event_get_target_obj(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		editor_type(ed, ta);
		break;
	case LV_EVENT_READY: /* Enter: the next field, then save */
		if (ta == ed->name)
			editor_type(ed, ed->number);
		else
			editor_save(ed);
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(ed->kb, false);
		break;
	default:
		break;
	}
}

static void save_cb(lv_event_t *e)
{
	editor_save(lv_event_get_user_data(e));
}

static void editor_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static lv_obj_t *field_create(struct editor *ed, lv_obj_t *parent, const char *label, uint32_t max_length,
                              const char *text)
{
	ui_section_label(parent, label);
	lv_obj_t *ta = ui_text_field(parent, FIELD_H, max_length, field_cb, ed);
	lv_obj_set_width(ta, LV_PCT(100));
	lv_textarea_set_text(ta, text);
	return ta;
}

/* `c`: the contact to change, NULL for a new one (with `number` if not NULL) */
static void editor_open(const struct contact *c, const char *number)
{
	struct editor *ed = calloc(1, sizeof(*ed));
	ed->contact = c;

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(c ? _("Edit contact") : _("New contact"), &header, &body);
	lv_obj_add_event_cb(scr, editor_delete_cb, LV_EVENT_DELETE, ed);
	ui_header_button(header, LV_SYMBOL_OK, save_cb, ed);
	lv_obj_set_scrollable(body, false);

	/* Scrolls above the keyboard */
	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(content, 16, 0);
	lv_obj_set_style_pad_bottom(content, 32, 0);
	lv_obj_set_style_pad_row(content, 12, 0);
	lv_obj_set_scrollable(content, true);
	lv_obj_set_clickable(content, true);
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

	ed->name = field_create(ed, content, _("Name"), 64, c ? c->name : "");
	lv_textarea_set_placeholder_text(ed->name, _("First and last name"));
	ed->number = field_create(ed, content, _("Mobile"), 32, c ? c->number : number ? number : "");
	lv_textarea_set_placeholder_text(ed->number, "+44 7700 900123");
	ui_textarea_set_phone(ed->number);

	ed->kb = ui_keyboard_create(body, ed->name);
	/* A new contact starts with the keyboard, an existing one reads first */
	if (!c)
		ui_keyboard_show(ed->kb, true);
	ui_push(scr);
}

/* Detail */

struct detail {
	const struct contact *contact;
	lv_obj_t *avatar, *name, *number, *favorite, *confirm;
};

static void call_cb(lv_event_t *e)
{
	const struct contact *c = lv_event_get_user_data(e);
	phone_call(c->number);
}

/* The number in the dialer, to change it before calling */
static void dial_cb(lv_event_t *e)
{
	const struct contact *c = lv_event_get_user_data(e);
	phone_dial(c->number);
}

static void message_cb(lv_event_t *e)
{
	const struct contact *c = lv_event_get_user_data(e);
	messages_open_thread(c->number);
}

/* A filled star on orange, or an outline on gray */
static void favorite_show(struct detail *d)
{
	bool on = d->contact->favorite;
	ui_button_set_color(d->favorite, on ? COLOR_FAVORITE : UI_COLOR_BUTTON,
	                    on ? UI_COLOR_ORANGE_PRESSED : UI_COLOR_BUTTON_PRESSED);
	lv_label_set_text_static(lv_obj_get_child(d->favorite, 0), on ? STAR_ON : STAR_OFF);
}

static void favorite_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	ui_vibrate(UI_VIBRATE_TAP);
	contact_set_favorite(d->contact, !d->contact->favorite);
	favorite_show(d);
}

/* Back from the editor: show the changes */
static void detail_load_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	const struct contact *c = d->contact;
	ui_avatar_set(d->avatar, c->color, c->initials);
	lv_label_set_text_static(d->name, c->name);
	lv_label_set_text_static(d->number, c->number);
}

static void edit_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	lv_obj_set_hidden(d->confirm, true);
	editor_open(d->contact, NULL);
}

static void trash_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	lv_obj_set_hidden(d->confirm, !lv_obj_is_hidden(d->confirm));
}

static void cancel_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	lv_obj_set_hidden(d->confirm, true);
}

static void delete_cb(lv_event_t *e)
{
	struct detail *d = lv_event_get_user_data(e);
	contact_delete(d->contact);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_pop();
}

static void detail_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void detail_open(const struct contact *c)
{
	struct detail *d = calloc(1, sizeof(*d));
	d->contact = c;

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("", &header, &body);
	lv_obj_add_event_cb(scr, detail_load_cb, LV_EVENT_SCREEN_LOAD_START, d);
	lv_obj_add_event_cb(scr, detail_delete_cb, LV_EVENT_DELETE, d);
	ui_header_button(header, LV_SYMBOL_TRASH, trash_cb, d);
	ui_header_button(header, LV_SYMBOL_EDIT, edit_cb, d);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(body, false);

	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(content, 24, 0);
	lv_obj_set_style_pad_top(content, 24, 0);

	d->avatar = avatar_create(content, c, 224);

	d->name = lv_label_create(content);
	lv_label_set_text_static(d->name, c->name);
	lv_obj_set_width(d->name, LV_PCT(100));
	lv_obj_set_style_text_align(d->name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(d->name, UI_FONT_DISPLAY, 0);
	lv_obj_set_style_margin_top(d->name, 32, 0);

	lv_obj_t *actions = ui_box(content);
	lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(actions, 48, 0);
	lv_obj_set_style_margin_top(actions, 48, 0);
	ui_captioned_button(actions, 120, UI_COLOR_GREEN, UI_COLOR_GREEN_PRESSED, UI_SYMBOL_PHONE, _("Call"),
	                    call_cb, (void *)c);
	ui_captioned_button(actions, 120, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, LV_SYMBOL_ENVELOPE, _("Message"),
	                    message_cb, (void *)c);
	d->favorite = ui_captioned_button(actions, 120, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, STAR_ON,
	                                  _("Favorite"), favorite_cb, d);
	favorite_show(d);

	/* The number, tapping it puts it in the dialer (a stray tap does not call) */
	lv_obj_t *card = lv_obj_create(content);
	lv_obj_remove_style_all(card);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(card, 32, 0);
	lv_obj_set_style_pad_ver(card, 24, 0);
	lv_obj_set_style_pad_row(card, 6, 0);
	lv_obj_set_style_margin_top(card, 56, 0);
	lv_obj_set_style_radius(card, 24, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_add_event_cb(card, dial_cb, LV_EVENT_CLICKED, (void *)c);

	lv_obj_t *kind = lv_label_create(card);
	lv_label_set_text_static(kind, "mobile");
	lv_obj_set_style_text_font(kind, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(kind, lv_color_hex(UI_COLOR_DIM), 0);
	d->number = lv_label_create(card);
	lv_label_set_text_static(d->number, c->number);
	lv_obj_set_style_text_font(d->number, UI_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(d->number, lv_color_hex(UI_COLOR_ACCENT), 0);

	/* Delete confirmation, shown by the trash button */
	char text[96];
	snprintf(text, sizeof(text), LV_SYMBOL_TRASH "  %s", _("Delete contact"));
	d->confirm = ui_confirm_bar(body, ui_intern(text), delete_cb, cancel_cb, d);

	ui_push(scr);
}

/* List */

static void row_cb(lv_event_t *e)
{
	detail_open(lv_event_get_user_data(e));
}

static lv_obj_t *section_create(lv_obj_t *list, unsigned section)
{
	lv_obj_t *label = ui_section_label(list, letters[section]);
	/* An index: in the accent color */
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_ACCENT), 0);

	return ui_card_create(list);
}

static void row_create(lv_obj_t *card, const struct contact *c, bool divider)
{
	lv_obj_t *row = ui_row_create(card, ROW_H, divider, row_cb, (void *)c);
	avatar_create(row, c, 72);
	lv_obj_t *name = ui_one_line_label(row, c->name, UI_FONT_TITLE);
	lv_obj_set_flex_grow(name, 1);
}

static void list_fill(lv_obj_t *body)
{
	lv_obj_clean(body);
	lv_obj_set_user_data(body, (void *)(uintptr_t)data_version);

	unsigned n = contact_count();
	if (!n) {
		ui_empty_label(body, _("No contacts"));
		return;
	}

	/* Favorites first, then everyone (favorites too) */
	unsigned favorites = 0;
	for (unsigned i = 0; i < n; i++)
		favorites += contact_get(i)->favorite;
	lv_obj_t *card = NULL;
	if (favorites) {
		char text[64];
		snprintf(text, sizeof(text), STAR_ON "  %s", _("Favorites"));
		lv_obj_t *label = ui_section_label(body, ui_intern(text));
		lv_obj_set_style_text_color(label, lv_color_hex(COLOR_FAVORITE), 0);
		card = ui_card_create(body);
		for (unsigned i = 0; i < n; i++)
			if (contact_get(i)->favorite)
				row_create(card, contact_get(i), --favorites > 0);
		card = NULL;
	}

	/* Sorted: a section starts where the first letter changes */
	for (unsigned i = 0; i < n; i++) {
		const struct contact *c = contact_get(i), *next = contact_get(i + 1);
		unsigned section = contact_section(c);
		if (!card || section != contact_section(contact_get(i - 1)))
			card = section_create(body, section);
		row_create(card, c, next && contact_section(next) == section);
	}

	lv_obj_t *count = lv_label_create(body);
	ui_set_text_fmt(count, P_("%u contact", "%u contacts", n), n);
	lv_obj_set_width(count, LV_PCT(100));
	lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(count, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(count, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_margin_top(count, 24, 0);
}

/* Back from the detail or the editor: a contact may have changed */
static void list_load_cb(lv_event_t *e)
{
	lv_obj_t *body = lv_event_get_user_data(e);
	if ((uintptr_t)lv_obj_get_user_data(body) != data_version)
		list_fill(body);
}

static void add_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	contacts_new();
}

void contacts_list_create(lv_obj_t *list)
{
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	list_fill(list);
	lv_obj_add_event_cb(lv_obj_get_screen(list), list_load_cb, LV_EVENT_SCREEN_LOAD_START, list);
}

void contacts_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Contacts"), &header, &body);
	ui_header_button(header, LV_SYMBOL_PLUS, add_cb, NULL);
	contacts_list_create(body);
	ui_push(scr);
}

void contacts_new(void)
{
	editor_open(NULL, NULL);
}

void contacts_show(const char *number)
{
	const struct contact *c = contact_find(number);
	if (c)
		detail_open(c);
	else
		editor_open(NULL, number);
}
