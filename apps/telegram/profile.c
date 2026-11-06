/*
 * Telegram's profile, the bar's second tab: the account's photo (tapped: another
 * one from the Gallery), name and phone number, then its name, username and bio,
 * each opening a screen to change it, and logging out (after a confirmation).
 * The account is kept from getMe and the updates about it while the client runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "telegram.h"

#define PHOTO 200    /* the avatar, across */
#define ROW_H 112
#define FIELD_H 88
#define FIRST_LAST_MAX 64  /* first and last name, each (TDLib's limit) */
#define USERNAME_MAX 32
#define BIO_MAX 140  /* Premium's; TDLib refuses longer for others */

enum field { F_NAME, F_USERNAME, F_BIO, FIELDS };

/* The account: outlives the screen */
static struct {
	bool known; /* getMe answered */
	char first[256], last[256], username[64], phone[32];
	char *bio;  /* NULL: not known yet */
	int photo_file; /* its large size, 0: none */
	char *photo_path;
	unsigned photo_tex;
	bool photo_bad;
} me;

/* The tab, while the screen is open */
static struct {
	lv_obj_t *page;
	lv_obj_t *avatar, *name, *phone;
	lv_obj_t *values[FIELDS];
	lv_obj_t *logout, *confirm;
	unsigned tex; /* shown by the avatar */
} tab;

/* The screen changing a field, while it is open */
static struct {
	lv_obj_t *scr, *ta[2], *error, *kb;
	enum field field;
	bool busy; /* the change is on its way */
} ed;

static void photo_drop(void)
{
	ui_texture_delete(me.photo_tex);
	free(me.photo_path);
	me.photo_file = 0;
	me.photo_path = NULL;
	me.photo_tex = 0;
	me.photo_bad = false;
}

void profile_clear(void)
{
	photo_drop();
	free(me.bio);
	memset(&me, 0, sizeof(me));
	profile_refresh();
}

/* The first character of s, upper case if ASCII, appended to out */
static void initial_add(char *out, size_t size, const char *s)
{
	size_t n = 0, len = strlen(out);
	if (!s[0])
		return;
	n = 1;
	while (s[n] && ((unsigned char)s[n] & 0xc0) == 0x80 && n < 4)
		n++;
	if (len + n >= size)
		return;
	memcpy(out + len, s, n);
	out[len + n] = '\0';
	if (out[len] >= 'a' && out[len] <= 'z')
		out[len] -= 'a' - 'A';
}

static void full_name(char *buf, size_t size)
{
	snprintf(buf, size, "%s%s%s", me.first, me.first[0] && me.last[0] ? " " : "", me.last);
}

/* A label's text with emoji drawn (names and bios may have some) */
static void emoji_text(lv_obj_t *label, const char *text)
{
	char *s = ui_emoji_compose(text);
	lv_label_set_text(label, s ? s : text);
	free(s);
}

void profile_refresh(void)
{
	if (!tab.page)
		return;
	char name[520], initials[12] = "";
	full_name(name, sizeof(name));
	initial_add(initials, sizeof(initials), me.first);
	initial_add(initials, sizeof(initials), me.last);
	ui_avatar_set(tab.avatar, UI_COLOR_ACCENT, initials);
	/* Decoded once shown, kept for the process */
	if (me.photo_path && !me.photo_tex && !me.photo_bad) {
		me.photo_tex = jpeg_texture(me.photo_path, PHOTO, true);
		me.photo_bad = !me.photo_tex;
	}
	if (tab.tex != me.photo_tex) {
		tab.tex = me.photo_tex;
		ui_avatar_set_photo(tab.avatar, tab.tex);
	}
	emoji_text(tab.name, name);
	emoji_text(tab.values[F_NAME], name);
	if (me.phone[0])
		ui_set_text_fmt(tab.phone, "+%s", me.phone);
	else
		ui_set_text(tab.phone, "");
	if (me.username[0])
		ui_set_text_fmt(tab.values[F_USERNAME], "@%s", me.username);
	else
		lv_label_set_text_static(tab.values[F_USERNAME], me.known ? _("None") : "");
	if (me.bio && me.bio[0])
		emoji_text(tab.values[F_BIO], me.bio);
	else
		lv_label_set_text_static(tab.values[F_BIO], me.bio ? _("None") : "");
}

