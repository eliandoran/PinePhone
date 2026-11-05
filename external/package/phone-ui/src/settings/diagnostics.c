/* Settings, Diagnostics: the frame rate counter, what the sensors read and what phone-ui
 * makes of it */

#include <math.h>

#include "settings.h"

static struct {
	lv_obj_t *device, *matrix, *raw[3], *axis[3], *up, *rotation;
} diag;

static void diagnostics_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	struct ui_accel a;
	if (!ui_accel_read(&a)) {
		ui_set_text(diag.device, "No accelerometer");
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
		{ "Left edge", "Right edge" },
		{ "Bottom edge", "Top edge" },
		{ "Back (face down)", "Screen (face up)" },
	};
	ui_set_text(diag.up, up[most][v[most] > 0]);
	switch (a.rotation) {
	case LV_DISPLAY_ROTATION_0:   ui_set_text(diag.rotation, "Upright"); break;
	case LV_DISPLAY_ROTATION_90:  ui_set_text(diag.rotation, "Right edge up (90°)"); break;
	case LV_DISPLAY_ROTATION_270: ui_set_text(diag.rotation, "Left edge up (270°)"); break;
	default: ui_set_text(diag.rotation, "Stays (flat, upside down or between)"); break;
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
	lv_obj_t *scr = settings_page("Diagnostics", &page);
	static const char *const raw[3] = { "x", "y", "z" };
	static const char *const axis[3] = { "x (right)", "y (top)", "z (out of the screen)" };

	/* The frame rate and CPU in the bottom right corner, over everything */
	ui_section_label(page, "Performance");
	lv_obj_t *card = ui_card_create(page);
	lv_obj_t *row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, "Show frame rate");
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_setting_get_int("fps_counter", 0));
	lv_obj_add_event_cb(sw, fps_cb, LV_EVENT_VALUE_CHANGED, NULL);

	ui_section_label(page, "Accelerometer");
	card = ui_card_create(page);
	diag.device = settings_info_row(card, "Device");
	diag.matrix = settings_info_row(card, "Mount matrix");

	ui_section_label(page, "Raw");
	card = ui_card_create(page);
	for (int i = 0; i < 3; i++)
		diag.raw[i] = settings_info_row(card, raw[i]);

	ui_section_label(page, "Turned by the mount matrix");
	card = ui_card_create(page);
	for (int i = 0; i < 3; i++)
		diag.axis[i] = settings_info_row(card, axis[i]);
	diag.up = settings_info_row(card, "Up");

	ui_section_label(page, "Screen rotation");
	card = ui_card_create(page);
	diag.rotation = settings_info_row(card, "Turns to");

	settings_timer(scr, diagnostics_update, 250);
	ui_push(scr);
}
