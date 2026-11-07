/*
 * Doom on the PinePhone, the engine side: doomgeneric's platform layer, built
 * with doomgeneric into lib/doomengine.so, which the plugin (plugin.c) loads and
 * runs in a thread with doom_run(). See doom.h for what they share.
 *
 * - Frames: DG_DrawFrame() copies Doom's into host->frame, opaque (Doom leaves
 *   the alpha byte 0), for the plugin to upload as a texture.
 * - Touch: the plugin reads the fingers (touch.c, the controls), in this thread:
 *   DG_DrawFrame() and DG_GetKey() ask it (host->touch_read()), and the keys it
 *   says are held go into Doom's queue, the turning into its mouse.
 * - Pausing (host->paused, the app not shown or covered: the lock screen, asleep or
 *   woken, the quick settings): Doom's clock stops (no catching up
 *   afterwards), the keys held are let go, the sound device is let go (sound.c).
 * - Quitting: Doom calls exit() (its menu, I_Error()), built as doom_exit(), which
 *   returns from doom_run(); the plugin then unloads the library, whose state
 *   cannot be used again (doomgeneric has no way to start over).
 * - Settings and saves go in host->configdir (m_config.c, changed by the Makefile).
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doom.h"
#include "doomgeneric.h"

#define TURN_SCALE 6  /* mouse units per px dragged: 8192 turn around */
#define PAUSE_POLL_MS 50

/* g_game.c: mouse turning for the next tic, cleared once used */
extern int mousex;
extern int key_prevweapon, key_nextweapon;

/* sound.c */
void doom_sound_host(struct doom_host *h);
void doom_sound_pause(bool paused);
void doom_sound_stop(void);

/* m_config.c (the Makefile makes its default configuration directory this) */
char *doom_configdir;

static struct doom_host *host;
static jmp_buf quit_jmp;
static uint32_t paused_ms; /* left out of Doom's clock */

/* Input: the keys the fingers hold (the plugin's touch.c), queued for Doom */
static bool key_down[256];
static unsigned short queue[64];
static unsigned queue_r, queue_w;

void doom_exit(int code)
{
	printf("doom: quit (%d)\n", code);
	longjmp(quit_jmp, 1);
}

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void key_set(int key, bool down)
{
	if (key_down[key] == down)
		return;
	key_down[key] = down;
	if (queue_w - queue_r < sizeof(queue) / sizeof(queue[0]))
		queue[queue_w++ % (sizeof(queue) / sizeof(queue[0]))] = down << 8 | key;
}

/* After each report of the fingers: the keys they hold, the turning */
static void held(const bool keys[256], int turn)
{
	for (int k = 1; k < 256; k++)
		key_set(k, keys[k]);
	mousex += turn * TURN_SCALE; /* added up until the next tic uses it */
}

/* While the app is not shown: nothing held, the clock stopped, the sound let go.
 * Touches made meanwhile (in other screens) are dropped. */
static void pause_wait(void)
{
	if (!__atomic_load_n(&host->paused, __ATOMIC_ACQUIRE))
		return;
	for (int k = 1; k < 256; k++)
		key_set(k, false);
	host->touch_drop();
	doom_sound_pause(true);
	uint32_t start = now_ms();
	while (__atomic_load_n(&host->paused, __ATOMIC_ACQUIRE) && !__atomic_load_n(&host->quit, __ATOMIC_ACQUIRE))
		usleep(PAUSE_POLL_MS * 1000);
	paused_ms += now_ms() - start;
	host->touch_drop();
	doom_sound_pause(false);
}

void DG_Init(void)
{
	/* The fingers are the plugin's (touch.c) */
}

void DG_DrawFrame(void)
{
	host->touch_read(held);
	pthread_mutex_lock(&host->lock);
	for (int i = 0; i < DOOM_W * DOOM_H; i++)
		host->frame[i] = DG_ScreenBuffer[i] | 0xff000000;
	host->frame_seq++;
	pthread_mutex_unlock(&host->lock);
}

void DG_SleepMs(uint32_t ms)
{
	usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
	return now_ms() - paused_ms;
}

int DG_GetKey(int *pressed_out, unsigned char *key)
{
	if (queue_r == queue_w)
		host->touch_read(held);
	if (queue_r == queue_w)
		return 0;
	unsigned short e = queue[queue_r++ % (sizeof(queue) / sizeof(queue[0]))];
	*pressed_out = e >> 8;
	*key = e & 0xff;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

__attribute__((visibility("default"))) void doom_run(struct doom_host *h)
{
	static char *args[4];
	host = h;
	doom_sound_host(h);
	doom_configdir = (char *)h->configdir;
	args[0] = "doom";
	args[1] = "-iwad";
	args[2] = (char *)h->iwad;
	if (setjmp(quit_jmp) == 0) {
		doomgeneric_Create(3, args);
		/* After the settings were read, which would leave them unbound */
		key_prevweapon = DOOM_KEY_PREV_WEAPON;
		key_nextweapon = DOOM_KEY_NEXT_WEAPON;
		for (;;) {
			pause_wait();
			if (__atomic_load_n(&host->quit, __ATOMIC_ACQUIRE))
				doom_exit(0); /* without saving the settings, as if killed */
			doomgeneric_Tick();
		}
	}
	doom_sound_stop();
	__atomic_store_n(&host->exited, true, __ATOMIC_RELEASE);
}
