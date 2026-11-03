/*
 * Telegram's screen: the login form until the account is ready, then the chat
 * list; the settings button shows the account (and logs out) instead of the
 * chats, and back. See telegram.h for the other files.
 */

#include <string.h>

#include "telegram.h"

static struct {
	lv_obj_t *scr;
	lv_obj_t *account;
	bool show_account;
} app;

void telegram_show(void)
{
	if (!app.scr)
		return;
	if (tg.state != ST_READY)
		app.show_account = false;

	bool chats = tg.state == ST_READY && !app.show_account;
	lv_obj_set_hidden(app.account, tg.state != ST_READY);
	login_show(!chats);
	list_show(chats);
}

static void account_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	app.show_account = !app.show_account;
	telegram_show();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&app, 0, sizeof(app));
}

void telegram_open(void)
{
	lv_obj_t *header, *body;
	app.scr = ui_app_screen_create("Telegram", &header, &body);
	lv_obj_add_event_cb(app.scr, delete_cb, LV_EVENT_DELETE, NULL);
	app.account = ui_header_button(header, LV_SYMBOL_SETTINGS, account_cb, NULL);
	lv_obj_set_scrollable(body, false);

	login_create(body);
	list_create(body);

	telegram_start();
	telegram_show();
	/* The first page if the chats are not loaded yet (login done earlier) */
	load_chats();
	ui_push(app.scr);
}
