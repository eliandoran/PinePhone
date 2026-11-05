/* Settings, Battery: charge, the last day, details with the thermistor's temperature */

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"

static bool battery_read(int *capacity, char *status, size_t size)
{
	long v;
	if (!ui_read_long(UI_BATTERY "/capacity", &v) || !ui_battery_present())
		return false;
	*capacity = (int)v;
	ui_battery_status(status, size);
	return true;
}

void battery_summary(lv_obj_t *value)
{
	int capacity;
	char status[32];
	if (battery_read(&capacity, status, sizeof(status)))
		ui_set_text_fmt(value, "%d%%", capacity);
	else
		ui_set_text(value, "None");
}

/* The USB input (axp20x_usb_power) and the USB-C port (the ANX7688's typec port) */
#define USB_POWER "/sys/class/power_supply/axp20x-usb"
#define TYPEC_PORT "/sys/class/typec/port0"
#define UDC "/sys/class/udc"

/* A host configured the USB gadget: a computer, not a charger */
static bool usb_host_attached(void)
{
	DIR *dir = opendir(UDC);
	struct dirent *ent;
	char path[300], state[32];
	bool configured = false;
	while (dir && !configured && (ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), UDC "/%s/state", ent->d_name);
		configured = ui_read_line(path, state, sizeof(state)) && strcmp(state, "configured") == 0;
	}
	if (dir)
		closedir(dir);
	return configured;
}

/* "Computer · USB-C PD · 3 A": what powers the phone, what the USB-C port advertises
 * and the input current limit the driver set from it */
static void charger_text(lv_obj_t *label)
{
	long online, limit;
	char mode[32] = "";
	if (!ui_read_long(USB_POWER "/online", &online)) {
		ui_set_text(label, "Unknown");
		return;
	}
	if (!online) {
		ui_set_text(label, "Not connected");
		return;
	}
	/* Without a USB-C partner, or at USB-C's default current: plain USB */
	const char *kind = "USB";
	if (access(TYPEC_PORT "-partner", F_OK) == 0 &&
	    ui_read_line(TYPEC_PORT "/power_operation_mode", mode, sizeof(mode)))
		kind = strcmp(mode, "usb_power_delivery") == 0 ? "USB-C PD" :
		       strcmp(mode, "default") == 0 ? "USB" : "USB-C";
	const char *source = usb_host_attached() ? "Computer" : "Charger";
	if (!ui_read_long(USB_POWER "/input_current_limit", &limit))
		ui_set_text_fmt(label, "%s \xc2\xb7 %s", source, kind);
	else if (limit >= 1000000)
		ui_set_text_fmt(label, "%s \xc2\xb7 %s \xc2\xb7 %g A", source, kind, limit / 1e6);
	else
		ui_set_text_fmt(label, "%s \xc2\xb7 %s \xc2\xb7 %ld mA", source, kind, limit / 1000);
}

/* The last day of the battery's history (battery.c), a bar per SLOT_S seconds: the
 * level at the slot's end, drawn straight between samples (phone-ui does not run while the
 * phone sleeps) unless they are more than HISTORY_GAP apart (it was off), up to now */
#define HISTORY_SLOTS 96
#define SLOT_S 900
#define HISTORY_GAP (12 * 3600)
#define HISTORY_H 200 /* the bars' height at 100% */
#define HISTORY_AXIS 80 /* the levels' labels, right of the bars */
#define HISTORY_HOURS 5
#define COLOR_CHARGING 0x34c759
/* Below the bars, the times the screen was on (battery.c) */
#define STRIP_Y (HISTORY_H + 22)
#define STRIP_H 8
#define STRIP_SEGMENTS 64
#define HOURS_Y (STRIP_Y + STRIP_H + 14)

static struct {
	lv_obj_t *capacity, *status, *icon;
	lv_obj_t *health, *voltage, *current, *charger, *technology, *temperature;
	lv_obj_t *history, *history_line[3], *history_bar[HISTORY_SLOTS], *history_hour[HISTORY_HOURS];
	lv_obj_t *strip, *strip_segment[STRIP_SEGMENTS];
	lv_obj_t *since_label, *since_card, *screen_on, *asleep;
	int history_ticks;
} bat;

