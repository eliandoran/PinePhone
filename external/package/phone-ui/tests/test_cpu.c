/*
 * The CPU's frequency (cpu.c): pinned at its lowest, where the interface runs, and at
 * its highest while an app asks for it; never left to schedutil, whose changes cost
 * more than they save
 */

#include <pthread.h>
#include <unistd.h>

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

/* Games: at the lowest while their engine keeps up, at the highest once it does not */

static bool spin_quit;

static void *spin(void *arg)
{
	(void)arg;
	while (!__atomic_load_n(&spin_quit, __ATOMIC_ACQUIRE))
		;
	return NULL;
}

static void *idle(void *arg)
{
	(void)arg;
	while (!__atomic_load_n(&spin_quit, __ATOMIC_ACQUIRE))
		usleep(10000);
	return NULL;
}

static bool boosted(void)
{
	return min_mhz() == 1152 && max_mhz() == 1152;
}

TEST(busy_engine_boosted)
{
	pthread_t t;
	CHECK(pthread_create(&t, NULL, spin, NULL) == 0);
	ui_cpu_boost_when_busy(t);
	CHECK_INT(min_mhz(), 648);
	CHECK(ui_test_wait_until(boosted, 5000));
	/* Until the game is left */
	ui_cpu_boost(false);
	CHECK_INT(max_mhz(), 648);
	__atomic_store_n(&spin_quit, true, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
}

TEST(idle_engine_not_boosted)
{
	pthread_t t;
	CHECK(pthread_create(&t, NULL, idle, NULL) == 0);
	ui_cpu_boost(true);
	ui_cpu_boost_when_busy(t); /* lowers it */
	CHECK_INT(max_mhz(), 648);
	CHECK(!ui_test_wait_until(boosted, 3000));
	__atomic_store_n(&spin_quit, true, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
}

/* The engine's thread ended (the game quit by itself): no longer watched */
TEST(ended_engine_not_watched)
{
	pthread_t t;
	CHECK(pthread_create(&t, NULL, idle, NULL) == 0);
	ui_cpu_boost_when_busy(t);
	__atomic_store_n(&spin_quit, true, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	CHECK(!ui_test_wait_until(boosted, 3000));
	CHECK_INT(max_mhz(), 648);
}

/* Left, the engine no longer watched: busy afterwards (another app's), not boosted */
TEST(left_not_watched)
{
	pthread_t t;
	CHECK(pthread_create(&t, NULL, spin, NULL) == 0);
	ui_cpu_boost_when_busy(t);
	ui_cpu_boost(false);
	CHECK(!ui_test_wait_until(boosted, 3000));
	__atomic_store_n(&spin_quit, true, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
}
