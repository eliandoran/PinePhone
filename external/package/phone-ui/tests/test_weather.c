/*
 * Weather (weather.c) with Open-Meteo served by the tests (fake_curl.c): choosing a
 * place, the forecast and its cache, offline.
 */

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define CACHE "/var/lib/weather/forecast"
#define PARIS "weather.name=Paris\nweather.lat=48.8534\nweather.lon=2.3488\n"
#define FORECAST "https://api.open-meteo.com/v1/forecast?latitude=48.8534&longitude=2.3488&*"
#define SEARCH_IN(language) "https://geocoding-api.open-meteo.com/v1/search?count=10&language=" language \
	"&format=json&name="
#define SEARCH SEARCH_IN("en")
#define OFFSET 3600 /* Paris in winter */
#define UPDATING "Updating\xe2\x80\xa6"

static char *file(const char *path)
{
	static char buf[4096];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

/* The place's midnight today, as Open-Meteo gives days */
static time_t midnight(void)
{
	time_t now = time(NULL) + OFFSET;
	return now - now % 86400 - OFFSET;
}

/* Open-Meteo's answer: 24 hours from this one, 10 days from today, with every kind
 * of sky (WMO codes) by day and night */
static char *forecast(double temp)
{
	static char json[16384];
	static const int hour_codes[] = { 0, 1, 2, 3, 45, 51, 61, 71, 95, 80, 48, 56 };
	static const int day_codes[] = { 2, 0, 3, 45, 53, 63, 73, 96, 55, 77 };
	time_t hour = time(NULL) - time(NULL) % 3600, day = midnight();
	char *p = json;
	p += sprintf(p, "{\"utc_offset_seconds\":%d,\"current\":{\"temperature_2m\":%.1f,"
	             "\"apparent_temperature\":10.6,\"relative_humidity_2m\":81,\"weather_code\":2,"
	             "\"is_day\":1,\"wind_speed_10m\":14.2,\"wind_direction_10m\":225,"
	             "\"surface_pressure\":1013.4},\"hourly\":{\"time\":[", OFFSET, temp);
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(hour + i * 3600));
	p += sprintf(p, "],\"temperature_2m\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", 10 + i % 5);
	p += sprintf(p, "],\"weather_code\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", hour_codes[i % 12]);
	p += sprintf(p, "],\"is_day\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", i < 12);
	p += sprintf(p, "],\"precipitation_probability\":[");
	for (int i = 0; i < 24; i++)
		p += sprintf(p, "%s%d", i ? "," : "", i == 3 ? 40 : 5);
	p += sprintf(p, "]},\"daily\":{\"time\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400));
	p += sprintf(p, "],\"weather_code\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%d", i ? "," : "", day_codes[i]);
	p += sprintf(p, "],\"temperature_2m_max\":[15.2");
	for (int i = 1; i < 10; i++)
		p += sprintf(p, ",%d", 30 - i * 4);
	p += sprintf(p, "],\"temperature_2m_min\":[7.8");
	for (int i = 1; i < 10; i++)
		p += sprintf(p, ",%d", 20 - i * 4);
	p += sprintf(p, "],\"precipitation_probability_max\":[10,60,0,0,0,0,0,0,0,0],\"sunrise\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400 + 7 * 3600 + 12 * 60));
	p += sprintf(p, "],\"sunset\":[");
	for (int i = 0; i < 10; i++)
		p += sprintf(p, "%s%lld", i ? "," : "", (long long)(day + i * 86400 + 17 * 3600 + 45 * 60));
	p += sprintf(p, "],\"uv_index_max\":[4.6,1,7,9,12,0,0,0,0,0]}}");
	return json;
}

/* Answers arrive at phone-ui's next poll */
static void net_wait(void)
{
	ui_test_wait(200);
	ui_test_settle();
}

static void hhmm(char *buf, size_t size, time_t t)
{
	strftime(buf, size, "%H:%M", localtime(&t));
}

/* Of the place, at the start of its day + seconds */
static void place_day(char *buf, size_t size, int days)
{
	time_t t = midnight() + days * 86400 + OFFSET;
	strftime(buf, size, "%a", gmtime(&t));
}

TEST_SETUP
{
	ui_test_reset();
}

TEST(no_place_chosen)
{
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_shows("No place chosen"));
	CHECK_INT(ui_test_request_count(), 0);
}

TEST(choose_a_place)
{
	ui_test_serve(SEARCH "Paris", 200, "application/json",
	              "{\"results\":[{\"name\":\"Paris\",\"admin1\":\"Ile-de-France\",\"country\":\"France\","
	              "\"latitude\":48.85341,\"longitude\":2.3488},"
	              "{\"name\":\"Paris\",\"admin1\":\"Texas\",\"country\":\"United States\","
	              "\"latitude\":33.66094,\"longitude\":-95.55551},"
	              "{\"name\":\"Paris\",\"admin1\":\"Paris\",\"country\":\"France\","
	              "\"latitude\":48.8566,\"longitude\":2.3522}]}");
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Choose a place"));
	CHECK(ui_test_type("Paris\n"));
	net_wait();
	CHECK(ui_test_shows("Ile-de-France, France"));
	CHECK(ui_test_shows("Texas, United States"));
	CHECK(ui_test_shows("France")); /* the region named as the place: not repeated */

	CHECK(ui_test_tap_text("Ile-de-France, France"));
	net_wait();
	CHECK_STR(file(SETTINGS), PARIS);
	CHECK(ui_test_requested(FORECAST) != NULL);
	CHECK(ui_test_shows("Paris"));
	CHECK(ui_test_shows("12\xc2\xb0"));
	CHECK(ui_test_shows("Partly cloudy"));
	CHECK(strncmp(file(CACHE), "48.8534 2.3488 ", 15) == 0);
}

TEST(search_words_escaped)
{
	ui_test_serve(SEARCH "New%20York", 200, "application/json", "{}");
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Choose a place"));
	CHECK(ui_test_type("New York\n"));
	net_wait();
	CHECK(ui_test_requested(SEARCH "New%20York") != NULL);
	CHECK(ui_test_shows("No places found"));
}

TEST(search_offline)
{
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Choose a place"));
	ui_test_network_hold(true);
	CHECK(ui_test_type("Paris\n"));
	CHECK(ui_test_shows("Searching\xe2\x80\xa6"));
	ui_test_network_hold(false);
	net_wait();
	CHECK(ui_test_shows("No internet connection"));
}

TEST(search_server_error)
{
	ui_test_serve(SEARCH "*", 500, "text/plain", "Internal Server Error");
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Choose a place"));
	CHECK(ui_test_type("Paris\n"));
	net_wait();
	CHECK(ui_test_shows("No internet connection"));
}

TEST(forecast)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	CHECK(ui_test_open("Weather"));
	net_wait();
	char buf[32], when[64];
	hhmm(buf, sizeof(buf), time(NULL));
	snprintf(when, sizeof(when), "Updated %s", buf);
	CHECK(ui_test_shows(when));
	CHECK(ui_test_shows("H 15\xc2\xb0   L 8\xc2\xb0"));
	/* The hours */
	CHECK(ui_test_shows("Now"));
	CHECK(ui_test_shows("40%"));
	CHECK(ui_test_font("40%") == UI_FONT_CAPTION); /* the chance of rain */
	time_t next = time(NULL) - time(NULL) % 3600 + 3600 + OFFSET;
	strftime(buf, sizeof(buf), "%H", gmtime(&next));
	CHECK(ui_test_shows(buf));
	/* The days */
	CHECK(ui_test_shows("Today"));
	place_day(buf, sizeof(buf), 1);
	CHECK(ui_test_shows(buf));
	CHECK(ui_test_shows("60%"));
	/* The details */
	CHECK(ui_test_shows("11\xc2\xb0"));
	CHECK(ui_test_shows("81%"));
	CHECK(ui_test_shows("14 km/h SW"));
	CHECK(ui_test_shows("1013 hPa"));
	CHECK(ui_test_shows("5, moderate"));
	CHECK(ui_test_shows("07:12"));
	CHECK(ui_test_shows("17:45"));
	CHECK(ui_test_shows("Weather data by Open-Meteo.com"));
}

