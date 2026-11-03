/*
 * What Chocolate Doom 2.3's opl.c, midifile.c and i_oplmusic.c find in its own
 * headers but not in doomgeneric's (older): included first by the Makefile.
 */

#ifndef DOOM_CHOCO_H
#define DOOM_CHOCO_H

#include "SDL.h"

/* i_sound.h: the DMX version whose OPL quirks to reproduce */
typedef enum {
	opl_doom1_1_666, /* Doom 1 v1.666 */
	opl_doom2_1_666, /* Doom 2 v1.666, Hexen, Heretic */
	opl_doom_1_9     /* Doom v1.9, Strife */
} opl_driver_ver_t;

#endif