/* The large size of a profilePhoto (or null: none), downloaded if it is not yet */
static void photo_set(const struct json *photo)
{
	const struct json *file = json_get(photo, "big");
	int id = json_int(json_get(file, "id"), 0);
	if (id == me.photo_file)
		return;
	photo_drop();
	if (!id)
		return;
	me.photo_file = id;
	profile_file(file);
	if (!me.photo_path) {
		char buf[160];
		snprintf(buf, sizeof(buf), "{\"@type\":\"downloadFile\",\"file_id\":%d,\"priority\":2,"
			"\"offset\":0,\"limit\":0,\"synchronous\":false,\"@extra\":\"download\"}", id);
		td_request(buf);
	}
}

void profile_file(const struct json *file)
{
	const struct json *local = json_get(file, "local");
	const char *path = json_str(json_get(local, "path"), "");
	if (!me.photo_file || json_int(json_get(file, "id"), 0) != me.photo_file || me.photo_path ||
	    !json_bool(json_get(local, "is_downloading_completed")) || !path[0])
		return;
	me.photo_path = strdup(path);
	profile_refresh();
}

void profile_me(const struct json *user)
{
	me.known = true;
	snprintf(me.first, sizeof(me.first), "%s", json_str(json_get(user, "first_name"), ""));
	snprintf(me.last, sizeof(me.last), "%s", json_str(json_get(user, "last_name"), ""));
	/* The first active one is shown (others are collectible ones) */
	const struct json *active = json_get(json_get(user, "usernames"), "active_usernames");
	snprintf(me.username, sizeof(me.username), "%s", json_str(active ? active->child : NULL, ""));
	snprintf(me.phone, sizeof(me.phone), "%s", json_str(json_get(user, "phone_number"), ""));
	photo_set(json_get(user, "profile_photo"));
	profile_refresh();
}

void profile_full_info(const struct json *info)
{
	set_str(&me.bio, json_str(json_get(json_get(info, "bio"), "text"), ""));
	profile_refresh();
}

/* Changing a field */

static void edit_show_error(const char *text)
{
	if (!ed.scr)
		return;
	ui_set_text(ed.error, text);
	lv_obj_set_hidden(ed.error, !text[0]);
}

static void edit_type(lv_obj_t *ta);

/* The answer to the change: ok, back to the profile; an error, said on its screen,
 * the keyboard back to correct it */
void profile_saved(const struct json *answer)
{
	if (!ed.busy)
		return;
	ed.busy = false;
	if (strcmp(json_type(answer), "error")) {
		if (lv_screen_active() == ed.scr)
			ui_pop();
		return;
	}
	const char *message = json_str(json_get(answer, "message"), "");
	printf("telegram: profile: %s\n", message);
	edit_show_error(
		!strcmp(message, "FIRSTNAME_INVALID") ? _("The first name cannot be empty.") :
		!strcmp(message, "LASTNAME_INVALID") ? _("This last name cannot be used.") :
		!strcmp(message, "USERNAME_OCCUPIED") ? _("This username is taken.") :
		!strcmp(message, "USERNAME_INVALID") ?
			_("A username has 5 to 32 letters, digits or underscores, and starts with a letter.") :
		!strcmp(message, "ABOUT_TOO_LONG") ? _("The bio is too long.") :
		!strncmp(message, "Too Many Requests", 17) ? _("Too many attempts, try again later") : message);
	if (ed.scr)
		edit_type(!strcmp(message, "LASTNAME_INVALID") && ed.ta[1] ? ed.ta[1] : ed.ta[0]);
}

/* Without spaces around it (names, usernames, bios) */
static void trimmed(char *out, size_t size, const char *s)
{
	s += strspn(s, " ");
	snprintf(out, size, "%s", s);
	size_t n = strlen(out);
	while (n && out[n - 1] == ' ')
		out[--n] = '\0';
}

static void edit_save(void)
{
	if (ed.busy)
		return;
	char a[sizeof(me.first)], b[sizeof(me.last)];
	trimmed(a, sizeof(a), lv_textarea_get_text(ed.ta[0]));
	b[0] = '\0';
	if (ed.ta[1])
		trimmed(b, sizeof(b), lv_textarea_get_text(ed.ta[1]));
	bool same = ed.field == F_NAME ? !strcmp(a, me.first) && !strcmp(b, me.last) :
		ed.field == F_USERNAME ? !strcmp(a, me.username) : !strcmp(a, me.bio ? me.bio : "");
	if (same) {
		ui_pop();
		return;
	}

	char ea[2 * sizeof(a)], eb[2 * sizeof(b)], buf[sizeof(ea) + sizeof(eb) + 128];
	json_escape(ea, sizeof(ea), a);
	json_escape(eb, sizeof(eb), b);
	if (ed.field == F_NAME)
		snprintf(buf, sizeof(buf), "{\"@type\":\"setName\",\"first_name\":\"%s\",\"last_name\":\"%s\","
			"\"@extra\":\"profile\"}", ea, eb);
	else if (ed.field == F_USERNAME)
		snprintf(buf, sizeof(buf), "{\"@type\":\"setUsername\",\"username\":\"%s\",\"@extra\":\"profile\"}", ea);
	else
		snprintf(buf, sizeof(buf), "{\"@type\":\"setBio\",\"bio\":\"%s\",\"@extra\":\"profile\"}", ea);
	td_request(buf);
	ed.busy = true;
	edit_show_error("");
	ui_keyboard_show(ed.kb, false);
}

