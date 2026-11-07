/*
 * What the plugin's files share (plugin.c lists them). The engine's side is
 * doom.h.
 */

#ifndef DOOM_PLUGIN_H
#define DOOM_PLUGIN_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "doom.h"
#include "i18n.h"
#include "ui.h"

/* plugin.c: Doom's engine, run in a thread */
struct engine {
	bool running;        /* started, until it ends (paused or not) */
	pthread_t thread;    /* running it */
	char iwad[PATH_MAX]; /* its game, "" if none */
	struct doom_host host;
};
extern struct engine engine;
const char *plugin_dir(void);        /* doom.so's folder */
bool engine_start(const char *iwad);
void engine_end(void);               /* after it quit (host.exited): the library unloaded */
void engine_stop(void);              /* ends it (paused, another game picked) */

/* screen.c: the game's screen, Doom with `iwad` started (another one ended
 * first) or resumed; `title` is copied */
void game_open(const char *iwad, const char *title);

/* list.c: the games, the app's first screen */
void list_open(void);
void list_game_left(void); /* the game's screen went: which one is paused, again */

/* touch.c: the fingers, for the engine (its host's touch_read(), touch_drop()) */
void touch_start(void);
void touch_stop(void);

/* wad.c: WAD files */
#define WAD_PIC_W 320 /* title screens */
#define WAD_PIC_H 200
const char *wad_known_name(const char *file); /* NULL if not known */
bool wad_is_game(const char *path, const char *file);
uint8_t *wad_title(const char *path); /* malloc'd BGRA, NULL if none */

#endif
