/*
 * Trilium: the app's screen, see trilium.h. Setting up sync until the database has a
 * server and the document's secret (setup.c); then the first sync's progress, as
 * Trilium's setup shows it (its steps, the bar of what was pulled), or why it failed;
 * once synced, the root's notes (tree.c) or Search (search.c) by a bar at the bottom,
 * what the document holds behind the header's info button, beside the sync indicator
 * (the screens of notes have it too); a sync each time it opens, both ways. A timer
 * follows the first sync (sync.c) while the screen is there; the sync goes on without it.
 */

#include <stdlib.h>
#include <string.h>

#include "trilium.h"
#include "ui_symbols.h"

#define PAD 32
#define STEP_H 88
#define STEPS 3

enum { TAB_NOTES, TAB_SEARCH, TABS };

static struct {
	lv_obj_t *body, *info, *sync;
	lv_obj_t *notes, *search, *nav; /* once synced */
	lv_timer_t *timer;
	bool progress, failed;        /* what the body shows */
	lv_obj_t *icons[STEPS], *bar, *percent;
} view;

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	lv_label_set_text(label, text);
	return label;
}

/* A dim line of what the document holds */
static void count_line(lv_obj_t *body, const char *text)
{
	lv_obj_t *label = ui_dim_label(body, "", UI_FONT_BODY);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_margin_top(label, 12, 0);
	lv_label_set_text(label, text);
}

/* What the document holds, into `body` */
static void document_show(lv_obj_t *body, sqlite3 *db)
{
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(body, 16, 0);

	char host[600], text[200];
	tr_db_option(db, "syncServerHost", host, sizeof(host));
	/* NON-NLS-BEGIN: SQL */
	long long notes = tr_db_int(db, "SELECT COUNT(*) FROM notes WHERE isDeleted = 0", 0);
	long long attributes = tr_db_int(db, "SELECT COUNT(*) FROM attributes WHERE isDeleted = 0", 0);
	long long attachments = tr_db_int(db, "SELECT COUNT(*) FROM attachments WHERE isDeleted = 0", 0);
	long long revisions = tr_db_int(db, "SELECT COUNT(*) FROM revisions", 0);
	long long bytes = tr_db_int(db, "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()", 0);
	/* NON-NLS-END */

	lv_obj_t *count = ui_empty_label(body, "");
	lv_obj_set_style_text_color(count, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_text_font(count, UI_FONT_TITLE, 0);
	if (notes)
		ui_set_text_fmt(count, P_("%lld note", "%lld notes", notes), notes);
	else
		lv_label_set_text_static(count, _("No notes yet"));
	snprintf(text, sizeof(text), P_("%lld attribute", "%lld attributes", attributes), attributes);
	count_line(body, text);
	snprintf(text, sizeof(text), P_("%lld attachment", "%lld attachments", attachments), attachments);
	count_line(body, text);
	snprintf(text, sizeof(text), P_("%lld revision", "%lld revisions", revisions), revisions);
	count_line(body, text);

	const char *scheme = strstr(host, "://");
	snprintf(text, sizeof(text), _("Syncs with %s"), scheme ? scheme + 3 : host);
	count_line(body, text);

	char size[16];
	ui_size_format(size, sizeof(size), bytes > 0 ? (unsigned long long)bytes : 0);
	lv_obj_t *details = ui_dim_label(body, "", UI_FONT_CAPTION);
	lv_obj_set_style_margin_top(details, 12, 0);
	ui_set_text_fmt(details, _("Database %s, SQLite %s"), size, sqlite3_libversion());
}

static void info_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Document"), NULL, &body);
	sqlite3 *db = tr_db_open();
	if (db) {
		document_show(body, db);
		sqlite3_close(db);
	} else {
		ui_empty_label(body, _("The database cannot be opened"));
	}
	ui_push(scr);
}


/* The bar's tabs: the notes, Search */

static void tab_select(unsigned tab)
{
	ui_nav_bar_select(view.nav, tab);
	lv_obj_set_hidden(view.notes, tab != TAB_NOTES);
	lv_obj_set_hidden(view.search, tab != TAB_SEARCH);
	if (tab == TAB_SEARCH)
		tr_search_show(view.search, view.nav);
}

static void tab_cb(lv_event_t *e)
{
	tab_select(ui_nav_bar_index(e));
}

