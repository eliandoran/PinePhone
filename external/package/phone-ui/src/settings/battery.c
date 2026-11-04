/* Settings, Battery: charge, details, the thermistor's temperature and the charger's cut-offs */

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
	if (!ui_read_line(UI_BATTERY "/status", status, size))
		status[0] = '\0';
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

/* The last day of the battery's history (battery_log.c), a bar per SLOT_S seconds: the
 * level at the slot's end, drawn straight between samples (phone-ui does not run while the
 * phone sleeps) unless they are more than HISTORY_GAP apart (it was off), up to now */
#define HISTORY_SLOTS 96
#define SLOT_S 900
#define HISTORY_GAP (12 * 3600)
#define HISTORY_H 200 /* the bars' height at 100% */
#define HISTORY_AXIS 80 /* the levels' labels, right of the bars */
#define HISTORY_HOURS 5
#define COLOR_CHARGING 0x34c759

static struct {
	lv_obj_t *capacity, *status, *bar;
	lv_obj_t *health, *voltage, *current, *charger, *technology;
	lv_obj_t *marker, *line, *temp_note, *charge_band, *alarm_line[2];
	lv_obj_t *charge_label[2], *alarm_label[2], *legend_charge, *legend_alarm, *limits_note;
	lv_obj_t *history, *history_line[3], *history_bar[HISTORY_SLOTS], *history_hour[HISTORY_HOURS];
	int history_ticks;
} bat;



/* The battery's thermistor, on the power chip's TS pin. The PinePhone's battery has a
 * 3 kΩ NTC (B about 3880, from the resistances in Megi's kernel patch "axp20x_battery:
 * Setup thermal regulation"). The AXP803 feeds it a current and compares the voltage with
 * thresholds that by default assume a 10 kΩ one; its registers are only in debugfs. */
#define PMIC_REGS "/sys/kernel/debug/regmap/sunxi-rsb-3a3/registers"
#define NTC_R25 3000.0
#define NTC_B 3880.0
#define SCALE_MIN (-30) /* the scale, °C: 1% of its width per degree */
#define SCALE_MAX 70

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

/* A threshold register: 12.8 mV per step */
static long threshold_celsius(int reg, double ua)
{
	double c;
	return reg > 0 && ntc_celsius(reg * 12.8, ua, &c) ? lround(c) : SCALE_MAX;
}

static int scale_pct(long c)
{
	return (int)(LV_CLAMP(SCALE_MIN, c, SCALE_MAX) - SCALE_MIN) * 100 / (SCALE_MAX - SCALE_MIN);
}

/* Shown at c °C of the scale (centered by its translation), or hidden */
static void scale_place(lv_obj_t *obj, bool shown, long c)
{
	lv_obj_set_hidden(obj, !shown);
	if (shown)
		lv_obj_set_x(obj, LV_PCT(scale_pct(c)));
}

static void scale_label(lv_obj_t *label, bool shown, long c)
{
	scale_place(label, shown, c);
	if (shown)
		ui_set_text_fmt(label, "%ld°", c);
}