/* The battery's thermistor, on the power chip's TS pin. The PinePhone's battery has a
 * 3 kΩ NTC (B about 3880, from the resistances in Megi's kernel patch "axp20x_battery:
 * Setup thermal regulation"). The AXP803 feeds it a current and compares the voltage with
 * thresholds that by default assume a 10 kΩ one; its registers are only in debugfs. */
#define PMIC_REGS "/sys/kernel/debug/regmap/sunxi-rsb-3a3/registers"
#define NTC_R25 3000.0
#define NTC_B 3880.0

/* Registers 0x00-0xff as debugfs lists them, -1 for those it leaves out */
static bool pmic_regs(int regs[256])
{
	FILE *f = fopen(PMIC_REGS, "r");
	if (!f)
		return false;
	for (int i = 0; i < 256; i++)
		regs[i] = -1;
	char line[32];
	unsigned int r, v;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%x: %x", &r, &v) == 2 && r < 256)
			regs[r] = v;
	fclose(f);
	return true;
}

static bool ntc_celsius(double mv, double ua, double *c)
{
	if (mv <= 0 || ua <= 0)
		return false;
	*c = 1.0 / (1.0 / 298.15 + log(mv / ua * 1000.0 / NTC_R25) / NTC_B) - 273.15;
	return true;
}

/* The TS current: 0x84 bits 5:4, 20 µA steps (80 µA by default, or without the registers) */
static double ts_ua(const int *regs)
{
	return regs && regs[0x84] >= 0 ? ((regs[0x84] >> 4 & 3) + 1) * 20 : 80;
}

/* A threshold register: 12.8 mV per step */
static bool threshold_celsius(const int *regs, int reg, long *c)
{
	double d;
	if (regs[reg] <= 0 || !ntc_celsius(regs[reg] * 12.8, ts_ua(regs), &d))
		return false;
	*c = lround(d);
	return true;
}

/* In whole degrees, read by the PMIC's ADC (regs: the PMIC's registers, or NULL) */
static bool thermistor_celsius(const int *regs, long *c)
{
	long raw;
	double scale, d;
	const char *adc = settings_pmic_adc();
	if (!settings_attr_double(adc, "in_voltage0_scale", &scale) || scale <= 0 ||
	    !settings_attr_long(adc, "in_voltage0_raw", &raw) || !ntc_celsius(raw * scale, ts_ua(regs), &d))
		return false;
	*c = lround(d);
	return true;
}

bool battery_temperature(long *c)
{
	int regs[256];
	return thermistor_celsius(pmic_regs(regs) ? regs : NULL, c);
}

/* "20 °C, charges -18 to 14 °C": the limits of 0x38 and 0x39 (a higher voltage is colder);
 * 0x84 bit 2 makes the pin a plain ADC input, the chip ignores the temperature */
static void battery_temp_update(void)
{
	int regs[256];
	bool have_regs = pmic_regs(regs) && regs[0x84] >= 0;
	long c, lo, hi;
	if (!thermistor_celsius(have_regs ? regs : NULL, &c))
		ui_set_text(bat.temperature, "Unknown");
	else if (have_regs && regs[0x84] & 4)
		ui_set_text_fmt(bat.temperature, "%ld °C, no charging limits", c);
	else if (have_regs && threshold_celsius(regs, 0x38, &lo) && threshold_celsius(regs, 0x39, &hi))
		ui_set_text_fmt(bat.temperature, "%ld °C, charges %ld to %ld °C", c, lo, hi);
	else
		ui_set_text_fmt(bat.temperature, "%ld °C", c);
}

static lv_obj_t *chart_bar(lv_obj_t *parent, int32_t w, int32_t h, int32_t y, uint32_t color)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, w, h);
	lv_obj_set_y(bar, y);
	lv_obj_set_style_radius(bar, 3, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
	return bar;
}