/* The root's notes, or Search, by the bar at the bottom; the info button shown */
static void notes_show(sqlite3 *db)
{
	/* Trilium's icons: static, as the bar keeps them */
	static char icons[TABS][8];
	tr_icon(icons[TAB_NOTES], "bx-notepad"); /* NON-NLS */
	tr_icon(icons[TAB_SEARCH], "bx-search"); /* NON-NLS */
	const struct ui_nav_tab tabs[TABS] = {
		[TAB_NOTES] = {_("Notes"), icons[TAB_NOTES]},
		[TAB_SEARCH] = {_("Search"), icons[TAB_SEARCH]},
	};
	lv_obj_set_scrollable(view.body, false);
	view.notes = ui_box(view.body);
	lv_obj_set_width(view.notes, LV_PCT(100));
	lv_obj_set_flex_grow(view.notes, 1);
	lv_obj_set_flex_flow(view.notes, LV_FLEX_FLOW_COLUMN);
	lv_obj_t *plus = tr_tree_fill(view.notes, db, "root", NULL); /* NON-NLS */
	if (plus) /* over the bar, which is clear of the home gesture's strip */
		lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -32, -32);
	view.search = tr_search_create(view.body);
	view.nav = ui_nav_bar_create(view.body, tabs, TABS, 32, tab_cb, NULL);
	for (unsigned i = 0; i < TABS; i++)
		lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(view.nav, (int32_t)i), 0), tr_icon_font_px(40), 0);
	lv_obj_set_hidden(view.search, true);
	ui_nav_bar_select(view.nav, TAB_NOTES);
	lv_obj_set_hidden(view.info, false);
	lv_obj_set_hidden(view.sync, false);
}
/* A column padded as the form, for the sync's screens */
static lv_obj_t *column(void)
{
	lv_obj_t *col = ui_box(view.body);
	lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(col, PAD, 0);
	lv_obj_set_style_pad_ver(col, 48, 0);
	lv_obj_set_style_pad_row(col, 24, 0);
	return col;
}

