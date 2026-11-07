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
	app.scr = ui_app_screen_create("Telegram", &header, &body); /* NON-NLS: app.json's name */
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
