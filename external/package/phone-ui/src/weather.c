/*
 * Weather: the conditions now, the next 24 hours and the next 10 days at one
 * place, from Open-Meteo (free, no account) over HTTPS when the phone has
 * internet. The place is searched by name with Open-Meteo's geocoding and kept
 * in the settings (weather.name, weather.lat, weather.lon). The last forecast
 * is kept in /var/lib/weather/forecast and shown until a newer one arrives: the
 * app fetches one when it opens if it is older than REFRESH_S, or on the
 * refresh button. Times are the place's own (Open-Meteo gives its UTC offset).
 *
 * The fonts have no weather symbols: the icons are drawn from round fills
 * (sun, moon, clouds, drops) and LV_SYMBOL_CHARGE for lightning.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "json.h"
#include "ui.h"

#define DIR "/var/lib/weather"
#define CACHE DIR "/forecast"
#define REFRESH_S (30 * 60)
#define HOURS 24
#define DAYS 10
#define RESULTS 10
#define USER_AGENT "phone-ui/1.0 (PinePhone feature phone firmware)"
#define FORECAST_URL "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
	"&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day," \
	"wind_speed_10m,wind_direction_10m,surface_pressure" \
	"&hourly=temperature_2m,weather_code,is_day,precipitation_probability" \
	"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max," \
	"sunrise,sunset,uv_index_max" \
	"&forecast_hours=%d&forecast_days=%d&timezone=auto&timeformat=unixtime"
#define SEARCH_URL "https://geocoding-api.open-meteo.com/v1/search?count=%d" \
	"&language=en&format=json&name=%s"

#define COLOR_SUN 0xffd60a
#define COLOR_MOON 0xd8d8e0
#define COLOR_CRATER 0xb4b4be
#define COLOR_RAIN_CLOUD 0x8e8e96
#define COLOR_DROP 0x64d2ff
#define COLOR_SNOW 0xf2f2f7

/* Downloads: one at a time per screen, libcurl's multi interface polled by a timer */

struct fetch {
	CURLM *multi;
	CURL *curl; /* NULL: idle */
	char *data;
	size_t size;
	lv_timer_t *timer;
	void (*done)(const char *data); /* NULL: failed */
};

static size_t write_cb(char *data, size_t size, size_t n, void *user)
{
	struct fetch *f = user;
	/* One more byte, for the terminating NUL */
	char *p = realloc(f->data, f->size + size * n + 1);
	if (!p)
		return 0;
	memcpy(p + f->size, data, size * n);
	f->data = p;
	f->size += size * n;
	p[f->size] = '\0';
	return size * n;
}

static void fetch_stop(struct fetch *f)
{
	if (f->timer)
		lv_timer_delete(f->timer);
	if (f->curl) {
		curl_multi_remove_handle(f->multi, f->curl);
		curl_easy_cleanup(f->curl);
	}
	if (f->multi)
		curl_multi_cleanup(f->multi);
	free(f->data);
	memset(f, 0, sizeof(*f));
}

static void fetch_tick(lv_timer_t *timer)
{
	struct fetch *f = lv_timer_get_user_data(timer);
	int running, left;
	curl_multi_perform(f->multi, &running);
	CURLMsg *m;
	while ((m = curl_multi_info_read(f->multi, &left))) {
		if (m->msg != CURLMSG_DONE)
			continue;
		long code = 0;
		curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &code);
		if (m->data.result != CURLE_OK)
			printf("weather: %s\n", curl_easy_strerror(m->data.result));
		else if (code != 200)
			printf("weather: HTTP %ld\n", code);
		bool ok = m->data.result == CURLE_OK && code == 200 && f->data;
		/* Stopped first: done() may start the next one */
		char *data = f->data;
		void (*done)(const char *) = f->done;
		f->data = NULL;
		fetch_stop(f);
		done(ok ? data : NULL);
		free(data);
		return;
	}
}

