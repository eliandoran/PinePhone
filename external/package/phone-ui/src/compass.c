/*
 * Compass: the magnetic heading of the phone's top edge on a dial that turns
 * with it, from the magnetometer (AF8133J or LIS3MDL) and the accelerometer
 * (MPU6050), both read from IIO sysfs. The accelerometer gives the tilt, so the
 * heading holds whether the phone lies flat or stands upright (then it is the
 * direction the back faces).
 *
 * Calibration takes the extremes of each axis while the phone is turned over
 * in every direction: their middle is the phone's own field (hard iron), their
 * spread evens out the axes (soft iron, per axis). It is kept in the settings
 * (compass.cal: three offsets in gauss, three scales).
 */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui.h"

#define IIO "/sys/bus/iio/devices"
#define TICK_MS 50
#define DIAL 600         /* dial diameter */
#define R_DOTS 272       /* radius of the ticks */
#define R_NUMBERS 228    /* of the degree numbers */
#define R_CARDINALS 172  /* of N, E, S, W */
#define SMOOTH 0.25f     /* low-pass weight of a new sample */
#define CAL_MIN_MS 8000  /* shortest calibration */
#define CAL_MIN_SPAN 0.3f /* gauss: twice the weakest field on Earth, with margin */
#define CAL_EVEN 0.8f    /* every axis must see this part of the largest span */

#define COLOR_NORTH UI_COLOR_RED

#define TICKS 36
#define NUMBERS 8
#define CARDINALS 4

typedef float vec3[3];

struct sensor {
	int fd[3];      /* x, y, z raw */
	float scale;    /* to gauss (magnetometer); unused for the accelerometer */
	float m[3][3];  /* mount matrix, to the phone's axes */
};

static struct {
	struct sensor mag, acc;
	bool ok;

	lv_obj_t *dial, *heading, *direction, *hint, *button, *progress, *progress_fill;
	lv_obj_t *ticks[TICKS], *numbers[NUMBERS], *cardinals[CARDINALS];
	int32_t number_w[NUMBERS], number_h[NUMBERS], cardinal_w[CARDINALS], cardinal_h[CARDINALS];
	lv_timer_t *timer;

	vec3 a;              /* smoothed gravity (up), phone axes */
	float hc, hs;        /* smoothed heading as a unit vector */
	bool have_sample;
	int shown;           /* heading on the dial, in tenths of degrees; -1: none */

	vec3 offset, gain;   /* calibration */
	bool calibrated;
	bool calibrating;
	vec3 lo, hi;
	uint32_t cal_start;
} cp;

static const char *const cardinal_text[CARDINALS] = { "N", "E", "S", "W" };
static const char *const number_text[NUMBERS] = { "30", "60", "120", "150", "210", "240", "300", "330" };
static const int number_deg[NUMBERS] = { 30, 60, 120, 150, 210, 240, 300, 330 };
static const char *const direction_text[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

/* Sensors */

static bool sensor_open(struct sensor *s, const char *dev, const char *type)
{
	char path[300];
	const char axis[3] = { 'x', 'y', 'z' };
	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/in_%s_%c_raw", dev, type, axis[i]);
		s->fd[i] = open(path, O_RDONLY | O_CLOEXEC);
		if (s->fd[i] < 0) {
			while (i--)
				close(s->fd[i]);
			return false;
		}
	}

	char buf[64];
	snprintf(path, sizeof(path), "%s/in_%s_scale", dev, type);
	s->scale = ui_read_line(path, buf, sizeof(buf)) ? strtof(buf, NULL) : 1;

	/* "a, b, c; d, e, f; g, h, i", per channel type (MPU6050) or for the device
	 * (AF8133J); the identity if there is none */
	memset(s->m, 0, sizeof(s->m));
	s->m[0][0] = s->m[1][1] = s->m[2][2] = 1;
	snprintf(path, sizeof(path), "%s/in_%s_mount_matrix", dev, type);
	bool found = ui_read_line(path, buf, sizeof(buf));
	if (!found) {
		snprintf(path, sizeof(path), "%s/in_mount_matrix", dev);
		found = ui_read_line(path, buf, sizeof(buf));
	}
	float m[9];
	if (found && sscanf(buf, "%f, %f, %f; %f, %f, %f; %f, %f, %f", &m[0], &m[1], &m[2], &m[3],
	                    &m[4], &m[5], &m[6], &m[7], &m[8]) == 9)
		memcpy(s->m, m, sizeof(m));
	return true;
}