static lv_obj_t *chart_text(lv_obj_t *parent, int32_t y, uint32_t color, bool centered)
{
	lv_obj_t *label = lv_label_create(parent);
	ui_set_text(label, "");
	lv_obj_set_y(label, y);
	lv_obj_set_style_text_font(label, &ui_font_20, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	if (centered)
		lv_obj_set_style_translate_x(label, LV_PCT(-50), 0);
	return label;
}

static int sample_cmp(const void *a, const void *b)
{
	const struct battery_sample *x = a, *y = b;
	return (x->time > y->time) - (x->time < y->time);
}

static void history_update(void)
{
	static struct battery_sample s[2048];
	int n = battery_log_read(s, (int)(sizeof(s) / sizeof(s[0])) - 1);
	time_t now = time(NULL);
	int capacity;
	char status[32];
	if (battery_read(&capacity, status, sizeof(status)))
		s[n++] = (struct battery_sample){ now, capacity, ui_battery_plugged(status) };
	if (n > 1)
		qsort(s, (size_t)n, sizeof(s[0]), sample_cmp);

	lv_obj_update_layout(bat.history);
	int32_t w = lv_obj_get_content_width(bat.history) - HISTORY_AXIS;
	for (int i = 0; i < 3; i++)
		lv_obj_set_width(bat.history_line[i], w);
	time_t end = now / SLOT_S * SLOT_S + SLOT_S, start = end - HISTORY_SLOTS * SLOT_S;
	int j = 0;
	for (int i = 0; i < HISTORY_SLOTS; i++) {
		lv_obj_t *bar = bat.history_bar[i];
		time_t t = start + (time_t)(i + 1) * SLOT_S;
		if (t > now)
			t = now;
		/* s[j]: the last sample at t or before, s[j + 1] the next */
		while (j + 1 < n && s[j + 1].time <= t)
			j++;
		bool last = j + 1 >= n;
		bool shown = n > 0 && s[j].time <= t && (last ? s[j].time == t :
		             s[j + 1].time - s[j].time <= HISTORY_GAP);
		lv_obj_set_hidden(bar, !shown);
		if (!shown)
			continue;
		double level = last ? s[j].level : s[j].level + (double)(s[j + 1].level - s[j].level) *
		               (double)(t - s[j].time) / (double)(s[j + 1].time - s[j].time);
		int32_t h = (int32_t)lround(level * HISTORY_H / 100);
		if (h < 2)
			h = 2;
		int32_t x = w * i / HISTORY_SLOTS;
		lv_obj_set_pos(bar, x, HISTORY_H - h);
		lv_obj_set_size(bar, LV_MAX(w * (i + 1) / HISTORY_SLOTS - x - 2, 2), h);
		lv_obj_set_style_bg_color(bar, lv_color_hex(s[j].charging ? COLOR_CHARGING :
		                          level < 15 ? UI_COLOR_RED : UI_COLOR_ACCENT), 0);
	}

	/* The screen on, at least 2 px; periods that meet on screen merged, the last segment
	 * stretched once there are no more */
	static struct screen_period p[512];
	int np = battery_screen_read(p, 512), seg = 0;
	time_t span = end - start;
	lv_obj_set_width(bat.strip, w);
	int32_t prev_x1 = 0, prev_x2 = INT32_MIN;
	for (int i = 0; i < np; i++) {
		if (p[i].end < start || p[i].start > now)
			continue;
		time_t a = LV_MAX(p[i].start, start), b = LV_MIN(p[i].end, now);
		int32_t x1 = (int32_t)(w * (a - start) / span), x2 = (int32_t)(w * (b - start) / span);
		x2 = LV_MAX(x2, x1 + 2);
		if (seg && (x1 <= prev_x2 + 1 || seg == STRIP_SEGMENTS)) {
			x1 = prev_x1;
			seg--;
		}
		lv_obj_set_pos(bat.strip_segment[seg], x1, 0);
		lv_obj_set_width(bat.strip_segment[seg], x2 - x1);
		lv_obj_set_hidden(bat.strip_segment[seg++], false);
		prev_x1 = x1;
		prev_x2 = x2;
	}
	for (int i = seg; i < STRIP_SEGMENTS; i++)
		lv_obj_set_hidden(bat.strip_segment[i], true);

	/* Every 6 hours of the clock */
	int k = 0;
	for (int i = 0; i <= HISTORY_SLOTS && k < HISTORY_HOURS; i++) {
		time_t b = start + (time_t)i * SLOT_S;
		struct tm tm;
		localtime_r(&b, &tm);
		int32_t x = w * i / HISTORY_SLOTS;
		if (tm.tm_min != 0 || tm.tm_hour % 6 != 0 || x < 32 || x > w - 32)
			continue;
		char text[8];
		strftime(text, sizeof(text), "%H:%M", &tm);
		ui_set_text(bat.history_hour[k], text);
		lv_obj_set_x(bat.history_hour[k], x);
		lv_obj_set_hidden(bat.history_hour[k++], false);
	}
	for (; k < HISTORY_HOURS; k++)
		lv_obj_set_hidden(bat.history_hour[k], true);
}

/* The levels' lines and labels, the bars, the hours below */
static void history_create(lv_obj_t *card)
{
	lv_obj_t *chart = bat.history = ui_box(card);
	lv_obj_set_size(chart, LV_PCT(100), HOURS_Y + 44);
	lv_obj_set_style_pad_top(chart, 14, 0); /* the 100% label, centered on its line */
	lv_obj_set_scrollable(chart, false);
	static const char *const levels[] = { "100%", "50%", "0%" };
	for (int i = 0; i < 3; i++) {
		int32_t y = (HISTORY_H - 1) * i / 2;
		bat.history_line[i] = chart_bar(chart, 0, 1, y, UI_COLOR_TRACK);
		lv_obj_set_style_radius(bat.history_line[i], 0, 0);
		lv_obj_t *label = chart_text(chart, 0, UI_COLOR_DIM, false);
		ui_set_text(label, levels[i]);
		lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, y - 12);
	}
	for (int i = 0; i < HISTORY_SLOTS; i++) {
		bat.history_bar[i] = chart_bar(chart, 2, 2, 0, UI_COLOR_ACCENT);
		lv_obj_set_style_radius(bat.history_bar[i], 2, 0);
	}
	bat.strip = chart_bar(chart, 0, STRIP_H, STRIP_Y, UI_COLOR_TRACK);
	lv_obj_set_style_radius(bat.strip, 2, 0);
	for (int i = 0; i < STRIP_SEGMENTS; i++) {
		bat.strip_segment[i] = chart_bar(bat.strip, 2, STRIP_H, 0, UI_COLOR_ACCENT);
		lv_obj_set_style_radius(bat.strip_segment[i], 2, 0);
		lv_obj_set_hidden(bat.strip_segment[i], true);
	}
	lv_obj_t *label = chart_text(chart, 0, UI_COLOR_DIM, false);
	ui_set_text(label, "Screen");
	lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, STRIP_Y + STRIP_H / 2 - 12);
	for (int i = 0; i < HISTORY_HOURS; i++)
		bat.history_hour[i] = chart_text(chart, HOURS_Y, UI_COLOR_DIM, true);
}

