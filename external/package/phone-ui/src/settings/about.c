/* Settings, About phone: the phone number and IMEI, software, hardware, temperatures */

#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "settings.h"

#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq"
#define THERMAL "/sys/class/thermal"

void about_summary(lv_obj_t *value)
{
	ui_set_text(value, "PinePhone");
}

static struct {
	struct settings_mobile mobile;
	lv_obj_t *uptime, *memory, *freq;
	lv_obj_t *cpu_temp, *gpu_temp, *pmic_temp, *battery_temp;
} about;

/* Thermal zone in millidegrees: 0 cpu0, 1 gpu0, 2 gpu1 */
static bool zone_temp(int zone, long *mc)
{
	char path[64];
	snprintf(path, sizeof(path), THERMAL "/thermal_zone%d/temp", zone);
	return ui_read_long(path, mc);
}

/* IIO temperature channel in millidegrees: (raw + offset) * scale */
static bool iio_temp(const char *dev, long *mc)
{
	long raw, offset, scale;
	if (!settings_attr_long(dev, "in_temp_raw", &raw) || !settings_attr_long(dev, "in_temp_offset", &offset) ||
	    !settings_attr_long(dev, "in_temp_scale", &scale))
		return false;
	*mc = (raw + offset) * scale;
	return true;
}

/* Whole degrees: each distinct text is kept */
static void temp_text(lv_obj_t *label, bool ok, long mc)
{
	if (ok)
		ui_set_text_fmt(label, "%ld °C", (mc + (mc < 0 ? -500 : 500)) / 1000);
	else
		ui_set_text(label, _("Unknown"));
}

static void temp_update(void)
{
	long cpu, gpu0, gpu1, pmic, trip;

	/* Orange within 10 °C of the first trip point (throttling starts), red past it */
	bool ok = zone_temp(0, &cpu);
	temp_text(about.cpu_temp, ok, cpu);
	if (!ui_read_long(THERMAL "/thermal_zone0/trip_point_0_temp", &trip))
		trip = 75000;
	lv_obj_set_style_text_color(about.cpu_temp,
	                            !ok || cpu < trip - 10000 ? lv_color_hex(UI_COLOR_DIM) :
	                            lv_color_hex(cpu < trip ? UI_COLOR_ORANGE : UI_COLOR_RED), 0);

	/* Two sensors on the GPU: the hotter one */
	bool ok0 = zone_temp(1, &gpu0), ok1 = zone_temp(2, &gpu1);
	temp_text(about.gpu_temp, ok0 || ok1, ok0 && (!ok1 || gpu0 > gpu1) ? gpu0 : gpu1);

	ok = iio_temp(settings_pmic_adc(), &pmic);
	temp_text(about.pmic_temp, ok, pmic);

	long battery;
	ok = battery_temperature(&battery);
	temp_text(about.battery_temp, ok, battery * 1000);
}

static void about_update(lv_timer_t *t)
{
	LV_UNUSED(t);
	struct sysinfo si;
	char buf[64];
	long khz;

	settings_mobile_update(&about.mobile);

	if (sysinfo(&si) == 0) {
		long up = si.uptime;
		if (up >= 86400)
			ui_set_text_fmt(about.uptime, _("%ld d %ld h %ld min"), up / 86400, up / 3600 % 24, up / 60 % 60);
		else if (up >= 3600)
			ui_set_text_fmt(about.uptime, "%ld h %ld min", up / 3600, up / 60 % 60);
		else
			ui_set_text_fmt(about.uptime, "%ld min", up / 60);
		unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
		ui_set_text_fmt(about.memory, _("%llu MB free of %llu MB"),
		                (si.freeram + si.bufferram) * unit >> 20, si.totalram * unit >> 20);
	}

	if (ui_read_long(CPUFREQ "/scaling_cur_freq", &khz)) {
		if (!ui_read_line(CPUFREQ "/scaling_governor", buf, sizeof(buf)))
			buf[0] = '\0';
		ui_set_text_fmt(about.freq, buf[0] ? "%ld MHz (%s)" : "%ld MHz", khz / 1000, buf);
	} else {
		ui_set_text(about.freq, _("Unknown"));
	}

	temp_update();
}

static const char *boot_slot(void)
{
	char cmdline[1024];
	if (!ui_read_line("/proc/cmdline", cmdline, sizeof(cmdline)))
		return _("Unknown");
	const char *root = strstr(cmdline, "root=PARTUUID=50480001-0");
	if (!root)
		return _("Unknown");
	switch (root[strlen("root=PARTUUID=50480001-0")]) {
	case '1': return _("A (partition 1)");
	case '2': return _("B (partition 2)");
	default:  return _("Unknown");
	}
}

void about_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("About phone"), &page);
	char buf[128];
	struct utsname un;

	/* Big model name */
	lv_obj_t *card = settings_panel(page);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *icon = lv_label_create(card);
	lv_label_set_text_static(icon, LV_SYMBOL_CALL);
	lv_obj_set_style_text_font(icon, &ui_font_48, 0);
	lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_t *model = lv_label_create(card);
	lv_obj_set_style_text_font(model, &ui_font_32, 0);
	/* The device tree string ends with a NUL, not a newline */
	if (!ui_read_line("/proc/device-tree/model", buf, sizeof(buf)))
		snprintf(buf, sizeof(buf), "PinePhone");
	ui_set_text(model, buf);

	settings_mobile_create(page, &about.mobile, false);

	ui_section_label(page, _("Software"));
	card = ui_card_create(page);
	if (uname(&un) == 0)
		ui_set_text_fmt(settings_info_row(card, _("Kernel")), "Linux %s (%s)", un.release, un.machine);
	ui_set_text_fmt(settings_info_row(card, "LVGL"), "%d.%d.%d", lv_version_major(), lv_version_minor(),
	                lv_version_patch());
	ui_set_text(settings_info_row(card, _("Boot slot")), boot_slot());
	about.uptime = settings_info_row(card, _("Uptime"));

	ui_section_label(page, _("Hardware"));
	card = ui_card_create(page);
	ui_set_text(settings_info_row(card, "SoC"), "Allwinner A64");
	ui_set_text_fmt(settings_info_row(card, "CPU"), "%ld x Cortex-A53", sysconf(_SC_NPROCESSORS_ONLN));
	about.freq = settings_info_row(card, _("CPU frequency"));
	ui_set_text(settings_info_row(card, "GPU"), "Mali-400 MP2 (Mesa lima)");
	about.memory = settings_info_row(card, _("Memory"));

	ui_section_label(page, _("Temperature"));
	card = ui_card_create(page);
	about.cpu_temp = settings_info_row(card, "CPU");
	about.gpu_temp = settings_info_row(card, "GPU");
	about.pmic_temp = settings_info_row(card, _("Power chip"));
	about.battery_temp = settings_info_row(card, _("Battery"));

	settings_timer(scr, about_update, 2000);
	ui_push(scr);
}
