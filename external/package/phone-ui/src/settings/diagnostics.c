/* Settings, Diagnostics: the frame rate counter, the network's speed, what the sensors read
 * and what phone-ui makes of it */

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
