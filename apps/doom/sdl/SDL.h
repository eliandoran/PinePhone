/*
 * The parts of SDL that Chocolate Doom's OPL music uses (opl.c, opl_sdl.c,
 * midifile.c), so that its files build unchanged: mutexes and conditions on
 * pthreads, byte swapping. The audio itself is sound.c's (SDL_mixer.h).
 */

#ifndef DOOM_SDL_H
#define DOOM_SDL_H

#include <endian.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t Sint16;

#define SDL_INIT_AUDIO 0x10
#define AUDIO_S16SYS 0x8010

typedef pthread_mutex_t SDL_mutex;
typedef pthread_cond_t SDL_cond;

static inline SDL_mutex *SDL_CreateMutex(void)
{
	SDL_mutex *m = malloc(sizeof(*m));
	pthread_mutex_init(m, NULL);
	return m;
}

static inline void SDL_DestroyMutex(SDL_mutex *m)
{
	pthread_mutex_destroy(m);
	free(m);
}

static inline int SDL_LockMutex(SDL_mutex *m)
{
	return pthread_mutex_lock(m);
}

static inline int SDL_UnlockMutex(SDL_mutex *m)
{
	return pthread_mutex_unlock(m);
}

static inline SDL_cond *SDL_CreateCond(void)
{
	SDL_cond *c = malloc(sizeof(*c));
	pthread_cond_init(c, NULL);
	return c;
}

static inline void SDL_DestroyCond(SDL_cond *c)
{
	pthread_cond_destroy(c);
	free(c);
}

static inline int SDL_CondSignal(SDL_cond *c)
{
	return pthread_cond_signal(c);
}

static inline int SDL_CondWait(SDL_cond *c, SDL_mutex *m)
{
	return pthread_cond_wait(c, m);
}

/* The output is opened by sound.c, never here */
static inline int SDL_Init(Uint32 flags)
{
	(void)flags;
	return 0;
}

static inline void SDL_QuitSubSystem(Uint32 flags)
{
	(void)flags;
}

static inline void SDL_PauseAudio(int pause)
{
	(void)pause;
}

#define SDL_SwapBE16(x) be16toh(x)
#define SDL_SwapBE32(x) be32toh(x)

#endif
