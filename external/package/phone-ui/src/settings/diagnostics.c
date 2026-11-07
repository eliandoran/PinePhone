/* Settings, Diagnostics: the frame rate counter, the computers allowed over USB, the
 * network's speed, what the sensors read
 * and what phone-ui makes of it */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../pairing.h"
#include "settings.h"

/* The speed test: a download from Cloudflare's speed test, timed from its first data (not
 * the connection) for at most DOWNLOAD_MS, then an upload of what UPLOAD_MS take at that
 * speed, timed from the request (one request each, polled every 50 ms) */

#define SPEED_DOWN_URL "https://speed.cloudflare.com/__down?bytes=25000000"
#define SPEED_UP_URL "https://speed.cloudflare.com/__up"
#define DOWNLOAD_MS 10000
#define UPLOAD_MS 4000
#define UPLOAD_MIN 100000
/* Cloudflare answers the phone "400 Unexpected Server Error" from about 8.1 MB (8 MB went
 * through; a PC with the same headers sends 10): what it holds against it is not known */
#define UPLOAD_MAX 5000000
#define UPLOAD_LIMIT_MS 20000 /* past it, the upload is slower than what it sent in that time */

static struct {
	struct ui_fetch fetch;
	lv_obj_t *down, *up, *start;
	enum { SPEED_IDLE, SPEED_DOWN, SPEED_UP } state;
	uint32_t t0;   /* the download's first data, the upload's request */
	size_t first;  /* bytes in the download's first data */
	size_t size;   /* downloaded, or uploading */
	double bps;    /* the download's speed */
} speed;

static void speed_show(lv_obj_t *label, const char *prefix, double bps)
{
	if (bps < 1e6)
		ui_set_text_fmt(label, "%s%.0f kbit/s", prefix, bps / 1e3);
	else
		ui_set_text_fmt(label, "%s%.1f Mbit/s", prefix, bps / 1e6);
}

/* The download's speed so far, 0 without data after its first */
static double speed_download_bps(void)
{
	uint32_t ms = lv_tick_elaps(speed.t0);
	return speed.size > speed.first && ms ? (speed.size - speed.first) * 8e3 / ms : 0;
}

static void speed_done(void)
{
	speed.state = SPEED_IDLE;
	ui_set_text(speed.start, _("Start speed test"));
}

static void speed_failed(lv_obj_t *label, long status)
{
	if (status)
		ui_set_text_fmt(label, _("Failed (HTTP %ld)"), status);
	else
		ui_set_text(label, _("No connection"));
	speed_done();
}

static void upload_done(const char *data, long status)
{
	LV_UNUSED(data);
	if (status != 200) {
		speed_failed(speed.up, status);
		return;
	}
	uint32_t ms = lv_tick_elaps(speed.t0);
	speed_show(speed.up, "", speed.size * 8e3 / (ms ? ms : 1));
	speed_done();
}

static void upload_start(void)
{
	double size = speed.bps / 8 * UPLOAD_MS / 1000;
	speed.size = size < UPLOAD_MIN ? UPLOAD_MIN : size > UPLOAD_MAX ? UPLOAD_MAX : (size_t)size;
	char *body = malloc(speed.size + 1);
	if (!body) {
		ui_set_text(speed.up, _("Failed"));
		speed_done();
		return;
	}
	memset(body, 'x', speed.size);
	body[speed.size] = '\0';
	static const char *const headers[] = { "Content-Type: application/octet-stream", NULL };
	speed.state = SPEED_UP;
	speed.t0 = lv_tick_get();
	ui_set_text(speed.up, _("Testing\xe2\x80\xa6"));
	ui_fetch_post(&speed.fetch, SPEED_UP_URL, headers, body, NULL, upload_done);
	free(body); /* libcurl copied it */
}

/* The end of the download, or DOWNLOAD_MS of it */
static void download_end(void)
{
	speed.bps = speed_download_bps();
	ui_fetch_stop(&speed.fetch);
	if (speed.bps <= 0) {
		ui_set_text(speed.down, _("Failed"));
		speed_done();
		return;
	}
	speed_show(speed.down, "", speed.bps);
	upload_start();
}

static void download_progress(const char *data, size_t size)
{
	LV_UNUSED(data);
	if (!speed.first) {
		speed.t0 = lv_tick_get();
		speed.first = size;
	}
	speed.size = size;
}

static void download_done(const char *data, long status)
{
	LV_UNUSED(data);
	if (status != 200) {
		speed_failed(speed.down, status);
		return;
	}
	download_end();
}

/* The page's timer: the download's speed as it comes, the time limits */
static void speed_update(void)
{
	if (speed.state == SPEED_DOWN && speed.first) {
		if (lv_tick_elaps(speed.t0) >= DOWNLOAD_MS) {
			download_end();
			return;
		}
		double bps = speed_download_bps();
		if (bps > 0)
			speed_show(speed.down, "", bps);
	} else if (speed.state == SPEED_UP && lv_tick_elaps(speed.t0) >= UPLOAD_LIMIT_MS) {
		ui_fetch_stop(&speed.fetch);
		speed_show(speed.up, _("Under "), speed.size * 8e3 / UPLOAD_LIMIT_MS);
		speed_done();
	}
}