/* Types into `ta`, the other field loses the cursor */
static void edit_type(lv_obj_t *ta)
{
	for (int i = 0; i < 2; i++)
		if (ed.ta[i] && ed.ta[i] != ta)
			lv_obj_remove_state(ed.ta[i], LV_STATE_FOCUSED);
	lv_keyboard_set_textarea(ed.kb, ta);
	ui_keyboard_show(ed.kb, true);
}

static void field_cb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target_obj(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		edit_type(ta);
		break;
	case LV_EVENT_READY: /* Enter: the next field, then save */
		if (ta == ed.ta[0] && ed.ta[1])
			edit_type(ed.ta[1]);
		else
			edit_save();
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(ed.kb, false);
		break;
	default:
		break;
	}
}

static void save_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	edit_save();
}

static void edit_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&ed, 0, sizeof(ed));
}

static lv_obj_t *field_create(lv_obj_t *parent, const char *label, uint32_t max_length, const char *text)
{
	ui_section_label(parent, label);
	lv_obj_t *ta = ui_text_field(parent, FIELD_H, max_length, field_cb, NULL);
	lv_obj_set_width(ta, LV_PCT(100));
	lv_textarea_set_text(ta, text);
	return ta;
}

static lv_obj_t *hint_label(lv_obj_t *parent, const char *text, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_obj_set_style_pad_hor(label, 8, 0);
	return label;
}

static void edit_open(enum field field)
{
	static const char *const titles[FIELDS] = { N_("Name"), N_("Username"), N_("Bio") };
	if (ed.scr)
		return;
	lv_obj_t *header, *body;
	ed.scr = ui_app_screen_create(_(titles[field]), &header, &body);
	ed.field = field;
	lv_obj_add_event_cb(ed.scr, edit_delete_cb, LV_EVENT_DELETE, NULL);
	ui_header_button(header, LV_SYMBOL_OK, save_cb, NULL);
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
	lv_obj_set_scroll_dir(content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

	switch (field) {
	case F_NAME:
		ed.ta[0] = field_create(content, _("First name"), FIRST_LAST_MAX, me.first);
		ed.ta[1] = field_create(content, _("Last name"), FIRST_LAST_MAX, me.last);
		lv_textarea_set_placeholder_text(ed.ta[1], _("Optional"));
		break;
	case F_USERNAME:
		ed.ta[0] = field_create(content, _("Username"), USERNAME_MAX, me.username);
		lv_textarea_set_accepted_chars(ed.ta[0],
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"); /* NON-NLS */
		hint_label(content, _("People can find you by it and write to you without your phone number. "
			"Leave it empty to have none."), UI_COLOR_DIM);
		break;
	default:
		ed.ta[0] = field_create(content, _("Bio"), BIO_MAX, me.bio ? me.bio : "");
		hint_label(content, _("A few words about yourself, on one line."), UI_COLOR_DIM);
		break;
	}
	ed.error = hint_label(content, "", UI_COLOR_RED);
	lv_obj_set_hidden(ed.error, true);

	ed.kb = ui_keyboard_create(body, ed.ta[0]);
	lv_textarea_set_cursor_pos(ed.ta[0], LV_TEXTAREA_CURSOR_LAST);
	ui_push(ed.scr);
}

/* The profile's photo */

/* A photo from the Gallery; screenshots (PPM) as JPEG, kept in DIR/sent */
static void photo_picked(const char *path, void *user_data)
{
	LV_UNUSED(user_data);
	char jpeg[256];
	int w, h;
	const char *ext = strrchr(path, '.');
	if (ext && !strcmp(ext, ".ppm")) {
		const char *name = strrchr(path, '/') + 1;
		mkdir(DIR "/sent", 0700);
		snprintf(jpeg, sizeof(jpeg), DIR "/sent/%.*s.jpg", (int)(ext - name), name);
		if (!ppm_to_jpeg(path, jpeg, &w, &h)) {
			printf("telegram: cannot convert %s\n", path);
			return;
		}
		path = jpeg;
	}
	char escaped[512], buf[768];
	json_escape(escaped, sizeof(escaped), path);
	snprintf(buf, sizeof(buf), "{\"@type\":\"setProfilePhoto\",\"photo\":{\"@type\":\"inputChatPhotoStatic\","
		"\"photo\":{\"@type\":\"inputFileLocal\",\"path\":\"%s\"}},\"is_public\":false,"
		"\"@extra\":\"profile photo\"}", escaped); /* NON-NLS */
	td_request(buf);
}

static void avatar_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_photo_pick(_("Profile photo"), photo_picked, NULL);
}

/* The tab */

static void row_cb(lv_event_t *e)
{
	edit_open((enum field)(uintptr_t)lv_event_get_user_data(e));
}

static void confirm_show(bool shown)
{
	lv_obj_set_hidden(tab.confirm, !shown);
	lv_obj_set_hidden(tab.logout, shown);
	if (shown)
		lv_obj_scroll_to_view(tab.confirm, LV_ANIM_ON);
}

static void logout_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	confirm_show(true);
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	confirm_show(false);
}

