/*
 * Telegram's screen: the login form until the account is ready, then the chats,
 * the contacts and the profile, switched by a bar at the bottom (starting on the
 * chats each time the app opens). See telegram.h for the other files.
 */

#include <string.h>

#include "telegram.h"

enum tab { TAB_CHATS, TAB_CONTACTS, TAB_PROFILE, TABS };

static struct {
	lv_obj_t *scr;
	lv_obj_t *nav;
	enum tab tab;
} app;

void telegram_show(void)
{
	if (!app.scr)
		return;
	bool ready = tg.state == ST_READY;
	if (!ready)
		app.tab = TAB_CHATS;
	ui_nav_bar_select(app.nav, app.tab);
	lv_obj_set_hidden(app.nav, !ready);
	login_show(!ready);
	list_show(ready && app.tab == TAB_CHATS);
	contact_list_show(ready && app.tab == TAB_CONTACTS);
	profile_show(ready && app.tab == TAB_PROFILE);
}

/* The connection's state under the header of each of its screens while it is not
 * ready (no network, connecting, catching up), as other Telegram apps show it */
#define NOTICES 8
static const char *connection; /* NULL: ready, or not known yet */
static lv_obj_t *notices[NOTICES];

static void notice_set(lv_obj_t *notice)
{
	lv_obj_set_hidden(notice, !connection);
	if (connection)
		lv_label_set_text_static(notice, connection);
}

void telegram_connection(const struct json *state)
{
	const char *type = json_type(state);
	connection = !strcmp(type, "connectionStateWaitingForNetwork") ? _("Waiting for network...") :
	             !strcmp(type, "connectionStateConnectingToProxy") ? _("Connecting to proxy...") :
	             !strcmp(type, "connectionStateConnecting") ? _("Connecting...") :
	             !strcmp(type, "connectionStateUpdating") ? _("Updating...") : NULL;
	for (int i = 0; i < NOTICES; i++)
		if (notices[i])
			notice_set(notices[i]);
}

static void notice_delete_cb(lv_event_t *e)
{
	for (int i = 0; i < NOTICES; i++)
		if (notices[i] == lv_event_get_target_obj(e))
			notices[i] = NULL;
}

static void notice_create(lv_obj_t *body)
{
	int i = 0;
	while (i < NOTICES && notices[i])
		i++;
	if (i == NOTICES)
		return;
	/* Between the header and the body: it does not scroll */
	lv_obj_t *notice = notices[i] = lv_label_create(lv_obj_get_parent(body));
	lv_label_set_text_static(notice, "");
	lv_obj_set_width(notice, LV_PCT(100));
	lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(notice, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(notice, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_pad_bottom(notice, 8, 0);
	lv_obj_move_to_index(notice, lv_obj_get_index(body));
	lv_obj_add_event_cb(notice, notice_delete_cb, LV_EVENT_DELETE, NULL);
	notice_set(notice);
}

/* Its screens are in a state of LVGL's left to apps (no style uses it) */
lv_obj_t *telegram_screen(const char *title, lv_obj_t **header, lv_obj_t **body)
{
	lv_obj_t *scr = ui_app_screen_create(title, header, body);
	lv_obj_set_state_user_1(scr, true);
	notice_create(*body);
	return scr;
}

bool telegram_in_front(void)
{
	return lv_obj_is_state_user_1(lv_screen_active()) && !ui_locked();
}

static void tab_cb(lv_event_t *e)
{
	app.tab = (enum tab)ui_nav_bar_index(e);
	telegram_show();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&app, 0, sizeof(app));
}

/* The bar at the bottom, as GitHub's and Phone's */
static void nav_create(lv_obj_t *body)
{
	/* The contacts as Phone's; the profile will have the settings too */
	const struct ui_nav_tab tabs[TABS] = {
		{ _("Chats"), LV_SYMBOL_ENVELOPE, 0 },
		{ _("Contacts"), NULL, UI_GLYPH_PERSON },
		{ _("Profile"), LV_SYMBOL_SETTINGS, 0 },
	};
	app.nav = ui_nav_bar_create(body, tabs, TABS, 32, tab_cb, NULL);
}

void telegram_open(void)
{
	lv_obj_t *header, *body;
	app.scr = telegram_screen("Telegram", &header, &body); /* NON-NLS: app.json's name */
	lv_obj_add_event_cb(app.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	login_create(body);
	list_create(body);
	contact_list_create(body);
	profile_create(body);
	/* Hidden with the tabs while the login (and its keyboard) shows */
	nav_create(body);

	telegram_start();
	telegram_show();
	/* The first page if the chats are not loaded yet (login done earlier) */
	load_first_chats();
	ui_push(app.scr);
}

void telegram_open_chat(long long id)
{
	if (!app.scr)
		telegram_open();
	chat_open(id);
}
