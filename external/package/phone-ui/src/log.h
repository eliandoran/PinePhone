/*
 * Log lines timed against each other and the kernel's: seconds of CLOCK_MONOTONIC,
 * which stops while the phone sleeps, as dmesg's timestamps do. Not for plugins.
 */
#ifndef LOG_H
#define LOG_H

#include <time.h>

static inline double log_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif
