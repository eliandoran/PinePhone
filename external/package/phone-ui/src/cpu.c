/*
 * The CPU's frequency, pinned: at its lowest (648 MHz on the A64, 1.04 V instead of 1.3 V
 * at 1152) for the interface, at its highest while an app asks for it (ui_cpu_boost():
 * games, Browser, Camera taking a photo, external apps with "cpu": "full"). Never left to
 * schedutil:
 * - Each change runs the clock driver's PLL search and a PMIC voltage write in a
 *   deadline-priority thread (sugov) that preempts everything else; the CPU waits on the
 *   24 MHz oscillator while the PLL locks (cpuinfo_transition_latency: 244 us).
 * - Work that waits on hardware in small steps makes it change all the time. Dragging,
 *   with the touchscreen's interrupts: 15% of the CPU in sugov, about 40 FPS instead of
 *   60. Receiving over Wi-Fi (an SDIO command per register, ~250 changes a second):
 *   1.25 MB/s, 2.4 at 648 MHz pinned, 3.2 at 1152.
 * The menu scrolls at 66 FPS at 648 MHz, about half a core.
 *
 * S90phone-ui gives the whole range back when phone-ui stops, pp-update its highest.
 */

#include "ui.h"

#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0/"

void ui_cpu_boost(bool on)
{
	long min, max;
	if (!ui_read_long(CPUFREQ "cpuinfo_min_freq", &min) || !ui_read_long(CPUFREQ "cpuinfo_max_freq", &max))
		return;
	long khz = on ? max : min;
	/* The kernel refuses a minimum above the maximum and the other way round */
	long cur;
	if (ui_read_long(CPUFREQ "scaling_max_freq", &cur) && khz > cur) {
		ui_write_long(CPUFREQ "scaling_max_freq", khz);
		ui_write_long(CPUFREQ "scaling_min_freq", khz);
	} else {
		ui_write_long(CPUFREQ "scaling_min_freq", khz);
		ui_write_long(CPUFREQ "scaling_max_freq", khz);
	}
}