static void battery_temp_update(void)
{
	long raw;
	double scale, c = 0;
	int regs[256];
	bool have_regs = pmic_regs(regs) && regs[0x84] >= 0;

	/* TS current: 0x84 bits 5:4, 20 µA steps (80 µA by default) */
	double ua = have_regs ? ((regs[0x84] >> 4 & 3) + 1) * 20 : 80;
	const char *adc = settings_pmic_adc();
	bool ok = settings_attr_double(adc, "in_voltage0_scale", &scale) && scale > 0 &&
	          settings_attr_long(adc, "in_voltage0_raw", &raw) && ntc_celsius(raw * scale, ua, &c);
	scale_label(bat.marker, ok, lround(c));
	scale_place(bat.line, ok, lround(c));
	lv_obj_set_hidden(bat.temp_note, ok);

	/* The cut-offs: a higher voltage is colder. 0x84 bit 2 makes the pin a plain ADC input,
	 * the chip ignores the temperature. */
	bool check = have_regs && !(regs[0x84] & 4);
	long charge[2] = { threshold_celsius(check ? regs[0x38] : -1, ua),
	                   threshold_celsius(check ? regs[0x39] : -1, ua) };
	long alarm[2] = { threshold_celsius(check ? regs[0x3c] : -1, ua),
	                  threshold_celsius(check ? regs[0x3d] : -1, ua) };
	for (int i = 0; i < 2; i++) {
		scale_label(bat.charge_label[i], check, charge[i]);
		scale_label(bat.alarm_label[i], check, alarm[i]);
		scale_place(bat.alarm_line[i], check, alarm[i]);
	}
	scale_place(bat.charge_band, check && charge[0] < charge[1], charge[0]);
	if (check)
		lv_obj_set_width(bat.charge_band, LV_PCT(scale_pct(charge[1]) - scale_pct(charge[0])));
	lv_obj_set_hidden(bat.legend_charge, !check);
	lv_obj_set_hidden(bat.legend_alarm, !check);
	lv_obj_set_hidden(bat.limits_note, check);
	ui_set_text(bat.limits_note, have_regs ? "Charges at any temperature" : "Limits unknown");
}