static void speed_start_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (speed.state != SPEED_IDLE)
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	speed.state = SPEED_DOWN;
	speed.first = speed.size = 0;
	ui_set_text(speed.start, _("Testing\xe2\x80\xa6"));
	ui_set_text(speed.down, _("Connecting\xe2\x80\xa6"));
	ui_set_text(speed.up, "");
	ui_fetch_start(&speed.fetch, SPEED_DOWN_URL, download_done);
	/* ui_fetch_start() has no progress of its own: set once it runs */
	if (speed.fetch.curl)
		speed.fetch.progress = download_progress;
}

static void speed_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&speed.fetch);
	speed.state = SPEED_IDLE;
}

static struct {
	lv_obj_t *device, *matrix, *raw[3], *axis[3], *up, *rotation;
} diag;

static void diagnostics_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	speed_update();
	struct ui_accel a;
	if (!ui_accel_read(&a)) {
		ui_set_text(diag.device, _("No accelerometer"));
		return;
	}
	ui_set_text_fmt(diag.device, a.name[0] ? "%s (%s)" : "%s", a.dev, a.name);
	ui_set_text_fmt(diag.matrix, "%g, %g, %g; %g, %g, %g; %g, %g, %g", a.m[0][0], a.m[0][1],
	                a.m[0][2], a.m[1][0], a.m[1][1], a.m[1][2], a.m[2][0], a.m[2][1], a.m[2][2]);
	float v[3];
	int most = 0;
	for (int i = 0; i < 3; i++) {
		ui_set_text_fmt(diag.raw[i], "%.0f", a.raw[i]);
		v[i] = a.m[i][0] * a.raw[0] + a.m[i][1] * a.raw[1] + a.m[i][2] * a.raw[2];
		if (a.scale > 0)
			ui_set_text_fmt(diag.axis[i], "%+.2f m/s²", v[i] * a.scale);
		else
			ui_set_text_fmt(diag.axis[i], "%+.0f", v[i]);
		if (fabsf(v[i]) > fabsf(v[most]))
			most = i;
	}
	static const char *const up[3][2] = {
		{ N_("Left edge"), N_("Right edge") },
		{ N_("Bottom edge"), N_("Top edge") },
		{ N_("Back (face down)"), N_("Screen (face up)") },
	};
	ui_set_text(diag.up, _(up[most][v[most] > 0]));
	switch (a.rotation) {
	case LV_DISPLAY_ROTATION_0:   ui_set_text(diag.rotation, _("Upright")); break;
	case LV_DISPLAY_ROTATION_90:  ui_set_text(diag.rotation, _("Right edge up (90°)")); break;
	case LV_DISPLAY_ROTATION_270: ui_set_text(diag.rotation, _("Left edge up (270°)")); break;
	default: ui_set_text(diag.rotation, _("Stays (flat, upside down or between)")); break;
	}
}

/* The computers allowed over USB (pairing.c): a list, tapped to revoke one */

static struct {
	struct pairing_computer *list;
	int count;
	int chosen; /* to revoke, -1: none */
	lv_obj_t *page, *confirm;
	lv_obj_t *row; /* the chosen one, highlighted while the bar asks */
	lv_obj_t *allowed; /* Diagnostics' count */
} pc = { .chosen = -1 };

static void computers_fill(void);

/* The bar asking to revoke `row`'s computer, or hidden (NULL) */
static void choose(lv_obj_t *row, int i)
{
	if (pc.row)
		lv_obj_remove_state(pc.row, LV_STATE_CHECKED);
	pc.row = row;
	pc.chosen = row ? i : -1;
	if (row)
		lv_obj_add_state(row, LV_STATE_CHECKED);
	lv_obj_set_hidden(pc.confirm, !row);
}

static void computer_cb(lv_event_t *e)
{
	choose(lv_event_get_current_target_obj(e), (int)(intptr_t)lv_event_get_user_data(e));
}

static void revoke_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pc.chosen >= 0 && pc.chosen < pc.count)
		pairing_revoke(pc.list[pc.chosen].b64);
	choose(NULL, -1);
	computers_fill();
}

static void revoke_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	choose(NULL, -1);
}