static void sensor_close(struct sensor *s)
{
	for (int i = 0; i < 3; i++)
		if (s->fd[i] > 0)
			close(s->fd[i]);
}

/* A sample in the phone's axes, scaled */
static bool sensor_read(struct sensor *s, vec3 out)
{
	float raw[3];
	char buf[24];
	for (int i = 0; i < 3; i++) {
		ssize_t n = pread(s->fd[i], buf, sizeof(buf) - 1, 0);
		if (n <= 0)
			return false;
		buf[n] = '\0';
		raw[i] = strtof(buf, NULL) * s->scale;
	}
	for (int i = 0; i < 3; i++)
		out[i] = s->m[i][0] * raw[0] + s->m[i][1] * raw[1] + s->m[i][2] * raw[2];
	return true;
}

/* The first IIO device with channels of `type` */
static bool sensor_find(struct sensor *s, const char *type)
{
	DIR *d = opendir(IIO);
	if (!d)
		return false;
	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		char dev[280];
		snprintf(dev, sizeof(dev), IIO "/%s", e->d_name);
		found = sensor_open(s, dev, type);
	}
	closedir(d);
	return found;
}

/* Calibration */

static void cal_load(void)
{
	cp.offset[0] = cp.offset[1] = cp.offset[2] = 0;
	cp.gain[0] = cp.gain[1] = cp.gain[2] = 1;
	cp.calibrated = sscanf(ui_setting_get("compass.cal", ""), "%f %f %f %f %f %f", &cp.offset[0],
	                       &cp.offset[1], &cp.offset[2], &cp.gain[0], &cp.gain[1], &cp.gain[2]) == 6;
}

static void cal_save(void)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f %.4f %.4f", cp.offset[0], cp.offset[1],
	         cp.offset[2], cp.gain[0], cp.gain[1], cp.gain[2]);
	ui_setting_set("compass.cal", buf);
}

static void hint_update(void)
{
	if (cp.calibrating)
		lv_label_set_text_static(cp.hint, "Turn the phone over slowly in every direction");
	else if (!cp.calibrated)
		lv_label_set_text_static(cp.hint, "Not calibrated: the heading may be far off");
	else
		lv_label_set_text_static(cp.hint, "Magnetic north");
	lv_label_set_text_static(lv_obj_get_child(cp.button, 0), cp.calibrating ? "Cancel" : "Calibrate");
	lv_obj_set_hidden(cp.progress, !cp.calibrating);
}

static void cal_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	cp.calibrating = !cp.calibrating;
	if (cp.calibrating) {
		for (int i = 0; i < 3; i++) {
			cp.lo[i] = INFINITY;
			cp.hi[i] = -INFINITY;
		}
		cp.cal_start = lv_tick_get();
		lv_obj_set_width(cp.progress_fill, 0);
	}
	hint_update();
}

/* Takes an uncalibrated sample; finishes once the extremes are wide and even enough */
static void cal_sample(const vec3 m)
{
	float span[3], most = 0, least = INFINITY;
	for (int i = 0; i < 3; i++) {
		cp.lo[i] = fminf(cp.lo[i], m[i]);
		cp.hi[i] = fmaxf(cp.hi[i], m[i]);
		span[i] = cp.hi[i] - cp.lo[i];
		most = fmaxf(most, span[i]);
		least = fminf(least, span[i]);
	}

	float even = most < CAL_MIN_SPAN ? 0 : fminf(1, least / (CAL_EVEN * most));
	float time = fminf(1, lv_tick_elaps(cp.cal_start) / (float)CAL_MIN_MS);
	float done = fminf(even, time);
	lv_obj_set_width(cp.progress_fill, (int32_t)(done * lv_obj_get_content_width(cp.progress)));
	if (done < 1)
		return;

	float mean = (span[0] + span[1] + span[2]) / 6;
	for (int i = 0; i < 3; i++) {
		cp.offset[i] = (cp.lo[i] + cp.hi[i]) / 2;
		cp.gain[i] = mean / (span[i] / 2);
	}
	cp.calibrated = true;
	cp.calibrating = false;
	cal_save();
	ui_vibrate(40);
	hint_update();
	printf("compass: calibrated, offset %.3f %.3f %.3f G, gain %.3f %.3f %.3f\n", cp.offset[0],
	       cp.offset[1], cp.offset[2], cp.gain[0], cp.gain[1], cp.gain[2]);
}

/* Heading */