/* The steps: connecting, syncing data (the bar), checking */
static void progress_show(void)
{
	static const char *const names[STEPS] = {N_("Connecting to the server"), N_("Syncing data"),
	                                         N_("Checking the notes")};
	view.progress = true;
	lv_obj_t *col = column();
	text_label(col, _("Sync in progress"), UI_FONT_HEADLINE, UI_COLOR_TEXT);
	text_label(col, _("The first sync copies all your notes to the phone. It goes on if you leave the app."),
	           UI_FONT_BODY, UI_COLOR_DIM);
	lv_obj_t *card = ui_card_create(col);
	for (int i = 0; i < STEPS; i++) {
		lv_obj_t *row = ui_row_create(card, STEP_H, i + 1 < STEPS, NULL, NULL);
		lv_obj_set_style_pad_column(row, 20, 0);
		view.icons[i] = lv_label_create(row);
		lv_obj_set_style_text_font(view.icons[i], UI_FONT_TITLE, 0);
		lv_obj_t *text = ui_row_text(row);
		ui_one_line_label(text, _(names[i]), UI_FONT_BODY_LARGE);
		if (i == 1) {
			lv_obj_t *line = ui_row_line(text);
			lv_obj_set_style_pad_column(line, 16, 0);
			view.bar = lv_bar_create(line);
			lv_obj_set_height(view.bar, 12);
			lv_obj_set_flex_grow(view.bar, 1);
			lv_obj_set_style_bg_color(view.bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
			lv_obj_set_style_bg_color(view.bar, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
			lv_bar_set_range(view.bar, 0, 100);
			view.percent = ui_dim_label(line, "0%", UI_FONT_CAPTION);
			lv_obj_set_height(row, STEP_H + 32);
		}
	}
}

static void progress_update(const struct tr_sync_state *st)
{
	/* The step under way: 0 connecting, 1 pulling, 2 checking */
	int current = st->step == TR_SYNC_PULLING ? 1 : st->step == TR_SYNC_CHECKING ? 2 : 0;
	for (int i = 0; i < STEPS; i++) {
		bool done = i < current;
		lv_label_set_text_static(view.icons[i], done ? UI_SYMBOL_CIRCLE_CHECK
		                                        : i == current ? UI_SYMBOL_CIRCLE_DOT_REGULAR : UI_SYMBOL_CIRCLE);
		lv_obj_set_style_text_color(view.icons[i],
		                            lv_color_hex(done ? UI_COLOR_GREEN : i == current ? UI_COLOR_ACCENT : UI_COLOR_TRACK), 0);
	}
	int percent = current > 1 ? 100 : st->total > 0 ? (int)(st->pulled * 100 / st->total) : 0;
	lv_bar_set_value(view.bar, percent, LV_ANIM_OFF);
	ui_set_text_fmt(view.percent, "%d%%", percent);
}

static void retry_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	tr_show();
}

static void failure_show(const struct tr_sync_state *st)
{
	view.failed = true;
	lv_obj_t *col = column();
	text_label(col, _("Sync failed"), UI_FONT_HEADLINE, UI_COLOR_TEXT);
	char text[300];
	snprintf(text, sizeof(text), _(st->error ? st->error : N_("The server's answer could not be read.")), st->status);
	text_label(col, text, UI_FONT_BODY, UI_COLOR_RED);
	text_label(col, _("What was copied so far is kept: trying again goes on from there."), UI_FONT_BODY,
	           UI_COLOR_DIM);
	ui_button(col, LV_PCT(100), 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Try again"), retry_cb, NULL);
}

static void body_clean(void)
{
	lv_obj_clean(view.body);
	lv_obj_set_scrollable(view.body, true);
	view.notes = view.search = view.nav = NULL;
	lv_obj_set_hidden(view.info, true);
	lv_obj_set_hidden(view.sync, true);
	view.progress = view.failed = false;
	memset(view.icons, 0, sizeof(view.icons));
	view.bar = view.percent = NULL;
}

/* The sync's state on the screen: its progress, its end */
static void timer_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (!view.progress)
		return;
	struct tr_sync_state st;
	tr_sync_get(&st);
	if (st.step == TR_SYNC_FAILED) {
		body_clean();
		failure_show(&st);
	} else if (st.step == TR_SYNC_DONE) {
		body_clean();
		sqlite3 *db = tr_db_open();
		if (db) {
			notes_show(db);
			sqlite3_close(db);
		} else {
			ui_empty_label(view.body, _("The database cannot be opened"));
		}
	} else {
		progress_update(&st);
	}
}

void tr_show(void)
{
	body_clean();
	sqlite3 *db = tr_db_open();
	if (!db) {
		ui_empty_label(view.body, _("The database cannot be opened"));
		return;
	}
	char secret[128], initialized[8];
	tr_db_option(db, "documentSecret", secret, sizeof(secret));
	tr_db_option(db, "initialized", initialized, sizeof(initialized));
	if (!secret[0]) {
		tr_setup_create(view.body);
	} else if (!strcmp(initialized, "true")) {
		notes_show(db);
		tr_sync_start(); /* what changed since, both ways */
	} else {
		tr_sync_start();
		progress_show();
		timer_cb(NULL);
	}
	sqlite3_close(db);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(view.timer);
	memset(&view, 0, sizeof(view));
}

/* The sync indicator, as Trilium's (SyncStatus.tsx): connected (bx-wifi), the server not
 * reached (bx-wifi-off), syncing (bx-analyse, still: a label is not turned), none yet
 * (bx-time); a star while this phone's changes wait to be pushed. Tapped, a sync. */

struct indicator {
	lv_obj_t *icon, *star;
	lv_timer_t *timer;
};

static void indicator_update(struct indicator *in)
{
	struct tr_sync_state st;
	tr_sync_get(&st);
	bool syncing = st.step == TR_SYNC_CONNECTING || st.step == TR_SYNC_PULLING || st.step == TR_SYNC_CHECKING;
	/* NON-NLS-BEGIN: Boxicons */
	const char *name = syncing ? "bx-analyse" : st.step == TR_SYNC_DONE ? "bx-wifi"
	                   : st.step == TR_SYNC_FAILED ? "bx-wifi-off" : "bx-time";
	/* NON-NLS-END */
	char icon[8];
	tr_icon(icon, name);
	if (strcmp(lv_label_get_text(in->icon), icon))
		lv_label_set_text(in->icon, icon);
	lv_obj_set_style_text_color(in->icon, lv_color_hex(syncing ? UI_COLOR_ACCENT : UI_COLOR_TEXT), 0);
	lv_obj_set_hidden(in->star, !st.pending || syncing);
}

static void indicator_timer_cb(lv_timer_t *t)
{
	indicator_update(lv_timer_get_user_data(t));
}

static void indicator_cb(lv_event_t *e)
{
	struct indicator *in = lv_event_get_user_data(e);
	tr_sync_start();
	indicator_update(in);
}

static void indicator_delete_cb(lv_event_t *e)
{
	struct indicator *in = lv_event_get_user_data(e);
	lv_timer_delete(in->timer);
	free(in);
}

lv_obj_t *tr_sync_indicator(lv_obj_t *header)
{
	struct indicator *in = calloc(1, sizeof(*in));
	if (!in)
		return NULL;
	lv_obj_t *button = ui_header_button(header, "", indicator_cb, in);
	in->icon = lv_obj_get_child(button, 0);
	lv_obj_set_style_text_font(in->icon, tr_icon_font(), 0);
	char star[8];
	tr_icon(star, "bxs-star"); /* NON-NLS */
	in->star = lv_label_create(button);
	lv_label_set_text(in->star, star);
	lv_obj_set_style_text_font(in->star, tr_icon_font_px(16), 0);
	lv_obj_set_style_text_color(in->star, lv_color_hex(UI_COLOR_ORANGE), 0);
	lv_obj_align(in->star, LV_ALIGN_CENTER, 18, -16);
	in->timer = lv_timer_create(indicator_timer_cb, 250, in);
	lv_obj_add_event_cb(button, indicator_delete_cb, LV_EVENT_DELETE, in);
	indicator_update(in);
	return button;
}

void tr_open(void)
{
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create("Trilium", &header, &view.body); /* NON-NLS */
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	view.info = ui_header_button(header, UI_SYMBOL_CIRCLE_INFO, info_cb, NULL);
	view.sync = tr_sync_indicator(header);
	view.timer = lv_timer_create(timer_cb, 100, NULL);
	tr_show();
	ui_push(scr);
}
