/*
 * The CPU's frequency (cpu.c): pinned at its lowest, where the interface runs, and at
 * its highest while an app asks for it; never left to schedutil, whose changes cost
 * more than they save
 */

#include "test.h"
#include "ui_test.h"

#define CPUFREQ "/sys/devices/system/cpu/cpufreq/policy0/"

TEST_SETUP
{
	ui_test_start();
}

/* scaling_min_freq and scaling_max_freq, in MHz */
static long min_mhz(void)
{
	long khz = 0;
	ui_read_long(CPUFREQ "scaling_min_freq", &khz);
	return khz / 1000;
}

static long max_mhz(void)
{
	long khz = 0;
	ui_read_long(CPUFREQ "scaling_max_freq", &khz);
	return khz / 1000;
}

TEST(lowest_at_startup)
{
	CHECK_INT(min_mhz(), 648);
	CHECK_INT(max_mhz(), 648);
}

/* Interaction used to pin it for a second, then schedutil had it again */
TEST(lowest_after_scrolling)
{
	CHECK(ui_test_open("Settings"));
	ui_test_swipe(360, 1200, 360, 400, 300);
	ui_test_wait(3000);
	CHECK_INT(min_mhz(), 648);
	CHECK_INT(max_mhz(), 648);
}

TEST(boost_pins_highest)
{
	ui_cpu_boost(true);
	CHECK_INT(min_mhz(), 1152);
	CHECK_INT(max_mhz(), 1152);
	ui_test_wait(3000);
	CHECK_INT(min_mhz(), 1152);
	ui_cpu_boost(false);
	CHECK_INT(min_mhz(), 648);
	CHECK_INT(max_mhz(), 648);
}
