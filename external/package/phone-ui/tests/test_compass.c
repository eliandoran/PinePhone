/*
 * Compass (compass.c) on the tests' motion sensors (ui_test.h): the heading flat
 * and upright, the phone's own field taken out by calibration, missing sensors.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define DEG "\xc2\xb0"
#define EARTH 0.2f /* gauss, horizontal */
#define DIP 0.4f   /* downward */

TEST_SETUP
{
	ui_test_reset();
}

/* The phone in a pose for `ms`: 5 samples of each sensor per 50 ms */
static void hold(const float up[3], const float field[3], uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 50) {
		ui_test_sensors_feed(up, field, 5);
		ui_test_wait(50);
	}
}

static const float flat[3] = { 0, 0, 1 };
static const float standing[3] = { 0, 1, 0 };

/* Lying flat, its top edge to `deg` */
static void field_flat(float deg, float out[3])
{
	float h = deg * (float)M_PI / 180;
	out[0] = -EARTH * sinf(h);
	out[1] = EARTH * cosf(h);
	out[2] = -DIP;
}

/* Standing, its back to `deg` */
static void field_standing(float deg, float out[3])
{
	float h = deg * (float)M_PI / 180;
	out[0] = -EARTH * sinf(h);
	out[1] = -DIP;
	out[2] = -EARTH * cosf(h);
}

/* The heading shown, within the degree the dial moves by before it follows */
static bool shows_heading(int deg)
{
	for (int d = deg - 1; d <= deg + 1; d++) {
		char text[16];
		snprintf(text, sizeof(text), "%d" DEG, (d + 360) % 360);
		if (ui_test_shows(text))
			return true;
	}
	fprintf(stderr, "  no heading near %d" DEG "\n", deg);
	ui_test_dump();
	return false;
}

static bool open_compass(void)
{
	ui_test_sensors(true, true);
	return ui_test_open("Compass");
}

TEST(no_sensors)
{
	CHECK(ui_test_open("Compass"));
	CHECK(ui_test_shows("No sensor trigger"));
}

TEST(no_magnetometer)
{
	ui_test_sensors(true, false);
	CHECK(ui_test_open("Compass"));
	CHECK(ui_test_shows("No magnetometer"));
	CHECK_STR(ui_test_sensor_attr("iio:device1/buffer0/enable"), "0"); /* closed again */
}

/* The buffers set up for the records it reads, closed when it goes */
TEST(sensors_set_up)
{
	CHECK(open_compass());
	CHECK_STR(ui_test_sensor_attr("iio:device1/buffer0/enable"), "1");
	CHECK_STR(ui_test_sensor_attr("iio:device2/buffer0/enable"), "1");
	CHECK_STR(ui_test_sensor_attr("iio:device2/trigger/current_trigger"), "mpu6050-dev1");
	CHECK_STR(ui_test_sensor_attr("iio:device2/scan_elements/in_timestamp_en"), "0");
	CHECK_STR(ui_test_sensor_attr("iio:device1/scan_elements/in_accel_y_en"), "1");
	CHECK_STR(ui_test_sensor_attr("iio:device1/sampling_frequency"), "100");
	CHECK(ui_test_shows("--")); /* no samples yet */
	CHECK(ui_test_back());
	CHECK_STR(ui_test_sensor_attr("iio:device1/buffer0/enable"), "0");
	CHECK_STR(ui_test_sensor_attr("iio:device2/buffer0/enable"), "0");
}

TEST(heading_lying_flat)
{
	static const struct {
		float deg;
		const char *direction;
	} poses[] = {
		{ 0, "N" }, { 90, "E" }, { 225, "SW" }, { 300, "NW" },
	};
	CHECK(open_compass());
	CHECK(ui_test_shows("Not calibrated: the heading may be far off"));
	for (size_t i = 0; i < sizeof(poses) / sizeof(poses[0]); i++) {
		float field[3];
		field_flat(poses[i].deg, field);
		hold(flat, field, 4000); /* smoothed: ~0.5 s */
		CHECK(shows_heading((int)poses[i].deg));
		CHECK(ui_test_shows(poses[i].direction));
	}
}