static lv_obj_t *scale_bar(lv_obj_t *parent, int32_t w, int32_t h, int32_t y, uint32_t color)
{
	lv_obj_t *bar = ui_box(parent);
	lv_obj_set_size(bar, w, h);
	lv_obj_set_y(bar, y);
	lv_obj_set_style_radius(bar, 3, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
	return bar;
}

static lv_obj_t *scale_text(lv_obj_t *parent, int32_t y, uint32_t color, bool centered)
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

/* Degrees from SCALE_MIN to SCALE_MAX: the battery's temperature as a marker with its
 * degrees above, where the chip charges as a blue band, its alarm's limits as red lines,
 * their degrees below */
static void temp_scale_create(lv_obj_t *card)
{
	lv_obj_t *box = ui_box(card);
	lv_obj_set_size(box, LV_PCT(100), 196);
	lv_obj_set_style_pad_hor(box, 32, 0);
	lv_obj_set_style_pad_ver(box, 20, 0);
	lv_obj_set_scrollable(box, false);

	lv_obj_t *track = scale_bar(box, LV_PCT(100), 16, 44, UI_COLOR_TRACK);
	lv_obj_set_style_radius(track, 8, 0);
	bat.charge_band = scale_bar(track, 0, LV_PCT(100), 0, UI_COLOR_ACCENT);
	lv_obj_set_style_radius(bat.charge_band, 8, 0);

	/* Centered on their degree: moved back by half their width */
	for (int i = 0; i < 2; i++) {
		bat.alarm_line[i] = scale_bar(box, 4, 40, 32, UI_COLOR_RED);
		lv_obj_set_style_translate_x(bat.alarm_line[i], -2, 0);
		bat.charge_label[i] = scale_text(box, 76, UI_COLOR_ACCENT, true);
		bat.alarm_label[i] = scale_text(box, 104, UI_COLOR_RED, true);
	}
	bat.line = scale_bar(box, 6, 32, 36, UI_COLOR_TEXT);
	lv_obj_set_style_translate_x(bat.line, -3, 0);
	bat.marker = lv_label_create(box);
	ui_set_text(bat.marker, "");
	lv_obj_set_style_translate_x(bat.marker, LV_PCT(-50), 0);
	bat.temp_note = scale_text(box, 4, UI_COLOR_DIM, false);
	ui_set_text(bat.temp_note, "Temperature unknown");

	bat.legend_charge = scale_text(box, 0, UI_COLOR_ACCENT, false);
	ui_set_text(bat.legend_charge, "Charging");
	lv_obj_align(bat.legend_charge, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	bat.legend_alarm = scale_text(box, 0, UI_COLOR_RED, false);
	ui_set_text(bat.legend_alarm, "Alarm");
	lv_obj_align(bat.legend_alarm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	bat.limits_note = scale_text(box, 0, UI_COLOR_DIM, false);
	lv_obj_align(bat.limits_note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
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
		s[n++] = (struct battery_sample){ now, capacity, strcmp(status, "Charging") == 0 };
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
	lv_obj_set_size(chart, LV_PCT(100), HISTORY_H + 54);
	lv_obj_set_style_pad_top(chart, 14, 0); /* the 100% label, centered on its line */
	lv_obj_set_scrollable(chart, false);
	static const char *const levels[] = { "100%", "50%", "0%" };
	for (int i = 0; i < 3; i++) {
		int32_t y = (HISTORY_H - 1) * i / 2;
		bat.history_line[i] = scale_bar(chart, 0, 1, y, UI_COLOR_TRACK);
		lv_obj_set_style_radius(bat.history_line[i], 0, 0);
		lv_obj_t *label = scale_text(chart, 0, UI_COLOR_DIM, false);
		ui_set_text(label, levels[i]);
		lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, y - 12);
	}
	for (int i = 0; i < HISTORY_SLOTS; i++) {
		bat.history_bar[i] = scale_bar(chart, 2, 2, 0, UI_COLOR_ACCENT);
		lv_obj_set_style_radius(bat.history_bar[i], 2, 0);
	}
	for (int i = 0; i < HISTORY_HOURS; i++)
		bat.history_hour[i] = scale_text(chart, HISTORY_H + 10, UI_COLOR_DIM, true);
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
		lv_bar_set_value(bat.bar, 0, LV_ANIM_OFF);
	} else {
		ui_set_text_fmt(bat.capacity, "%d%%", capacity);
		if (strcmp(buf, "Charging") == 0 && ui_battery_time_to_full(full, sizeof(full)))
			ui_set_text_fmt(bat.status, "%s \xc2\xb7 full in about %s", buf, full);
		else
			ui_set_text(bat.status, buf[0] ? buf : "Unknown");
		lv_bar_set_value(bat.bar, capacity, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(bat.bar, lv_color_hex(capacity < 15 ? UI_COLOR_RED : 0x34c759),
		                          LV_PART_INDICATOR);
	}

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
	charger_text(bat.charger);
	battery_temp_update();
	/* The chart: each minute */
	if (bat.history_ticks++ % 30 == 0)
		history_update();
}

void battery_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Battery", &page);

	lv_obj_t *card = settings_panel(page);
	lv_obj_set_style_pad_row(card, 8, 0);
	bat.capacity = lv_label_create(card);
	ui_set_text(bat.capacity, "");
	lv_obj_set_style_text_font(bat.capacity, &ui_font_48, 0);
	bat.status = lv_label_create(card);
	ui_set_text(bat.status, "");
	lv_obj_set_style_text_color(bat.status, lv_color_hex(UI_COLOR_DIM), 0);
	bat.bar = settings_bar(card, 100, 0x34c759);
	lv_obj_set_style_margin_top(bat.bar, 16, 0);

	ui_section_label(page, "Last 24 hours");
	bat.history_ticks = 0;
	history_create(settings_panel(page));

	ui_section_label(page, "Details");
	card = ui_card_create(page);
	bat.health = settings_info_row(card, "Health");
	bat.voltage = settings_info_row(card, "Voltage");
	bat.current = settings_info_row(card, "Current");
	bat.charger = settings_info_row(card, "Charger");
	bat.technology = settings_info_row(card, "Technology");
	/* From the device tree's battery description: the kernel has no measured one */
	long design;
	if (ui_read_long(UI_BATTERY "/charge_full_design", &design) && design > 0)
		ui_set_text_fmt(settings_info_row(card, "Design capacity"), "%ld mAh", design / 1000);

	ui_section_label(page, "Temperature");
	card = ui_card_create(page);
	temp_scale_create(card);

	settings_timer(scr, battery_update, 2000);
	ui_push(scr);
}
