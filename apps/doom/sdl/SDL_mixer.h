/*
 * The parts of SDL_mixer that Chocolate Doom's OPL music uses (opl_sdl.c), done
 * by sound.c: the output's format, and the music hook its mixer calls to fill
 * each period before adding the sound effects. (i_sound.c includes this too,
 * for nothing.)
 */

#ifndef DOOM_SDL_MIXER_H
#define DOOM_SDL_MIXER_H

#include "SDL.h"

/* Succeeds if sound.c's output is open, whatever the arguments */
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);
/* 0 if the output is not open */
int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);
void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg);
const char *Mix_GetError(void);

#endif
