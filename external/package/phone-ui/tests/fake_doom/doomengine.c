/*
 * Doom's engine in the tests (fake_doom.h): doom_run() as apps/doom/engine.c's,
 * drawing frames of one color and reading the fingers (the plugin's) until the plugin
 * ends it or the test quits it.
 */

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include "../../../../../apps/doom/doom.h"
#include "fake_doom.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

/* After each report */
static void held(const bool keys[256], int turn)
{
	for (int k = 0; k < 256; k++)
		STORE(fake_doom.keys[k], keys[k]);
	__atomic_add_fetch(&fake_doom.turn, turn, __ATOMIC_ACQ_REL);
}

__attribute__((visibility("default"))) void doom_run(struct doom_host *host)
{
	struct fake_doom *f = &fake_doom;
	snprintf(f->iwad, sizeof(f->iwad), "%s", host->iwad);
	snprintf(f->configdir, sizeof(f->configdir), "%s", host->configdir);
	STORE(f->told_to_quit, false);
	__atomic_add_fetch(&f->runs, 1, __ATOMIC_ACQ_REL);
	while (!LOAD(f->quit)) {
		if (LOAD(host->quit)) {
			STORE(f->told_to_quit, true);
			break;
		}
		STORE(f->orient, LOAD(host->orient));
		STORE(f->paused, LOAD(host->paused));
		if (LOAD(host->paused)) {
			host->touch_drop();
			usleep(2000);
			continue;
		}
		host->touch_read(held);
		uint32_t color = 0xff000000 | LOAD(f->color);
		pthread_mutex_lock(&host->lock);
		for (int i = 0; i < DOOM_W * DOOM_H; i++)
			host->frame[i] = color;
		host->frame_seq++;
		pthread_mutex_unlock(&host->lock);
		__atomic_add_fetch(&f->frames, 1, __ATOMIC_ACQ_REL);
		usleep(5000);
	}
	STORE(host->exited, true);
}
