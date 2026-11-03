/*
 * Compass: the magnetic heading of the phone's top edge on a dial that turns
 * with it, from the magnetometer (AF8133J or LIS3MDL) and the accelerometer
 * (MPU6050), both read from IIO sysfs. The accelerometer gives the tilt, so the
 * heading holds whether the phone lies flat or stands upright (then it is the
 * direction the back faces).
 *
 * The phone's own field (hard iron, about as strong as the Earth's) is taken
 * out by calibration: while the phone is turned over in every direction, the
 * samples lie on a sphere around it, fitted by least squares. Taking the middle
 * of each axis' extremes instead was tens of degrees off whenever the turning
 * missed a few directions. The center is kept in the settings (compass.center,
 * gauss in the phone's axes).
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
#define SMOOTH 0.1f      /* low-pass weight of a new sample: ~0.5 s */
#define STEP 1.0f        /* degrees the heading moves before the dial follows */
#define CAL_MIN_MS 8000  /* shortest calibration */
#define CAL_SAMPLES 1200 /* a minute */
#define CAL_MIN_R 0.15f  /* gauss: the Earth's field is 0.25 to 0.65 */
#define CAL_MAX_R 1.0f
#define CAL_FIT 0.15f    /* largest rms distance from the sphere, part of its radius */
#define CAL_OCTANTS 7    /* directions around the center the samples must reach */
#define CAL_PER_OCTANT 3

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
	float shown;         /* heading on the dial; < 0: none */

	vec3 offset;         /* calibration: the phone's own field */
	bool calibrated;
	bool calibrating;
	uint32_t cal_start;
	unsigned cal_n;
	double cal_sums[4][5]; /* normal equations of the sphere fit */
} cp;

static vec3 cal_samples[CAL_SAMPLES];

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
	cp.calibrated = sscanf(ui_setting_get("compass.center", ""), "%f %f %f", &cp.offset[0],
	                       &cp.offset[1], &cp.offset[2]) == 3;
	if (!cp.calibrated)
		cp.offset[0] = cp.offset[1] = cp.offset[2] = 0;
}

static void cal_save(void)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.4f %.4f %.4f", cp.offset[0], cp.offset[1], cp.offset[2]);
	ui_setting_set("compass.center", buf);
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

static void cal_reset(void)
{
	cp.cal_start = lv_tick_get();
	cp.cal_n = 0;
	memset(cp.cal_sums, 0, sizeof(cp.cal_sums));
	lv_obj_set_width(cp.progress_fill, 0);
}

static void cal_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	cp.calibrating = !cp.calibrating;
	if (cp.calibrating)
		cal_reset();
	hint_update();
}

/* The sphere through the samples, |m|² = 2 c·m + d: solves the 4 x 4 normal
 * equations (Gauss-Jordan with partial pivoting). False if they are singular. */
static bool sphere_fit(vec3 center, float *radius)
{
	double a[4][5];
	memcpy(a, cp.cal_sums, sizeof(a));
	for (int i = 0; i < 4; i++) {
		int p = i;
		for (int k = i + 1; k < 4; k++)
			if (fabs(a[k][i]) > fabs(a[p][i]))
				p = k;
		if (fabs(a[p][i]) < 1e-12)
			return false;
		for (int j = 0; j < 5; j++) {
			double t = a[i][j];
			a[i][j] = a[p][j];
			a[p][j] = t;
		}
		for (int k = 0; k < 4; k++) {
			if (k == i)
				continue;
			double f = a[k][i] / a[i][i];
			for (int j = i; j < 5; j++)
				a[k][j] -= f * a[i][j];
		}
	}
	double c[3], r2;
	for (int i = 0; i < 3; i++)
		c[i] = a[i][4] / a[i][i] / 2;
	r2 = a[3][4] / a[3][3] + c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
	if (r2 <= 0)
		return false;
	for (int i = 0; i < 3; i++)
		center[i] = (float)c[i];
	*radius = (float)sqrt(r2);
	return true;
}

/* Takes an uncalibrated sample; finishes once the fitted sphere is plausible,
 * the samples are close to it and reach enough directions around its center */
static void cal_sample(const vec3 m)
{
	if (cp.cal_n < CAL_SAMPLES) {
		memcpy(cal_samples[cp.cal_n++], m, sizeof(vec3));
		double r[5] = { m[0], m[1], m[2], 1, m[0] * m[0] + m[1] * m[1] + m[2] * m[2] };
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 5; j++)
				cp.cal_sums[i][j] += r[i] * r[j];
	}

	vec3 c;
	float radius = 0;
	float time = fminf(1, lv_tick_elaps(cp.cal_start) / (float)CAL_MIN_MS);
	float reach = 0, rms = INFINITY;
	bool ok = cp.cal_n >= 8 * CAL_PER_OCTANT && sphere_fit(c, &radius) &&
	          radius >= CAL_MIN_R && radius <= CAL_MAX_R;
	if (ok) {
		unsigned per_octant[8] = { 0 };
		double sq = 0;
		for (unsigned i = 0; i < cp.cal_n; i++) {
			float *s = cal_samples[i];
			vec3 d = { s[0] - c[0], s[1] - c[1], s[2] - c[2] };
			float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - radius;
			sq += dist * dist;
			per_octant[(d[0] > 0) | (d[1] > 0) << 1 | (d[2] > 0) << 2]++;
		}
		rms = (float)sqrt(sq / cp.cal_n);
		int octants = 0;
		for (int i = 0; i < 8; i++)
			octants += per_octant[i] >= CAL_PER_OCTANT;
		reach = fminf(1, octants / (float)CAL_OCTANTS);
	}
	float done = fminf(reach, time);
	lv_obj_set_width(cp.progress_fill, (int32_t)(done * lv_obj_get_content_width(cp.progress)));
	if (done < 1 || rms > CAL_FIT * radius) {
		if (cp.cal_n == CAL_SAMPLES) /* a minute without a fit: start over */
			cal_reset();
		return;
	}

	memcpy(cp.offset, c, sizeof(vec3));
	cp.calibrated = true;
	cp.calibrating = false;
	cal_save();
	ui_vibrate(40);
	hint_update();
	printf("compass: calibrated from %u samples, center %.3f %.3f %.3f G, field %.3f G, rms %.3f G\n",
	       cp.cal_n, c[0], c[1], c[2], radius, rms);
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
	float moved = fabsf(deg - cp.shown);
	if (cp.shown >= 0 && fminf(moved, 360 - moved) < STEP)
		return;
	cp.shown = deg;

	int whole = (int)lroundf(deg) % 360;
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
		m[i] -= cp.offset[i];

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