static void cross(const vec3 a, const vec3 b, vec3 out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool normalize(vec3 v)
{
	float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (n < 1e-6f)
		return false;
	for (int i = 0; i < 3; i++)
		v[i] /= n;
	return true;
}

/* Turns the dial so that `deg` is at the top */
static void dial_set(float deg)
{
	int tenths = (int)lroundf(deg * 10) % 3600;
	if (tenths == cp.shown)
		return;
	cp.shown = tenths;

	int whole = (tenths + 5) / 10 % 360;
	ui_set_text_fmt(cp.heading, "%d°", whole);
	lv_label_set_text_static(cp.direction, direction_text[(whole + 22) / 45 % 8]);

	const float c = DIAL / 2.0f;
	float rot = deg * (float)M_PI / 180;
	for (int i = 0; i < TICKS; i++) {
		float t = i * 10 * (float)M_PI / 180 - rot;
		int32_t d = lv_obj_get_width(cp.ticks[i]);
		lv_obj_set_pos(cp.ticks[i], (int32_t)lroundf(c + R_DOTS * sinf(t) - d / 2.0f),
		               (int32_t)lroundf(c - R_DOTS * cosf(t) - d / 2.0f));
	}
	for (int i = 0; i < NUMBERS; i++) {
		float t = number_deg[i] * (float)M_PI / 180 - rot;
		lv_obj_set_pos(cp.numbers[i], (int32_t)lroundf(c + R_NUMBERS * sinf(t) - cp.number_w[i] / 2.0f),
		               (int32_t)lroundf(c - R_NUMBERS * cosf(t) - cp.number_h[i] / 2.0f));
	}
	for (int i = 0; i < CARDINALS; i++) {
		float t = i * 90 * (float)M_PI / 180 - rot;
		lv_obj_set_pos(cp.cardinals[i],
		               (int32_t)lroundf(c + R_CARDINALS * sinf(t) - cp.cardinal_w[i] / 2.0f),
		               (int32_t)lroundf(c - R_CARDINALS * cosf(t) - cp.cardinal_h[i] / 2.0f));
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	vec3 m, a;
	if (!sensor_read(&cp.mag, m) || !sensor_read(&cp.acc, a))
		return;

	if (cp.calibrating)
		cal_sample(m);
	for (int i = 0; i < 3; i++)
		m[i] = (m[i] - cp.offset[i]) * cp.gain[i];

	if (!normalize(a))
		return;
	for (int i = 0; i < 3; i++)
		cp.a[i] = cp.have_sample ? cp.a[i] + SMOOTH * (a[i] - cp.a[i]) : a[i];

	/* East and north in the phone's axes (gravity points up from a resting
	 * accelerometer), then the heading of (0, 1, -1): the top edge when the phone
	 * lies flat, the back when it stands, and in between for any tilt. */
	vec3 up = { cp.a[0], cp.a[1], cp.a[2] }, east, north;
	normalize(up);
	cross(m, up, east);
	if (!normalize(east))
		return;
	cross(up, east, north);
	float e = east[1] - east[2], n = north[1] - north[2];
	float len = sqrtf(e * e + n * n);
	if (len < 0.2f)
		return; /* that direction points almost straight up or down */

	float hc = n / len, hs = e / len;
	if (cp.have_sample) {
		hc = cp.hc + SMOOTH * (hc - cp.hc);
		hs = cp.hs + SMOOTH * (hs - cp.hs);
	}
	cp.hc = hc;
	cp.hs = hs;
	cp.have_sample = true;

	float deg = atan2f(hs, hc) * 180 / (float)M_PI;
	dial_set(deg < 0 ? deg + 360 : deg);
}

/* Screen */

static lv_obj_t *dial_dot(int32_t size, uint32_t color)
{
	lv_obj_t *dot = ui_box(cp.dial);
	lv_obj_set_size(dot, size, size);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
	return dot;
}

static lv_obj_t *dial_label(const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(cp.dial);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (cp.timer)
		lv_timer_delete(cp.timer);
	if (cp.ok) {
		sensor_close(&cp.mag);
		sensor_close(&cp.acc);
	}
	memset(&cp, 0, sizeof(cp));
}

void compass_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Compass", NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(body, false);

	bool mag = sensor_find(&cp.mag, "magn");
	bool acc = mag && sensor_find(&cp.acc, "accel");
	if (!acc) {
		if (mag)
			sensor_close(&cp.mag);
		printf("compass: no %s in " IIO "\n", mag ? "accelerometer" : "magnetometer");
		ui_empty_label(body, mag ? "No accelerometer" : "No magnetometer");
		ui_push(scr);
		return;
	}
	cp.ok = true;
	cp.shown = -1;
	cal_load();

	cp.heading = lv_label_create(body);
	lv_obj_set_style_text_font(cp.heading, &ui_font_48, 0);
	lv_obj_set_style_text_color(cp.heading, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_margin_top(cp.heading, 32, 0);
	lv_label_set_text_static(cp.heading, "--");
	cp.direction = lv_label_create(body);
	lv_obj_set_style_text_font(cp.direction, &ui_font_32, 0);
	lv_obj_set_style_text_color(cp.direction, lv_color_hex(UI_COLOR_DIM), 0);
	lv_label_set_text_static(cp.direction, "");

	/* The mark the heading is read at, above the dial */
	lv_obj_t *mark = ui_box(body);
	lv_obj_set_size(mark, 8, 40);
	lv_obj_set_style_margin_top(mark, 24, 0);
	lv_obj_set_style_radius(mark, 4, 0);
	lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(mark, lv_color_hex(UI_COLOR_TEXT), 0);

	cp.dial = ui_box(body);
	lv_obj_set_size(cp.dial, DIAL, DIAL);
	lv_obj_set_style_margin_top(cp.dial, 8, 0);
	lv_obj_set_style_radius(cp.dial, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(cp.dial, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(cp.dial, lv_color_hex(UI_COLOR_CARD), 0);

	for (int i = 0; i < TICKS; i++)
		cp.ticks[i] = i % 3 ? dial_dot(8, UI_COLOR_DIM) :
		              dial_dot(14, i == 0 ? COLOR_NORTH : UI_COLOR_TEXT);
	for (int i = 0; i < NUMBERS; i++)
		cp.numbers[i] = dial_label(number_text[i], &ui_font_20, UI_COLOR_DIM);
	for (int i = 0; i < CARDINALS; i++)
		cp.cardinals[i] = dial_label(cardinal_text[i], &ui_font_48,
		                             i == 0 ? COLOR_NORTH : UI_COLOR_TEXT);
	lv_obj_t *center = dial_dot(16, UI_COLOR_DIM);
	lv_obj_set_pos(center, DIAL / 2 - 8, DIAL / 2 - 8);

	/* Fills the rest, pushing the hint and the button to the bottom */
	lv_obj_t *space = ui_box(body);
	lv_obj_set_width(space, 1);
	lv_obj_set_flex_grow(space, 1);

	cp.progress = ui_box(body);
	lv_obj_set_size(cp.progress, 400, 12);
	lv_obj_set_style_radius(cp.progress, 6, 0);
	lv_obj_set_style_bg_opa(cp.progress, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(cp.progress, lv_color_hex(UI_COLOR_TRACK), 0);
	lv_obj_set_style_margin_bottom(cp.progress, 20, 0);
	cp.progress_fill = ui_box(cp.progress);
	lv_obj_set_size(cp.progress_fill, 0, LV_PCT(100));
	lv_obj_set_style_radius(cp.progress_fill, 6, 0);
	lv_obj_set_style_bg_opa(cp.progress_fill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(cp.progress_fill, lv_color_hex(UI_COLOR_ACCENT), 0);

	cp.hint = lv_label_create(body);
	lv_obj_set_style_text_font(cp.hint, &ui_font_20, 0);
	lv_obj_set_style_text_color(cp.hint, lv_color_hex(UI_COLOR_DIM), 0);
	cp.button = ui_button(body, 400, 96, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, "Calibrate",
	                      cal_cb, NULL);
	lv_obj_set_style_text_color(lv_obj_get_child(cp.button, 0), lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_margin_top(cp.button, 20, 0);
	lv_obj_set_style_margin_bottom(cp.button, 40, 0);
	hint_update();

	/* Label sizes, to place them by their centers */
	lv_obj_update_layout(scr);
	for (int i = 0; i < NUMBERS; i++) {
		cp.number_w[i] = lv_obj_get_width(cp.numbers[i]);
		cp.number_h[i] = lv_obj_get_height(cp.numbers[i]);
	}
	for (int i = 0; i < CARDINALS; i++) {
		cp.cardinal_w[i] = lv_obj_get_width(cp.cardinals[i]);
		cp.cardinal_h[i] = lv_obj_get_height(cp.cardinals[i]);
	}
	dial_set(0);
	cp.shown = -1;
	lv_label_set_text_static(cp.heading, "--");
	lv_label_set_text_static(cp.direction, "");

	cp.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	ui_push(scr);
}
