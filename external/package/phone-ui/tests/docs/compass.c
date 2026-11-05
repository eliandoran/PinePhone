/* docs/apps/compass.md's screenshots (included by test_docs.c) */

#include <math.h>

/* The phone in a pose for `ms`: 5 samples of each sensor per 50 ms */
static void compass_hold(const float up[3], const float field[3], uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 50) {
		ui_test_sensors_feed(up, field, 5);
		ui_test_wait(50);
	}
}

/* Lying flat, its top edge to `deg`: the Earth's field as in Europe (gauss) */
static void compass_field_flat(float deg, float out[3])
{
	float h = deg * (float)M_PI / 180;
	out[0] = -0.2f * sinf(h);
	out[1] = 0.2f * cosf(h);
	out[2] = -0.4f;
}

/* compass.md: calibrated, lying flat, the top edge to the north-east */
TEST(compass_heading)
{
	ui_setting_set("compass.center", "0.0000 0.0000 0.0000"); /* calibrated before */
	ui_test_sensors(true, true);
	CHECK(ui_test_open("Compass"));
	CHECK(ui_test_shows("--")); /* no heading before the first samples */
	const float flat[3] = { 0, 0, 1 };
	float field[3];
	compass_field_flat(52, field);
	compass_hold(flat, field, 4000);
	CHECK(ui_test_shows("52\xc2\xb0") || ui_test_shows("51\xc2\xb0") || ui_test_shows("53\xc2\xb0"));
	CHECK(ui_test_shows("NE"));
	CHECK(ui_test_shows("Magnetic north"));
	CHECK(ui_test_shows("Calibrate"));
	shot("compass-heading");
}

/* compass.md: calibrating, three holes filled */
TEST(compass_calibration)
{
	ui_test_sensors(true, true);
	CHECK(ui_test_open("Compass"));
	CHECK(ui_test_shows("Not calibrated: the heading may be far off"));
	CHECK(ui_test_tap_text("Calibrate"));
	CHECK(ui_test_shows("0 of 8"));
	CHECK(ui_test_shows("Roll the ball into the blue circle"));
	CHECK(ui_test_shows("Cancel"));
	/* Three corners down in turn, the screen tilted up */
	for (int corner = 0; corner < 3; corner++) {
		const float up[3] = { corner & 1 ? -0.5f : 0.5f, corner & 2 ? -0.5f : 0.5f, 0.71f };
		float field[3];
		for (int k = 0; k < 3; k++)
			field[k] = 0.45f * up[k];
		compass_hold(up, field, 300);
	}
	/* Then on the way to the next one */
	const float tilt[3] = { 0.2f, -0.3f, 0.93f };
	float field[3];
	for (int k = 0; k < 3; k++)
		field[k] = 0.45f * tilt[k];
	compass_hold(tilt, field, 300);
	CHECK(ui_test_shows("3 of 8"));
	CHECK(ui_test_find_all(LV_SYMBOL_OK, NULL, 0) == 3);
	CHECK(ui_test_shows("Roll the ball into the blue circle"));
	shot("compass-calibration");
}