static void battery_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	char buf[64];
	long v;
	int capacity;
	char full[32];

	if (!battery_read(&capacity, buf, sizeof(buf))) {
		ui_set_text(bat.capacity, "--");
		/* The AXP803 reports a dead cell as present */
		ui_set_text(bat.status, ui_battery_present() ? "No battery driver" :
		            ui_read_long(UI_BATTERY "/present", &v) && v ? "Dead battery" : "No battery");
	} else {
		ui_set_text_fmt(bat.capacity, "%d%%", capacity);
		if (strcmp(buf, "Charging") == 0 && ui_battery_time_to_full(full, sizeof(full)))
			ui_set_text_fmt(bat.status, "%s \xc2\xb7 full in about %s", buf, full);
		else
			ui_set_text(bat.status, buf[0] ? buf : "Unknown");
	}
	ui_battery_icon_update(bat.icon);

	ui_set_text(bat.health, ui_read_line(UI_BATTERY "/health", buf, sizeof(buf)) ? buf : "Unknown");
	ui_set_text(bat.technology, ui_read_line(UI_BATTERY "/technology", buf, sizeof(buf)) ? buf : "Unknown");
	if (ui_read_long(UI_BATTERY "/voltage_now", &v))
		ui_set_text_fmt(bat.voltage, "%.2f V", v / 1e6);
	else
		ui_set_text(bat.voltage, "Unknown");
	/* Rounded to 10 mA: each distinct text is kept */
	if (ui_read_long(UI_BATTERY "/current_now", &v))
		ui_set_text_fmt(bat.current, "%ld mA", (v / 1000 + (v < 0 ? -5 : 5)) / 10 * 10);
	else
		ui_set_text(bat.current, "Unknown");
	/* Since unplugged: hidden while plugged in */
	long screen_on, asleep;
	bool since = ui_battery_since_unplugged(&screen_on, &asleep);
	lv_obj_set_hidden(bat.since_label, !since);
	lv_obj_set_hidden(bat.since_card, !since);
	if (since) {
		ui_battery_duration(buf, sizeof(buf), screen_on);
		ui_set_text(bat.screen_on, buf);
		ui_battery_duration(buf, sizeof(buf), asleep);
		ui_set_text(bat.asleep, buf);
	}
	charger_text(bat.charger);
	battery_temp_update();
	/* The chart: each minute */
	if (bat.history_ticks++ % 30 == 0)
		history_update();
}

