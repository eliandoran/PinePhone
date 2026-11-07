/*
 * A repository's open milestones to choose one from, on a screen of their own over
 * a pull request's or an issue's: "No milestone" first, then each with its due date,
 * GitHub's order (the soonest due first), the current one ticked. The one tapped goes
 * to the caller, then the screen closes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "github.h"

#define MILESTONES 100 /* GitHub's largest page */

static struct {
	lv_obj_t *scr, *card, *status;
	long long current;
	void (*picked)(long long number, const char *title);
	struct {
		long long number;
		const char *title, *due; /* interned; due NULL: none */
	} list[MILESTONES];
	unsigned count;
	struct ui_fetch fetch;
} m;

/* The one at that index, -1: none */
static void row_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	void (*picked)(long long, const char *) = m.picked;
	long long number = i < 0 ? 0 : m.list[i].number;
	const char *title = i < 0 ? NULL : m.list[i].title;
	ui_pop();
	picked(number, title);
}

static void row(const char *title, const char *due, long long number, int index, bool divider)
{
	lv_obj_t *row = ui_row_create(m.card, LV_SIZE_CONTENT, divider, row_cb, (void *)(intptr_t)index);
	lv_obj_set_style_pad_ver(row, 24, 0);
	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *label = ui_one_line_label(text, title, UI_FONT_BODY);
	lv_obj_set_width(label, LV_PCT(100));
	if (due) {
		lv_obj_t *when = ui_dim_label(text, "", UI_FONT_CAPTION);
		ui_set_text_fmt(when, _("Due %s"), due);
	}
	if (number == m.current) {
		lv_obj_t *tick = lv_label_create(row);
		lv_label_set_text_static(tick, LV_SYMBOL_OK);
		lv_obj_set_style_text_color(tick, lv_color_hex(UI_COLOR_ACCENT), 0);
	}
}

static void fill(void)
{
	lv_obj_clean(m.card);
	row(_("No milestone"), NULL, 0, -1, m.count > 0);
	for (unsigned i = 0; i < m.count; i++)
		row(m.list[i].title, m.list[i].due, m.list[i].number, (int)i, i + 1 < m.count);
}

/* "2026-11-30T08:00:00Z": its day, as GitHub shows it */
static const char *due_format(const char *iso)
{
	int y, mon, d;
	if (!iso || sscanf(iso, "%d-%d-%d", &y, &mon, &d) != 3 || mon < 1 || mon > 12)
		return NULL;
	char due[48];
	snprintf(due, sizeof(due), "%d %s %d", d, ui_month_name(mon - 1, true), y); /* NON-NLS */
	return ui_intern(due);
}

static void loaded(const char *data, long status)
{
	if (status == 401) {
		github_refused();
		return;
	}
	struct json *j = data && status == 200 ? json_parse(data) : NULL;
	if (!j || j->kind != JSON_ARRAY) {
		if (!status)
			lv_label_set_text_static(m.status, _("Cannot reach GitHub"));
		else
			ui_set_text_fmt(m.status, _("GitHub answered with an error (HTTP %ld)"), status);
		json_free(j);
		return;
	}
	m.count = 0;
	for (const struct json *it = j->child; it && m.count < MILESTONES; it = it->next) {
		m.list[m.count].number = json_int(json_get(it, "number"), 0);
		m.list[m.count].title = ui_intern(json_str(json_get(it, "title"), ""));
		m.list[m.count++].due = due_format(json_str(json_get(it, "due_on"), NULL));
	}
	json_free(j);
	if (m.count)
		lv_obj_set_hidden(m.status, true);
	else
		lv_label_set_text_static(m.status, _("No open milestones"));
	fill();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&m.fetch);
	memset(&m, 0, sizeof(m));
}

void milestone_pick(const char *repo, long long current, void (*picked)(long long number, const char *title))
{
	if (m.scr)
		return;
	lv_obj_t *header, *body;
	m.scr = ui_app_screen_create(_("Milestone"), &header, &body);
	lv_obj_add_event_cb(m.scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_pad_all(body, 16, 0);
	lv_obj_set_style_pad_row(body, 16, 0);
	m.current = current;
	m.picked = picked;
	m.card = ui_card_create(body);
	m.status = ui_empty_label(body, _("Loading..."));
	fill();

	char path[256];
	snprintf(path, sizeof(path), "/repos/%s/milestones?state=open&sort=due_on&per_page=100", repo);
	api_request(&m.fetch, "GET", path, NULL, loaded);
	ui_push(m.scr);
}
