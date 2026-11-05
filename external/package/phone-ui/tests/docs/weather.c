/* docs/apps/weather.md's screenshots (included by test_docs.c): Edinburgh's forecast from
 * Open-Meteo, served by the tests (a made-up but plausible March week) */

#define WEATHER_SEARCH "https://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name="
#define WEATHER_FORECAST "https://api.open-meteo.com/v1/forecast?latitude=55.9521&longitude=-3.1965&*"

/* Open-Meteo's answer for Edinburgh (UTC in March): 24 hours from 09:00, 10 days from today */
static const char *weather_forecast(void)
{
	static char json[8192];
	static const int temps[24] = { 7, 8, 9, 9, 10, 10, 9, 8, 7, 6, 6, 5, 5, 4, 4, 4, 3, 3, 3, 3, 4, 4, 5, 6 };
	static const int codes[24] = { 2, 2, 3, 3, 61, 61, 80, 3, 2, 2, 1, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 61, 61, 3 };
	static const int rain[24] = { 5, 5, 15, 25, 60, 70, 45, 20, 10, 5, 0, 0, 0, 0, 0, 5, 10, 15, 20, 25, 35, 55, 60, 30 };
	static const int day_codes[10] = { 61, 63, 3, 80, 1, 0, 3, 63, 2, 71 };
	static const int maxs[10] = { 10, 9, 8, 11, 12, 13, 10, 8, 9, 6 };
	static const int mins[10] = { 3, 5, 4, 4, 2, 1, 3, 4, 2, -1 };
	static const int day_rain[10] = { 70, 85, 20, 60, 5, 0, 15, 75, 25, 40 };
	time_t hour = NOW - NOW % 3600, day = NOW - NOW % 86400;
	char *p = json;
	p += sprintf(p, "{\"utc_offset_seconds\":0,\"current\":{\"temperature_2m\":7.4,"
	             "\"apparent_temperature\":4.1,\"relative_humidity_2m\":78,\"weather_code\":2,"
	             "\"is_day\":1,\"wind_speed_10m\":19.3,\"wind_direction_10m\":255,"
	             "\"surface_pressure\":1008.2},\"hourly\":{\"time\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(hour + i * 3600));
	p += sprintf(p, "],\"temperature_2m\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d.2", i ? "," : "", temps[i]);
	p += sprintf(p, "],\"weather_code\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", codes[i]);
	p += sprintf(p, "],\"is_day\":[");
	for (int i = 0; i < 24; i++) {
		int h = (9 + i) % 24;
		p += sprintf(p, "%s%d", i ? "," : "", h >= 7 && h <= 18);
	}
	p += sprintf(p, "],\"precipitation_probability\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", rain[i]);
	p += sprintf(p, "]},\"daily\":{\"time\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400));
	p += sprintf(p, "],\"weather_code\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%d", i ? "," : "", day_codes[i]);
	p += sprintf(p, "],\"temperature_2m_max\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%d.4", i ? "," : "", maxs[i]);
	p += sprintf(p, "],\"temperature_2m_min\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%d.1", i ? "," : "", mins[i]);
	p += sprintf(p, "],\"precipitation_probability_max\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%d", i ? "," : "", day_rain[i]);
	p += sprintf(p, "],\"sunrise\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400 + 6 * 3600 + (41 - 2 * i) * 60));
	p += sprintf(p, "],\"sunset\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400 + 18 * 3600 + (12 + 2 * i) * 60));
	p += sprintf(p, "],\"uv_index_max\":[2.2,1.8,2,2.5,3,3.1,2,1.5,2.4,1.2]}}");
	return json;
}

/* Weather opened for the first time, Edinburgh searched for: the results */
static void weather_search(void)
{
	ui_test_serve(WEATHER_SEARCH "Edinburgh", 200, "application/json",
	              "{\"results\":[{\"name\":\"Edinburgh\",\"admin1\":\"Scotland\",\"country\":\"United Kingdom\","
	              "\"latitude\":55.95206,\"longitude\":-3.19648},"
	              "{\"name\":\"Edinburgh\",\"admin1\":\"Indiana\",\"country\":\"United States\","
	              "\"latitude\":39.35422,\"longitude\":-85.96666},"
	              "{\"name\":\"Edinburgh\",\"admin1\":\"Mpumalanga\",\"country\":\"South Africa\","
	              "\"latitude\":-25.47167,\"longitude\":30.86889}]}");
	ui_test_serve(WEATHER_FORECAST, 200, "application/json", weather_forecast());
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_shows("No place chosen"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Choose a place"));
	CHECK(ui_test_shows("Place"));
	CHECK(ui_test_type("Edinburgh\n"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_shows("Scotland, United Kingdom"));
	CHECK(ui_test_shows("Indiana, United States"));
}

/* weather.md: choosing a place */
TEST(weather_place)
{
	weather_search();
	shot("weather-place");
}

/* The place chosen: the forecast */
static void weather_chosen(void)
{
	weather_search();
	CHECK(ui_test_tap_text("Scotland, United Kingdom"));
	ui_test_wait(300);
	ui_test_settle();
	CHECK(ui_test_requested(WEATHER_FORECAST) != NULL);
}

/* weather.md: the conditions now and the next hours */
TEST(weather_forecast)
{
	weather_chosen();
	CHECK(ui_test_shows("Edinburgh"));
	CHECK(ui_test_shows("Updated 09:41"));
	CHECK(ui_test_shows("7\xc2\xb0"));
	CHECK(ui_test_shows("Partly cloudy"));
	CHECK(ui_test_shows("H 10\xc2\xb0   L 3\xc2\xb0"));
	CHECK(ui_test_shows("Next 24 hours"));
	CHECK(ui_test_shows("Now"));
	CHECK(ui_test_shows("13"));
	CHECK(ui_test_shows("60%"));
	shot("weather-forecast");
}

/* weather.md: at the bottom, the details */
TEST(weather_details)
{
	weather_chosen();
	lv_obj_t *details = ui_test_find("Details");
	CHECK(details != NULL);
	if (!details)
		return;
	for (int i = 0; i < 3; i++)
		ui_test_swipe(360, 1200, 360, 300, 400);
	ui_test_wait(1500);
	/* The end, wherever the swipes' momentum left it */
	lv_obj_t *body = lv_obj_get_parent(details);
	lv_obj_scroll_to_y(body, lv_obj_get_scroll_y(body) + lv_obj_get_scroll_bottom(body), LV_ANIM_OFF);
	ui_test_settle();
	CHECK(ui_test_shows("Feels like"));
	CHECK(ui_test_shows("4\xc2\xb0"));
	CHECK(ui_test_shows("78%"));
	CHECK(ui_test_shows("Weather data by Open-Meteo.com"));
	CHECK(ui_test_shows("19 km/h W"));
	CHECK(ui_test_shows("1008 hPa"));
	CHECK(ui_test_shows("2, low"));
	CHECK(ui_test_shows("06:41"));
	CHECK(ui_test_shows("18:12"));
	shot("weather-details");
}