static void percent_cb(lv_event_t *e)
{
	ui_vibrate(UI_VIBRATE_TAP);
	ui_setting_set_int("battery_percent", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
	ui_status_update(); /* at once, not at the next second */
}

void battery_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Battery", &page);

	/* The charge in large beside the battery drawn as in the status bar, the state below */
	lv_obj_t *card = settings_panel(page);
	lv_obj_set_style_pad_row(card, 8, 0);
	lv_obj_t *row = ui_box(card);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	bat.capacity = lv_label_create(row);
	ui_set_text(bat.capacity, "");
	lv_obj_set_style_text_font(bat.capacity, &ui_font_48, 0);
	bat.icon = ui_battery_icon_create(row, 96, 48);
	bat.status = lv_label_create(card);
	ui_set_text(bat.status, "");
	lv_obj_set_style_text_color(bat.status, lv_color_hex(UI_COLOR_DIM), 0);

	/* The number beside the status bar's battery, off by default: it takes room there */
	card = ui_card_create(page);
	row = settings_row(card);
	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text_static(label, "Show percentage in status bar");
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *sw = ui_switch_create(row, ui_setting_get_int("battery_percent", 0));
	lv_obj_add_event_cb(sw, percent_cb, LV_EVENT_VALUE_CHANGED, NULL);

	ui_section_label(page, "Last 24 hours");
	bat.history_ticks = 0;
	history_create(settings_panel(page));

	bat.since_label = ui_section_label(page, "Since unplugged");
	card = bat.since_card = ui_card_create(page);
	bat.screen_on = settings_info_row(card, "Screen on");
	bat.asleep = settings_info_row(card, "Asleep");

	ui_section_label(page, "Details");
	card = ui_card_create(page);
	bat.health = settings_info_row(card, "Health");
	bat.voltage = settings_info_row(card, "Voltage");
	bat.current = settings_info_row(card, "Current");
	bat.charger = settings_info_row(card, "Charger");
	bat.temperature = settings_info_row(card, "Temperature");
	bat.technology = settings_info_row(card, "Technology");
	/* From the device tree's battery description: the kernel has no measured one */
	long design;
	if (ui_read_long(UI_BATTERY "/charge_full_design", &design) && design > 0)
		ui_set_text_fmt(settings_info_row(card, "Design capacity"), "%ld mAh", design / 1000);

	settings_timer(scr, battery_update, 2000);
	ui_push(scr);
}
