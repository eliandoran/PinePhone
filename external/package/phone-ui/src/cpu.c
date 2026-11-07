/*
 * The CPU's frequency, pinned: at its lowest (648 MHz on the A64, 1.04 V instead of 1.3 V
 * at 1152) for the interface, at its highest while an app asks for it (ui_cpu_boost():
 * Browser, Camera taking a photo, external apps with "cpu": "full"; games once their engine
 * no longer keeps up, ui_cpu_boost_when_busy()). Never left to schedutil:
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

/*
 * Games at the lowest while their engine keeps up: at 648 MHz Doom II's demo took 45%
 * of a core, Streets of Rage 55%, Dune (Sega CD) 68%, at full frame rate. Their engine
 * thread is measured each second; busy past BUSY_PERCENT of it, it is falling behind (a
 * crowded scene), and the CPU goes to its highest for the rest of the game: no going
 * back and forth, which costs more than it saves (above).
 */

#define BUSY_PERCENT 85
#define BUSY_CHECK_MS 1000

static struct {
	lv_timer_t *timer;
	clockid_t clock;          /* the thread's CPU time */
	struct timespec cpu, now; /* at the last check, the real time unwrapped by the tests */
} busy;

static void pin(bool highest)
{
	long min, max;
	if (!ui_read_long(CPUFREQ "cpuinfo_min_freq", &min) || !ui_read_long(CPUFREQ "cpuinfo_max_freq", &max))
		return;
	long khz = highest ? max : min;
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

static uint64_t ns(const struct timespec *t)
{
	return (uint64_t)t->tv_sec * 1000000000 + (uint64_t)t->tv_nsec;
}

static void busy_stop(void)
{
	if (busy.timer)
		lv_timer_delete(busy.timer);
	busy.timer = NULL;
}

static void busy_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	struct timespec cpu, now;
	if (clock_gettime(busy.clock, &cpu) != 0) { /* the thread ended */
		busy_stop();
		return;
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	uint64_t wall = ns(&now) - ns(&busy.now);
	if (wall < BUSY_CHECK_MS * 1000000ull / 2)
		return;
	uint64_t used = ns(&cpu) - ns(&busy.cpu);
	busy.cpu = cpu;
	busy.now = now;
	if (used * 100 < wall * BUSY_PERCENT)
		return;
	printf("phone-ui: the game's engine busy %d%% of the time, the CPU at its highest\n",
	       (int)(used * 100 / wall));
	busy_stop();
	pin(true);
}

void ui_cpu_boost(bool on)
{
	busy_stop();
	pin(on);
}

void ui_cpu_boost_when_busy(pthread_t thread)
{
	ui_cpu_boost(false);
	if (pthread_getcpuclockid(thread, &busy.clock) != 0 || clock_gettime(busy.clock, &busy.cpu) != 0)
		return;
	clock_gettime(CLOCK_MONOTONIC_RAW, &busy.now);
	busy.timer = lv_timer_create(busy_cb, BUSY_CHECK_MS, NULL);
}