static void confirm_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	confirm_show(false);
	log_out();
}

static void tab_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&tab, 0, sizeof(tab));
}

void profile_show(bool shown)
{
	if (!tab.page)
		return;
	lv_obj_set_hidden(tab.page, !shown);
	if (!shown)
		confirm_show(false);
}

void profile_create(lv_obj_t *body)
{
	static const char *const captions[FIELDS] = { N_("Name"), N_("Username"), N_("Bio") };

	tab.page = lv_obj_create(body);
	lv_obj_remove_style_all(tab.page);
	lv_obj_set_width(tab.page, LV_PCT(100));
	lv_obj_set_flex_grow(tab.page, 1);
	lv_obj_set_flex_flow(tab.page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tab.page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(tab.page, 16, 0);
	lv_obj_set_style_pad_top(tab.page, 32, 0);
	lv_obj_set_style_pad_bottom(tab.page, 24, 0);
	lv_obj_set_style_pad_row(tab.page, 8, 0);
	lv_obj_set_scroll_dir(tab.page, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(tab.page, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(tab.page, tab_delete_cb, LV_EVENT_DELETE, NULL);

	tab.avatar = ui_avatar_create(tab.page, PHOTO);
	lv_obj_set_style_text_font(tab.avatar, &ui_font_48, 0);
	lv_obj_set_clickable(tab.avatar, true);
	lv_obj_set_style_opa(tab.avatar, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(tab.avatar, avatar_cb, LV_EVENT_CLICKED, NULL);

	tab.name = lv_label_create(tab.page);
	lv_obj_set_width(tab.name, LV_PCT(100));
	lv_label_set_long_mode(tab.name, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(tab.name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(tab.name, &ui_font_32, 0);
	lv_obj_set_style_margin_top(tab.name, 16, 0);
	tab.phone = ui_dim_label(tab.page, "", &ui_font_26);

	lv_obj_t *card = ui_card_create(tab.page);
	lv_obj_set_style_margin_top(card, 24, 0);
	for (int i = 0; i < FIELDS; i++) {
		lv_obj_t *row = ui_row_create(card, LV_SIZE_CONTENT, i + 1 < FIELDS, row_cb, (void *)(uintptr_t)i);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_set_style_min_height(row, ROW_H, 0);
		lv_obj_t *text = ui_row_text(row);
		tab.values[i] = lv_label_create(text);
		lv_label_set_text_static(tab.values[i], "");
		lv_obj_set_width(tab.values[i], LV_PCT(100));
		lv_label_set_long_mode(tab.values[i], LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_font(tab.values[i], &ui_font_26, 0);
		ui_dim_label(text, _(captions[i]), &ui_font_20);
	}

	tab.logout = ui_button(tab.page, LV_PCT(100), 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Log out"),
			       logout_cb, NULL);
	lv_obj_set_style_text_color(lv_obj_get_child(tab.logout, 0), lv_color_hex(UI_COLOR_RED), 0);
	lv_obj_set_style_text_font(lv_obj_get_child(tab.logout, 0), &ui_font_26, 0);
	lv_obj_set_style_margin_top(tab.logout, 32, 0);
	tab.confirm = ui_confirm_bar(tab.page, _("Log out"), confirm_cb, cancel_cb, NULL);
	lv_obj_set_style_margin_top(tab.confirm, 32, 0);

	lv_obj_set_hidden(tab.page, true);
	profile_refresh();
}