TEST(updating_then_updated)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	ui_test_network_hold(true);
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_shows(UPDATING));
	ui_test_network_hold(false);
	net_wait();
	CHECK(!ui_test_shows(UPDATING));
	CHECK(ui_test_shows("Partly cloudy"));
}

TEST(offline_without_a_forecast)
{
	ui_test_write(SETTINGS, PARIS);
	CHECK(ui_test_open("Weather"));
	net_wait();
	CHECK(ui_test_shows("No internet connection"));
	CHECK(!ui_test_shows("Now"));
}

TEST(not_a_forecast)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve(FORECAST, 200, "application/json", "{\"error\":true,\"reason\":\"Nope\"}");
	CHECK(ui_test_open("Weather"));
	net_wait();
	CHECK(ui_test_shows("No internet connection"));
}

static void cache_write(const char *where, time_t updated, double temp)
{
	char buf[16384 + 64];
	snprintf(buf, sizeof(buf), "%s %lld\n%s", where, (long long)updated, forecast(temp));
	ui_test_write(CACHE, buf);
}

/* The last forecast, said to be old */
TEST(offline_with_the_cached_forecast)
{
	ui_test_write(SETTINGS, PARIS);
	time_t updated = time(NULL) - 2 * 3600;
	cache_write("48.8534 2.3488", updated, 9.6);
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_shows("10\xc2\xb0"));
	net_wait();
	char buf[16], when[64];
	hhmm(buf, sizeof(buf), updated);
	snprintf(when, sizeof(when), "No connection, updated %s", buf);
	CHECK(ui_test_shows(when));
	CHECK(ui_test_shows("10\xc2\xb0"));
}

