/*
 * Doom, a plugin app (see external.c in phone-ui): the screens around the engine
 * (lib/doomengine.so: doomgeneric and its platform layer, engine.c), which it
 * loads to play a game and runs in a thread. What they share: doom.h; what the
 * plugin's files share: plugin.h.
 *
 * - plugin.c: phone-ui's entry point, the engine
 * - list.c: the games, the app's first screen
 * - screen.c: the game's screen
 * - wad.c: WAD files (which are games, their names and title screens)
 *
 * The app opens on the games: the IWADs in /var/lib/doom/wads (`just wads`) and
 * the shareware Doom it comes with. Leaving a game pauses Doom (opening it again
 * goes on, the list says which one is paused); picking another ends it. Quitting
 * from Doom's menu ends the thread too. Ended, the library is unloaded
 * (doomgeneric cannot start over) and loaded again for the next game. Settings and
 * saves go in /var/lib/app-data/doom/<IWAD's name>/.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "abi.h"
#include "plugin.h"

#define DATA "/var/lib/app-data/doom"
#define ENGINE "doomengine.so"

struct engine engine = { .host = { .lock = PTHREAD_MUTEX_INITIALIZER } };

static void *lib;
static doom_run_fn *run;
static char configdir[PATH_MAX];

const char *plugin_dir(void)
{
	static char dir[PATH_MAX];
	Dl_info info;
	if (!dir[0] && dladdr((void *)plugin_dir, &info) && info.dli_fname) {
		snprintf(dir, sizeof(dir), "%s", info.dli_fname);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
	}
	return dir;
}

/* The engine, from lib/ next to this plugin */
static bool engine_load(void)
{
	if (lib)
		return true;
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/lib/" ENGINE, plugin_dir());
	lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("doom: %s\n", dlerror());
		return false;
	}
	run = (doom_run_fn *)dlsym(lib, "doom_run");
	if (!run) {
		printf("doom: %s has no doom_run\n", ENGINE);
		dlclose(lib);
		lib = NULL;
		return false;
	}
	return true;
}

static void *doom_main(void *arg)
{
	(void)arg;
	run(&engine.host);
	return NULL;
}

/* Each game has its own settings and saves (Doom keeps them all in one folder):
 * DATA/<its name>/, Doom joining its file names to configdir
 * (DATA/<name>/.default.cfg, DATA/<name>/./.savegame/). */
bool engine_start(const char *iwad)
{
	if (!engine_load())
		return false;
	snprintf(engine.iwad, sizeof(engine.iwad), "%s", iwad);
	const char *base = strrchr(iwad, '/');
	snprintf(configdir, sizeof(configdir), DATA "/%s", base ? base + 1 : iwad);
	char *dot = strrchr(configdir, '.');
	if (dot && dot > configdir + sizeof(DATA))
		*dot = '\0';
	mkdir("/var/lib/app-data", 0755);
	mkdir(DATA, 0755);
	mkdir(configdir, 0755);
	strcat(configdir, "/.");
	engine.host.iwad = engine.iwad;
	engine.host.configdir = configdir;
	engine.host.paused = engine.host.exited = engine.host.quit = false;
	engine.host.pressed = 0;
	engine.host.sound = ui_sound_open("doom", 48000, 2, 512 * 4 * 1000000LL / 48000); /* sound.c's periods */
	engine.host.sound_write = ui_sound_write;
	engine.host.sound_release = ui_sound_release;
	if (pthread_create(&engine.thread, NULL, doom_main, NULL) != 0)
		return false;
	engine.running = true;
	printf("doom: playing %s\n", iwad);
	return true;
}

void engine_end(void)
{
	pthread_join(engine.thread, NULL);
	ui_sound_close(engine.host.sound);
	engine.host.sound = NULL;
	engine.running = false;
	engine.iwad[0] = '\0';
	dlclose(lib);
	lib = NULL;
	run = NULL;
}

void engine_stop(void)
{
	if (!engine.running)
		return;
	__atomic_store_n(&engine.host.quit, true, __ATOMIC_RELEASE);
	engine_end();
}

/* What phone-ui looks for (see external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	list_open();
}
