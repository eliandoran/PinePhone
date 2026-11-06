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
	lv_obj_t *nav, *tabs[TABS];
	enum tab tab;
} app;

void telegram_show(void)
{
	if (!app.scr)
		return;
	bool ready = tg.state == ST_READY;
	if (!ready)
		app.tab = TAB_CHATS;
	for (unsigned i = 0; i < TABS; i++)
		lv_obj_set_state(app.tabs[i], LV_STATE_CHECKED, i == app.tab);
	lv_obj_set_hidden(app.nav, !ready);
	login_show(!ready);
	list_show(ready && app.tab == TAB_CHATS);
	contact_list_show(ready && app.tab == TAB_CONTACTS);
	profile_show(ready && app.tab == TAB_PROFILE);
}

static void tab_cb(lv_event_t *e)
{
	app.tab = (enum tab)(uintptr_t)lv_event_get_user_data(e);
	telegram_show();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&app, 0, sizeof(app));
}

/* The bar at the bottom, as GitHub's and Phone's: an icon and a caption per tab, the
 * one shown in the accent color */
static void nav_create(lv_obj_t *body)
{
	static const char *const names[TABS] = { N_("Chats"), N_("Contacts"), N_("Profile") };

	app.nav = ui_box(body);
	lv_obj_set_size(app.nav, LV_PCT(100), 128);
	lv_obj_set_flex_flow(app.nav, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_bg_opa(app.nav, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(app.nav, lv_color_hex(UI_COLOR_BAR), 0);

	for (unsigned i = 0; i < TABS; i++) {
		lv_obj_t *tab = app.tabs[i] = ui_box(app.nav);
		lv_obj_set_height(tab, LV_PCT(100));
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(tab, 8, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(tab, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_DIM), 0);
		lv_obj_set_style_text_color(tab, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_clickable(tab, true);
		lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

		/* The contacts as Phone's; the profile will have the settings too */
		if (i == TAB_CONTACTS) {
			ui_glyph_create(tab, 32, UI_GLYPH_PERSON);
		} else {
			lv_obj_t *icon = lv_label_create(tab);
			lv_label_set_text_static(icon, i == TAB_CHATS ? LV_SYMBOL_ENVELOPE : LV_SYMBOL_SETTINGS);
			lv_obj_set_style_text_font(icon, &ui_font_32, 0);
		}
		lv_obj_t *label = lv_label_create(tab);
		lv_label_set_text_static(label, _(names[i]));
		lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
	}
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