/* Recent: not fetched again until the refresh button */
TEST(fresh_cache_not_refetched)
{
	ui_test_write(SETTINGS, PARIS);
	cache_write("48.8534 2.3488", time(NULL) - 60, 9.6);
	ui_test_serve(FORECAST, 200, "application/json", forecast(14));
	CHECK(ui_test_open("Weather"));
	net_wait();
	CHECK_INT(ui_test_request_count(), 0);
	CHECK(ui_test_shows("10\xc2\xb0"));
	CHECK(ui_test_tap_text(LV_SYMBOL_REFRESH));
	net_wait();
	CHECK_INT(ui_test_request_count(), 1);
	CHECK(ui_test_shows("14\xc2\xb0"));
}

TEST(cache_of_another_place_ignored)
{
	ui_test_write(SETTINGS, PARIS);
	cache_write("51.5085 -0.1257", time(NULL) - 60, 9.6);
	CHECK(ui_test_open("Weather"));
	net_wait();
	CHECK(!ui_test_shows("10\xc2\xb0"));
	CHECK(ui_test_requested(FORECAST) != NULL);
}

TEST(timeout)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve_error(FORECAST, CURLE_OPERATION_TIMEDOUT);
	CHECK(ui_test_open("Weather"));
	net_wait();
	CHECK(ui_test_shows("No internet connection"));
}

/* Closed while it loads: nothing arrives in the freed screen */
TEST(closed_while_loading)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	ui_test_network_hold(true);
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_back());
	ui_test_network_hold(false);
	net_wait();
	CHECK(ui_at_home());
	CHECK(!ui_test_shows("Partly cloudy"));
	CHECK(!file(CACHE)[0]);
}

/* Another place chosen: its forecast and cache replace the first one's */
TEST(another_place)
{
	ui_test_write(SETTINGS, PARIS);
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	ui_test_serve(SEARCH "London", 200, "application/json",
	              "{\"results\":[{\"name\":\"London\",\"admin1\":\"England\",\"country\":\"United Kingdom\","
	              "\"latitude\":51.50853,\"longitude\":-0.12574}]}");
	ui_test_serve("https://api.open-meteo.com/v1/forecast?latitude=51.5085&longitude=-0.1257&*", 200,
	              "application/json", forecast(6.2));
	ui_test_network_hold(true);
	CHECK(ui_test_open("Weather"));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS));
	ui_test_network_hold(false);
	CHECK(ui_test_type("London\n"));
	net_wait();
	CHECK(ui_test_tap_text("England, United Kingdom"));
	net_wait();
	CHECK(ui_test_shows("London"));
	CHECK(ui_test_shows("6\xc2\xb0"));
	CHECK(strncmp(file(CACHE), "51.5085 -0.1257 ", 16) == 0);
}

/* In Romanian: the forecast, its words and units, and places searched in Romanian */
TEST(romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	ui_test_wall_clock(1773481260); /* Saturday 14 March 2026, 09:41 UTC */
	CHECK(ui_test_open("Vreme"));
	CHECK(ui_test_shows("Niciun loc ales"));
	ui_test_serve(SEARCH_IN("ro") "Bucuresti", 200, "application/json",
	              "{\"results\":[{\"name\":\"Paris\",\"admin1\":\"Île-de-France\",\"country\":\"Franța\","
	              "\"latitude\":48.8534,\"longitude\":2.3488}]}");
	ui_test_serve(FORECAST, 200, "application/json", forecast(12.4));
	CHECK(ui_test_tap_text(LV_SYMBOL_GPS "  Alege un loc"));
	CHECK(ui_test_type("Bucuresti\n"));
	net_wait();
	CHECK(ui_test_tap_text("Île-de-France, Franța"));
	net_wait();
	CHECK(ui_test_shows("Actualizat 09:41"));
	CHECK(ui_test_shows("Parțial noros"));
	CHECK(ui_test_shows("Max. 15\xc2\xb0   Min. 8\xc2\xb0"));
	CHECK(ui_test_shows("Următoarele 24 de ore"));
	CHECK(ui_test_shows("Acum"));
	CHECK(ui_test_shows("Următoarele 10 zile"));
	CHECK(ui_test_shows("Azi"));
	CHECK(ui_test_shows("dum."));
	CHECK(ui_test_shows("Se simte ca"));
	CHECK(ui_test_shows("Umiditate"));
	CHECK(ui_test_shows("14 km/h SV"));
	CHECK(ui_test_shows("5, moderat"));
	CHECK(ui_test_shows("Răsărit"));
	CHECK(ui_test_shows("Apus"));
	CHECK(ui_test_shows("Date meteo de la Open-Meteo.com"));
}