static void fetch_start(struct fetch *f, const char *url, void (*done)(const char *data))
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
	}
	fetch_stop(f);
	f->multi = curl_multi_init();
	f->curl = curl_easy_init();
	if (!f->multi || !f->curl) {
		fetch_stop(f);
		done(NULL);
		return;
	}
	f->done = done;
	curl_easy_setopt(f->curl, CURLOPT_URL, url);
	curl_easy_setopt(f->curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(f->curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(f->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(f->curl, CURLOPT_TIMEOUT, 30L);
	curl_multi_add_handle(f->multi, f->curl);
	f->timer = lv_timer_create(fetch_tick, 50, f);
}

/* Forecasts */

struct forecast {
	long offset; /* the place's UTC offset, seconds */
	struct {
		double temp, feels, wind, pressure;
		int humidity, code, wind_dir;
		bool day;
	} now;
	struct {
		time_t time;
		double temp;
		int code, rain; /* rain: chance in percent */
		bool day;
	} hours[HOURS];
	struct {
		time_t time, sunrise, sunset;
		double max, min, uv;
		int code, rain;
	} days[DAYS];
	int hour_count, day_count;
};

/* Element i of the array `key` of obj */
static struct json *item(const struct json *obj, const char *key, int i)
{
	struct json *a = json_get(obj, key);
	if (!a || a->kind != JSON_ARRAY)
		return NULL;
	struct json *v = a->child;
	while (v && i--)
		v = v->next;
	return v;
}

static int array_length(const struct json *obj, const char *key)
{
	int n = 0;
	struct json *a = json_get(obj, key);
	for (struct json *v = a && a->kind == JSON_ARRAY ? a->child : NULL; v; v = v->next)
		n++;
	return n;
}

static bool forecast_parse(const char *text, struct forecast *f)
{
	struct json *root = json_parse(text);
	struct json *now = json_get(root, "current");
	struct json *hourly = json_get(root, "hourly");
	struct json *daily = json_get(root, "daily");
	if (!now || !hourly || !daily) {
		json_free(root);
		return false;
	}

	memset(f, 0, sizeof(*f));
	f->offset = json_int(json_get(root, "utc_offset_seconds"), 0);
	f->now.temp = json_num(json_get(now, "temperature_2m"), 0);
	f->now.feels = json_num(json_get(now, "apparent_temperature"), f->now.temp);
	f->now.humidity = json_int(json_get(now, "relative_humidity_2m"), 0);
	f->now.code = json_int(json_get(now, "weather_code"), 0);
	f->now.day = json_int(json_get(now, "is_day"), 1);
	f->now.wind = json_num(json_get(now, "wind_speed_10m"), 0);
	f->now.wind_dir = json_int(json_get(now, "wind_direction_10m"), 0);
	f->now.pressure = json_num(json_get(now, "surface_pressure"), 0);

	f->hour_count = LV_MIN(array_length(hourly, "time"), HOURS);
	for (int i = 0; i < f->hour_count; i++) {
		f->hours[i].time = json_int(item(hourly, "time", i), 0);
		f->hours[i].temp = json_num(item(hourly, "temperature_2m", i), 0);
		f->hours[i].code = json_int(item(hourly, "weather_code", i), 0);
		f->hours[i].day = json_int(item(hourly, "is_day", i), 1);
		f->hours[i].rain = json_int(item(hourly, "precipitation_probability", i), 0);
	}
	f->day_count = LV_MIN(array_length(daily, "time"), DAYS);
	for (int i = 0; i < f->day_count; i++) {
		f->days[i].time = json_int(item(daily, "time", i), 0);
		f->days[i].code = json_int(item(daily, "weather_code", i), 0);
		f->days[i].max = json_num(item(daily, "temperature_2m_max", i), 0);
		f->days[i].min = json_num(item(daily, "temperature_2m_min", i), 0);
		f->days[i].rain = json_int(item(daily, "precipitation_probability_max", i), 0);
		f->days[i].sunrise = json_int(item(daily, "sunrise", i), 0);
		f->days[i].sunset = json_int(item(daily, "sunset", i), 0);
		f->days[i].uv = json_num(item(daily, "uv_index_max", i), 0);
	}
	json_free(root);
	return f->day_count > 0;
}

/* A time of the place: strftime() of it */
static void place_time(char *buf, size_t size, const char *fmt, time_t t, long offset)
{
	struct tm tm;
	t += offset;
	gmtime_r(&t, &tm);
	strftime(buf, size, fmt, &tm);
}

/* WMO weather codes */

enum sky { CLEAR, PARTLY, CLOUDY, FOG, DRIZZLE, RAIN, SNOW, THUNDER };

static enum sky sky_of(int code)
{
	if (code <= 1)
		return CLEAR;
	if (code == 2)
		return PARTLY;
	if (code == 3)
		return CLOUDY;
	if (code == 45 || code == 48)
		return FOG;
	if (code >= 51 && code <= 57)
		return DRIZZLE;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return SNOW;
	if (code >= 95)
		return THUNDER;
	return RAIN;
}

static const char *sky_text(int code)
{
	switch (code) {
	case 0: return "Clear";
	case 1: return "Mainly clear";
	case 2: return "Partly cloudy";
	case 3: return "Overcast";
	case 45: return "Fog";
	case 48: return "Freezing fog";
	case 51: return "Light drizzle";
	case 53: return "Drizzle";
	case 55: return "Heavy drizzle";
	case 56: case 57: return "Freezing drizzle";
	case 61: return "Light rain";
	case 63: return "Rain";
	case 65: return "Heavy rain";
	case 66: case 67: return "Freezing rain";
	case 71: return "Light snow";
	case 73: return "Snow";
	case 75: return "Heavy snow";
	case 77: return "Snow grains";
	case 80: return "Light showers";
	case 81: return "Showers";
	case 82: return "Heavy showers";
	case 85: case 86: return "Snow showers";
	case 95: return "Thunderstorm";
	case 96: case 99: return "Thunderstorm with hail";
	default: return "";
	}
}

/* Icons, in a box of s x s */

static lv_obj_t *blob(lv_obj_t *parent, float x, float y, float w, float h, uint32_t color, int32_t s)
{
	lv_obj_t *o = ui_box(parent);
	lv_obj_set_pos(o, lroundf(x * s), lroundf(y * s));
	lv_obj_set_size(o, LV_MAX(lroundf(w * s), 2), LV_MAX(lroundf(h * s), 2));
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

/* Centered on (x, y), d across */
static void sun(lv_obj_t *icon, float x, float y, float d, int32_t s)
{
	float halo = d * 1.36f;
	lv_obj_t *glow = blob(icon, x - halo / 2, y - halo / 2, halo, halo, COLOR_SUN, s);
	lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
	blob(icon, x - d / 2, y - d / 2, d, d, COLOR_SUN, s);
}

static void moon(lv_obj_t *icon, float x, float y, float d, int32_t s)
{
	blob(icon, x - d / 2, y - d / 2, d, d, COLOR_MOON, s);
	blob(icon, x - d * 0.22f, y - d * 0.26f, d * 0.22f, d * 0.22f, COLOR_CRATER, s);
	blob(icon, x + d * 0.08f, y + d * 0.06f, d * 0.16f, d * 0.16f, COLOR_CRATER, s);
	blob(icon, x - d * 0.26f, y + d * 0.14f, d * 0.12f, d * 0.12f, COLOR_CRATER, s);
}

/* w wide, 0.6 w high, from (x, y) */
static void cloud(lv_obj_t *icon, float x, float y, float w, uint32_t color, int32_t s)
{
	blob(icon, x + w * 0.08f, y + w * 0.16f, w * 0.44f, w * 0.44f, color, s);
	blob(icon, x + w * 0.32f, y, w * 0.56f, w * 0.56f, color, s);
	blob(icon, x, y + w * 0.3f, w, w * 0.3f, color, s);
}

static lv_obj_t *icon_create(lv_obj_t *parent, int32_t s, int code, bool day)
{
	lv_obj_t *icon = ui_box(parent);
	lv_obj_set_size(icon, s, s);
	uint32_t light = ui_light ? 0xc8c8d0 : 0xe8e8ee;

	enum sky sky = sky_of(code);
	switch (sky) {
	case CLEAR:
		if (day)
			sun(icon, 0.5f, 0.5f, 0.5f, s);
		else
			moon(icon, 0.5f, 0.5f, 0.56f, s);
		break;
	case PARTLY:
		if (day)
			sun(icon, 0.64f, 0.34f, 0.4f, s);
		else
			moon(icon, 0.64f, 0.32f, 0.42f, s);
		cloud(icon, 0.06f, 0.4f, 0.74f, light, s);
		break;
	case CLOUDY:
		cloud(icon, 0.08f, 0.24f, 0.84f, light, s);
		break;
	case FOG:
		cloud(icon, 0.12f, 0.1f, 0.76f, light, s);
		blob(icon, 0.12f, 0.64f, 0.76f, 0.07f, COLOR_RAIN_CLOUD, s);
		blob(icon, 0.2f, 0.76f, 0.6f, 0.07f, COLOR_RAIN_CLOUD, s);
		break;
	default:
		cloud(icon, 0.1f, 0.08f, 0.8f, sky == SNOW ? light : COLOR_RAIN_CLOUD, s);
		break;
	}

	/* Below the cloud, from 0.62 down */
	switch (sky) {
	case DRIZZLE:
		for (int i = 0; i < 3; i++)
			blob(icon, 0.3f + i * 0.17f, 0.64f + (i % 2) * 0.08f, 0.07f, 0.12f, COLOR_DROP, s);
		break;
	case RAIN:
		for (int i = 0; i < 3; i++)
			blob(icon, 0.3f + i * 0.17f, 0.62f + (i % 2) * 0.06f, 0.08f, 0.22f, COLOR_DROP, s);
		break;
	case SNOW:
		for (int i = 0; i < 3; i++)
			blob(icon, 0.28f + i * 0.17f, 0.66f + (i % 2) * 0.12f, 0.11f, 0.11f,
			     ui_light ? COLOR_DROP : COLOR_SNOW, s);
		break;
	case THUNDER: {
		lv_obj_t *bolt = lv_label_create(icon);
		lv_label_set_text_static(bolt, LV_SYMBOL_CHARGE);
		lv_obj_set_style_text_font(bolt, s >= 160 ? &ui_font_48 : s >= 80 ? &ui_font_32 : &ui_font_20, 0);
		lv_obj_set_style_text_color(bolt, lv_color_hex(COLOR_SUN), 0);
		lv_obj_align(bolt, LV_ALIGN_TOP_MID, 0, s * 0.56f);
		break;
	}
	default:
		break;
	}
	return icon;
}

/* Main screen */

static struct {
	lv_obj_t *body, *status;
	struct fetch fetch;
	struct forecast fc;
	bool have;  /* fc is valid */
	bool failed; /* the last fetch */
	time_t updated;
	char name[64];
	double lat, lon;
	unsigned version;
} wx;

static unsigned place_version; /* bumped when another place is chosen */

static void place_load(void)
{
	snprintf(wx.name, sizeof(wx.name), "%s", ui_setting_get("weather.name", ""));
	wx.lat = atof(ui_setting_get("weather.lat", "0"));
	wx.lon = atof(ui_setting_get("weather.lon", "0"));
	wx.version = place_version;
}

/* The cache: "<lat> <lon> <time>\n" then Open-Meteo's answer, for this place only */

static void cache_save(const char *data)
{
	mkdir(DIR, 0755);
	FILE *f = fopen(CACHE ".new", "w");
	if (!f) {
		printf("weather: %s: %s\n", CACHE, strerror(errno));
		return;
	}
	fprintf(f, "%.4f %.4f %lld\n%s", wx.lat, wx.lon, (long long)wx.updated, data);
	if (fclose(f) == 0)
		rename(CACHE ".new", CACHE);
}

static void cache_load(void)
{
	FILE *f = fopen(CACHE, "r");
	if (!f)
		return;
	double lat, lon;
	long long t;
	if (fscanf(f, "%lf %lf %lld\n", &lat, &lon, &t) == 3 &&
	    fabs(lat - wx.lat) < 1e-3 && fabs(lon - wx.lon) < 1e-3) {
		long start = ftell(f);
		fseek(f, 0, SEEK_END);
		long size = ftell(f) - start;
		char *data = size > 0 ? malloc(size + 1) : NULL;
		fseek(f, start, SEEK_SET);
		if (data && fread(data, 1, size, f) == (size_t)size) {
			data[size] = '\0';
			wx.have = forecast_parse(data, &wx.fc);
			wx.updated = t;
		}
		free(data);
	}
	fclose(f);
}

static void status_update(void)
{
	if (!wx.status)
		return;
	char when[32];
	ui_when_format(when, sizeof(when), wx.updated);
	if (wx.fetch.curl)
		ui_set_text(wx.status, "Updating" "\xe2\x80\xa6");
	else if (wx.failed && wx.have)
		ui_set_text_fmt(wx.status, "No connection, updated %s", when);
	else if (wx.failed)
		ui_set_text(wx.status, "No internet connection");
	else if (wx.have)
		ui_set_text_fmt(wx.status, "Updated %s", when);
	else
		ui_set_text(wx.status, "");
}

/* Of the bars of the days, by their warmest */
static uint32_t temp_color(double t)
{
	return t < 0 ? 0x5ac8fa : t < 10 ? 0x66d4cf : t < 20 ? 0xffd60a : t < 28 ? 0xff9f0a : 0xff453a;
}

static const char *compass(int deg)
{
	static const char *const names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	return names[((deg % 360 + 360) % 360 * 2 + 45) / 90 % 8];
}

static const char *uv_text(double uv)
{
	return uv < 3 ? "low" : uv < 6 ? "moderate" : uv < 8 ? "high" : uv < 11 ? "very high" : "extreme";
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static void hero_create(lv_obj_t *parent)
{
	const struct forecast *f = &wx.fc;
	lv_obj_t *hero = ui_box(parent);
	lv_obj_set_size(hero, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(hero, 8, 0);
	lv_obj_set_style_pad_bottom(hero, 16, 0);

	lv_obj_t *name = ui_one_line_label(hero, wx.name, &ui_font_32);
	lv_obj_set_width(name, LV_PCT(100));
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	wx.status = label(hero, &ui_font_20, UI_COLOR_DIM);
	lv_obj_set_style_margin_top(wx.status, 6, 0);
	status_update();
	if (!wx.have)
		return;

	icon_create(hero, 220, f->now.code, f->now.day);
	lv_obj_t *temp = label(hero, &ui_font_48, UI_COLOR_TEXT);
	ui_set_text_fmt(temp, "%ld°", lround(f->now.temp));
	lv_obj_t *sky = label(hero, &ui_font_26, UI_COLOR_TEXT);
	lv_label_set_text_static(sky, sky_text(f->now.code));
	lv_obj_set_style_margin_top(sky, 8, 0);
	lv_obj_t *range = label(hero, &ui_font_24, UI_COLOR_DIM);
	ui_set_text_fmt(range, "H %ld°   L %ld°", lround(f->days[0].max), lround(f->days[0].min));
	lv_obj_set_style_margin_top(range, 8, 0);
}

static void hours_create(lv_obj_t *parent)
{
	const struct forecast *f = &wx.fc;
	ui_section_label(parent, "Next 24 hours");
	/* The card scrolls sideways; the body still scrolls up and down */
	lv_obj_t *card = ui_card_create(parent);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_hor(card, 12, 0);
	lv_obj_set_style_pad_ver(card, 20, 0);
	lv_obj_set_scrollable(card, true);
	lv_obj_set_scroll_dir(card, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

	for (int i = 0; i < f->hour_count; i++) {
		lv_obj_t *col = ui_box(card);
		lv_obj_set_size(col, 100, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 8, 0);

		lv_obj_t *hour = label(col, &ui_font_20, UI_COLOR_DIM);
		if (i == 0) {
			lv_label_set_text_static(hour, "Now");
		} else {
			char buf[8];
			place_time(buf, sizeof(buf), "%H", f->hours[i].time, f->offset);
			ui_set_text(hour, buf);
		}
		icon_create(col, 64, f->hours[i].code, f->hours[i].day);
		lv_obj_t *rain = label(col, &ui_font_16, COLOR_DROP);
		if (f->hours[i].rain >= 20)
			ui_set_text_fmt(rain, "%d%%", f->hours[i].rain);
		else
			lv_label_set_text_static(rain, "");
		lv_obj_t *temp = label(col, &ui_font_24, UI_COLOR_TEXT);
		ui_set_text_fmt(temp, "%ld°", lround(f->hours[i].temp));
	}
}

static void days_create(lv_obj_t *parent)
{
	const struct forecast *f = &wx.fc;
	ui_section_label(parent, "Next 10 days");
	lv_obj_t *card = ui_card_create(parent);

	/* The bars share one scale: the coldest night to the warmest day */
	double lo = f->days[0].min, hi = f->days[0].max;
	for (int i = 1; i < f->day_count; i++) {
		lo = LV_MIN(lo, f->days[i].min);
		hi = LV_MAX(hi, f->days[i].max);
	}
	double span = hi - lo > 1 ? hi - lo : 1;

	for (int i = 0; i < f->day_count; i++) {
		lv_obj_t *row = ui_row_create(card, 104, i < f->day_count - 1, NULL, NULL);
		lv_obj_set_style_pad_column(row, 12, 0);

		lv_obj_t *day = label(row, &ui_font_26, UI_COLOR_TEXT);
		lv_obj_set_width(day, 96);
		if (i == 0) {
			lv_label_set_text_static(day, "Today");
		} else {
			char buf[16];
			place_time(buf, sizeof(buf), "%a", f->days[i].time, f->offset);
			ui_set_text(day, buf);
		}
		icon_create(row, 56, f->days[i].code, true);
		lv_obj_t *rain = label(row, &ui_font_20, COLOR_DROP);
		lv_obj_set_width(rain, 64);
		if (f->days[i].rain >= 20)
			ui_set_text_fmt(rain, "%d%%", f->days[i].rain);
		else
			lv_label_set_text_static(rain, "");

		lv_obj_t *min = label(row, &ui_font_24, UI_COLOR_DIM);
		lv_obj_set_width(min, 60);
		lv_obj_set_style_text_align(min, LV_TEXT_ALIGN_RIGHT, 0);
		ui_set_text_fmt(min, "%ld°", lround(f->days[i].min));

		lv_obj_t *track = ui_box(row);
		lv_obj_set_height(track, 8);
		lv_obj_set_flex_grow(track, 1);
		lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(track, lv_color_hex(UI_COLOR_TRACK), 0);
		lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
		int32_t x = lround((f->days[i].min - lo) / span * 100);
		int32_t w = lround((f->days[i].max - f->days[i].min) / span * 100);
		lv_obj_t *fill = ui_box(track);
		lv_obj_set_size(fill, LV_PCT(LV_MAX(w, 4)), LV_PCT(100));
		lv_obj_set_x(fill, LV_PCT(LV_MIN(x, 96)));
		lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(fill, lv_color_hex(temp_color(f->days[i].max)), 0);
		lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

		lv_obj_t *max = label(row, &ui_font_24, UI_COLOR_TEXT);
		lv_obj_set_width(max, 60);
		lv_obj_set_style_text_align(max, LV_TEXT_ALIGN_RIGHT, 0);
		ui_set_text_fmt(max, "%ld°", lround(f->days[i].max));
	}
}

static lv_obj_t *detail_row(lv_obj_t *card, const char *title, bool divider)
{
	lv_obj_t *row = ui_row_create(card, 96, divider, NULL, NULL);
	lv_obj_t *t = label(row, &ui_font_26, UI_COLOR_TEXT);
	lv_label_set_text_static(t, title);
	lv_obj_set_flex_grow(t, 1);
	return label(row, &ui_font_26, UI_COLOR_DIM);
}

static void details_create(lv_obj_t *parent)
{
	const struct forecast *f = &wx.fc;
	char buf[16];
	ui_section_label(parent, "Details");
	lv_obj_t *card = ui_card_create(parent);
	ui_set_text_fmt(detail_row(card, "Feels like", true), "%ld°", lround(f->now.feels));
	ui_set_text_fmt(detail_row(card, "Humidity", true), "%d%%", f->now.humidity);
	ui_set_text_fmt(detail_row(card, "Wind", true), "%ld km/h %s", lround(f->now.wind),
	                compass(f->now.wind_dir));
	ui_set_text_fmt(detail_row(card, "Pressure", true), "%ld hPa", lround(f->now.pressure));
	ui_set_text_fmt(detail_row(card, "UV index", true), "%ld, %s", lround(f->days[0].uv),
	                uv_text(f->days[0].uv));
	place_time(buf, sizeof(buf), "%H:%M", f->days[0].sunrise, f->offset);
	ui_set_text(detail_row(card, "Sunrise", true), buf);
	place_time(buf, sizeof(buf), "%H:%M", f->days[0].sunset, f->offset);
	ui_set_text(detail_row(card, "Sunset", false), buf);

	lv_obj_t *credit = ui_dim_label(parent, "Weather data by Open-Meteo.com", &ui_font_16);
	lv_obj_set_width(credit, LV_PCT(100));
	lv_obj_set_style_text_align(credit, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_margin_top(credit, 16, 0);
}

static void search_open(void);

static void place_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	search_open();
}

static void render(void)
{
	lv_obj_clean(wx.body);
	wx.status = NULL;
	if (!wx.name[0]) {
		lv_obj_t *box = ui_box(wx.body);
		lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(box, 40, 0);
		ui_empty_label(box, "No place chosen");
		ui_button(box, 400, 96, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
		          LV_SYMBOL_GPS "  Choose a place", place_cb, NULL);
		return;
	}
	hero_create(wx.body);
	if (!wx.have)
		return;
	hours_create(wx.body);
	days_create(wx.body);
	details_create(wx.body);
}

static void forecast_done(const char *data)
{
	struct forecast f;
	wx.failed = !data || !forecast_parse(data, &f);
	if (wx.failed) {
		status_update();
		return;
	}
	wx.fc = f;
	wx.have = true;
	wx.updated = time(NULL);
	cache_save(data);
	render();
}

static void refresh(void)
{
	if (!wx.name[0])
		return;
	char url[sizeof(FORECAST_URL) + 32];
	snprintf(url, sizeof(url), FORECAST_URL, wx.lat, wx.lon, HOURS, DAYS);
	fetch_start(&wx.fetch, url, forecast_done);
	status_update();
}

static void refresh_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	refresh();
}

/* Back from the search with another place */
static void load_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (wx.version == place_version)
		return;
	fetch_stop(&wx.fetch);
	place_load();
	wx.have = wx.failed = false;
	cache_load();
	render();
	refresh();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	fetch_stop(&wx.fetch);
	memset(&wx, 0, sizeof(wx));
}

void weather_open(void)
{
	lv_obj_t *header;
	lv_obj_t *scr = ui_app_screen_create("Weather", &header, &wx.body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(scr, load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
	ui_header_button(header, LV_SYMBOL_REFRESH, refresh_cb, NULL);
	ui_header_button(header, LV_SYMBOL_GPS, place_cb, NULL);
	lv_obj_set_style_pad_hor(wx.body, 16, 0);
	lv_obj_set_style_pad_row(wx.body, 12, 0);
	lv_obj_set_style_pad_bottom(wx.body, 24, 0);
	lv_obj_set_scroll_dir(wx.body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(wx.body, LV_SCROLLBAR_MODE_OFF);

	place_load();
	cache_load();
	render();
	time_t now = time(NULL);
	if (!wx.have || now - wx.updated > REFRESH_S || now < wx.updated)
		refresh();
	ui_push(scr);
}

/* Place search */

static struct {
	lv_obj_t *ta, *kb, *list;
	struct fetch fetch;
	struct place {
		char name[64], where[96];
		double lat, lon;
	} results[RESULTS];
	int count;
} search;

static void result_cb(lv_event_t *e)
{
	const struct place *p = lv_event_get_user_data(e);
	char buf[32];
	ui_setting_set("weather.name", p->name);
	snprintf(buf, sizeof(buf), "%.4f", p->lat);
	ui_setting_set("weather.lat", buf);
	snprintf(buf, sizeof(buf), "%.4f", p->lon);
	ui_setting_set("weather.lon", buf);
	place_version++;
	ui_pop();
}

static void search_done(const char *data)
{
	lv_obj_clean(search.list);
	search.count = 0;
	struct json *root = json_parse(data);
	struct json *results = json_get(root, "results");
	for (struct json *r = results ? results->child : NULL; r && search.count < RESULTS; r = r->next) {
		struct place *p = &search.results[search.count++];
		const char *admin = json_str(json_get(r, "admin1"), NULL);
		const char *country = json_str(json_get(r, "country"), "");
		snprintf(p->name, sizeof(p->name), "%s", json_str(json_get(r, "name"), ""));
		snprintf(p->where, sizeof(p->where), "%s%s%s", admin && strcmp(admin, p->name) ? admin : "",
		         admin && strcmp(admin, p->name) && country[0] ? ", " : "", country);
		p->lat = json_num(json_get(r, "latitude"), 0);
		p->lon = json_num(json_get(r, "longitude"), 0);
	}
	json_free(root);

	if (!data) {
		ui_empty_label(search.list, "No internet connection");
		return;
	}
	if (!search.count) {
		ui_empty_label(search.list, "No places found");
		return;
	}
	lv_obj_t *card = ui_card_create(search.list);
	for (int i = 0; i < search.count; i++) {
		struct place *p = &search.results[i];
		lv_obj_t *row = ui_row_create(card, 112, i < search.count - 1, result_cb, p);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_set_width(ui_one_line_label(text, p->name, &ui_font_26), LV_PCT(100));
		lv_obj_t *where = ui_one_line_label(text, p->where, &ui_font_20);
		lv_obj_set_width(where, LV_PCT(100));
		lv_obj_set_style_text_color(where, lv_color_hex(UI_COLOR_DIM), 0);
	}
}

static void search_start(void)
{
	const char *text = lv_textarea_get_text(search.ta);
	if (!text[0])
		return;
	char *q = curl_easy_escape(NULL, text, 0);
	if (!q)
		return;
	char url[sizeof(SEARCH_URL) + 3 * 64];
	snprintf(url, sizeof(url), SEARCH_URL, RESULTS, q);
	curl_free(q);
	ui_keyboard_show(search.kb, false);
	lv_obj_clean(search.list);
	ui_empty_label(search.list, "Searching" "\xe2\x80\xa6");
	fetch_start(&search.fetch, url, search_done);
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(search.kb, true);
		break;
	case LV_EVENT_READY:
		search_start();
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(search.kb, false);
		break;
	default:
		break;
	}
}

static void search_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	fetch_stop(&search.fetch);
	memset(&search, 0, sizeof(search));
}

static void search_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Place", NULL, &body);
	lv_obj_add_event_cb(scr, search_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	lv_obj_t *field = ui_box(body);
	lv_obj_set_size(field, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_hor(field, 16, 0);
	lv_obj_set_style_pad_bottom(field, 12, 0);
	search.ta = ui_text_field(field, 88, 60, ta_cb, NULL);
	lv_textarea_set_placeholder_text(search.ta, "City");
	lv_obj_set_width(search.ta, LV_PCT(100));

	search.list = ui_box(body);
	lv_obj_set_width(search.list, LV_PCT(100));
	lv_obj_set_flex_grow(search.list, 1);
	lv_obj_set_flex_flow(search.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(search.list, 16, 0);
	lv_obj_set_style_pad_bottom(search.list, 16, 0);
	lv_obj_set_scrollable(search.list, true);
	lv_obj_set_scroll_dir(search.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(search.list, LV_SCROLLBAR_MODE_OFF);

	search.kb = ui_keyboard_create(body, search.ta);
	ui_keyboard_show(search.kb, true);
	ui_push(scr);
}