static void computers_fill(void)
{
	free(pc.list);
	pc.list = pairing_computers(&pc.count);
	pc.row = NULL;
	lv_obj_clean(pc.page);
	const char *text = _("These computers can control the phone over USB. Revoked, a computer is asked again the next time.");
	lv_obj_t *note = ui_dim_label(pc.page, text, UI_FONT_CAPTION);
	lv_obj_set_width(note, LV_PCT(100));
	if (!pc.count) {
		ui_empty_label(pc.page, _("No computers allowed"));
		return;
	}
	lv_obj_t *card = ui_card_create(pc.page);
	for (int i = 0; i < pc.count; i++) {
		const struct pairing_computer *c = &pc.list[i];
		lv_obj_t *row = ui_row_create(card, LV_SIZE_CONTENT, i < pc.count - 1,
		                              computer_cb, (void *)(intptr_t)i);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_set_style_radius(row, 24, LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *name = lv_label_create(text);
		ui_set_text(name, c->name[0] ? c->name : _("Unnamed computer"));
		lv_obj_set_width(name, LV_PCT(100));
		lv_obj_set_style_text_font(name, UI_FONT_TITLE, 0);
		lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT), 0);
		lv_obj_t *fp = ui_dim_label(text, "", UI_FONT_CAPTION);
		ui_set_text(fp, c->fp);
		lv_obj_set_width(fp, LV_PCT(100));
	}
}

static void computers_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(pc.list);
	pc.list = NULL;
	pc.count = 0;
	pc.chosen = -1;
	pc.page = pc.confirm = pc.row = NULL;
}

static void computers_open(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create(_("Allowed computers"), NULL, &body);
	pc.page = ui_box(body);
	lv_obj_set_width(pc.page, LV_PCT(100));
	lv_obj_set_flex_grow(pc.page, 1);
	lv_obj_set_flex_flow(pc.page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(pc.page, 20, 0);
	lv_obj_set_style_pad_ver(pc.page, 8, 0);
	lv_obj_set_style_pad_row(pc.page, 16, 0);
	lv_obj_set_scrollable(pc.page, true);
	lv_obj_set_scroll_dir(pc.page, LV_DIR_VER);
	pc.confirm = ui_confirm_bar(body, _("Revoke access"), revoke_cb, revoke_cancel_cb, NULL);
	lv_obj_add_event_cb(scr, computers_delete_cb, LV_EVENT_DELETE, NULL);
	computers_fill();
	ui_push(scr);
}

/* Diagnostics' count, again when it is shown again (back from the list) */
static void allowed_update_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	int count;
	free(pairing_computers(&count));
	ui_set_text_fmt(pc.allowed, "%d", count);
}

static void fps_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
	ui_vibrate(UI_VIBRATE_TAP);
	ui_setting_set_int("fps_counter", on);
	ui_fps_counter_set(on);
}

void diagnostics_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Diagnostics"), &page);
	static const char *const raw[3] = { "x", "y", "z" };
	static const char *const axis[3] = { N_("x (right)"), N_("y (top)"), N_("z (out of the screen)") };

	/* The frame rate and CPU in the bottom right corner, over everything */
	ui_section_label(page, _("Performance"));
	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, _("Show frame rate"));
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_setting_get_int("fps_counter", 0));
	lv_obj_add_event_cb(sw, fps_cb, LV_EVENT_VALUE_CHANGED, NULL);

	ui_section_label(page, _("Computers on USB"));
	card = ui_card_create(page);
	row = settings_row(card);
	settings_row_tappable(row, computers_open, NULL);
	label = lv_label_create(row);
	lv_label_set_text_static(label, _("Allowed computers"));
	lv_obj_set_flex_grow(label, 1);
	pc.allowed = settings_value(row);
	allowed_update_cb(NULL);
	lv_obj_add_event_cb(scr, allowed_update_cb, LV_EVENT_SCREEN_LOADED, NULL);

	/* Downloads up to 25 MB, then uploads about 4 s worth: on mobile data, it counts */
	ui_section_label(page, _("Speed test"));
	card = ui_card_create(page);
	speed.down = settings_info_row(card, _("Download"));
	speed.up = settings_info_row(card, _("Upload"));
	row = settings_row(card);
	settings_row_tappable(row, speed_start_cb, NULL);
	speed.start = lv_label_create(row);
	lv_label_set_text_static(speed.start, _("Start speed test"));
	lv_obj_set_style_text_color(speed.start, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_add_event_cb(scr, speed_delete_cb, LV_EVENT_DELETE, NULL);

	ui_section_label(page, _("Accelerometer"));
	card = ui_card_create(page);
	diag.device = settings_info_row(card, _("Device"));
	diag.matrix = settings_info_row(card, _("Mount matrix"));

	ui_section_label(page, _("Raw"));
	card = ui_card_create(page);
	for (int i = 0; i < 3; i++)
		diag.raw[i] = settings_info_row(card, raw[i]);

	ui_section_label(page, _("Turned by the mount matrix"));
	card = ui_card_create(page);
	for (int i = 0; i < 3; i++)
		diag.axis[i] = settings_info_row(card, _(axis[i]));
	diag.up = settings_info_row(card, _("Up"));

	ui_section_label(page, _("Screen rotation"));
	card = ui_card_create(page);
	diag.rotation = settings_info_row(card, _("Turns to"));

	settings_timer(scr, diagnostics_update, 250);
	ui_push(scr);
}