/* The direction its back faces */
TEST(heading_standing)
{
	CHECK(open_compass());
	float field[3];
	field_standing(180, field);
	hold(standing, field, 4000);
	CHECK(shows_heading(180));
	CHECK(ui_test_shows("S"));
}

/* Its top edge, and back, pointing up: no heading */
TEST(pointing_up)
{
	CHECK(open_compass());
	const float tilted[3] = { 0, 0.7071f, -0.7071f };
	float field[3];
	field_flat(90, field);
	hold(tilted, field, 2000);
	CHECK(ui_test_shows("--"));
}

/* The phone's own field, as the calibration found it */
static const float iron[3] = { 0.3f, -0.1f, 0.2f };

static void with_iron(float field[3])
{
	for (int i = 0; i < 3; i++)
		field[i] += iron[i];
}

TEST(calibrated_center_taken_out)
{
	ui_test_write(SETTINGS, "compass.center=0.3000 -0.1000 0.2000\n");
	CHECK(open_compass());
	CHECK(ui_test_shows("Magnetic north"));
	float field[3];
	field_flat(90, field);
	with_iron(field);
	hold(flat, field, 4000);
	CHECK(shows_heading(90));
}

TEST(uncalibrated_far_off)
{
	CHECK(open_compass());
	float field[3];
	field_flat(90, field);
	with_iron(field);
	hold(flat, field, 4000);
	for (int d = 80; d <= 100; d++) {
		char text[16];
		snprintf(text, sizeof(text), "%d" DEG, d);
		CHECK(!ui_test_shows(text));
	}
}

/* Directions spread over the sphere (a golden spiral) */
static void direction(unsigned i, unsigned n, float out[3])
{
	float z = 1 - 2 * (i + 0.5f) / n, r = sqrtf(1 - z * z), a = i * 2.39996323f;
	out[0] = r * cosf(a);
	out[1] = r * sinf(a);
	out[2] = z;
}

/* Turned over in every direction: the center found, saved, used */
TEST(calibration)
{
	CHECK(open_compass());
	CHECK(ui_test_tap_text("Calibrate"));
	CHECK(ui_test_shows("Turn the phone over slowly in every direction"));
	CHECK(ui_test_shows("Cancel"));
	for (unsigned i = 0; i < 240 && ui_test_shows("Cancel"); i++) {
		float d[3], field[3];
		direction(i % 60, 60, d);
		for (int k = 0; k < 3; k++)
			field[k] = iron[k] + 0.45f * d[k];
		hold(d, field, 50);
	}
	CHECK(ui_test_shows("Magnetic north"));
	CHECK(ui_test_shows("Calibrate"));
	float c[3] = { 0 };
	FILE *f = fopen(SETTINGS, "r");
	CHECK(f && fscanf(f, "compass.center=%f %f %f", &c[0], &c[1], &c[2]) == 3);
	if (f)
		fclose(f);
	for (int k = 0; k < 3; k++)
		CHECK(fabsf(c[k] - iron[k]) < 0.005f);

	float field[3];
	field_flat(225, field);
	with_iron(field);
	hold(flat, field, 4000);
	CHECK(shows_heading(225));
}

/* Not turned: it does not finish */
TEST(calibration_needs_turning)
{
	CHECK(open_compass());
	CHECK(ui_test_tap_text("Calibrate"));
	float field[3];
	field_flat(0, field);
	hold(flat, field, 12000);
	CHECK(ui_test_shows("Turn the phone over slowly in every direction"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(ui_test_shows("Not calibrated: the heading may be far off"));
	FILE *f = fopen(SETTINGS, "r");
	char buf[256] = "";
	if (f) {
		buf[fread(buf, 1, sizeof(buf) - 1, f)] = '\0';
		fclose(f);
	}
	CHECK(strstr(buf, "compass.center") == NULL);
}

/* Samples that fit no sphere for a minute: it starts over, and goes on */
TEST(calibration_starts_over)
{
	CHECK(open_compass());
	CHECK(ui_test_tap_text("Calibrate"));
	srand(1);
	for (unsigned i = 0; i < 1300; i++) {
		float field[3];
		for (int k = 0; k < 3; k++)
			field[k] = (rand() % 2000 - 1000) / 1000.0f;
		hold(flat, field, 50);
	}
	CHECK(ui_test_shows("Turn the phone over slowly in every direction"));
}
