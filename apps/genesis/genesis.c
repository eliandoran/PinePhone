/*
 * Genesis: Sega Genesis / Mega Drive, Sega CD (and Master System, Game Gear, SG-1000)
 * games, a plugin app (see external.c in phone-ui) around Genesis Plus GX's libretro core.
 *
 * Games are ROM files or disc images (.cue with its tracks, .chd, .iso; .m3u for
 * several discs) in /var/lib/genesis/roms, listed by name (read again when pulled down). Held upright, as a
 * handheld: the game at the top, twice its size (640 px wide, 448 or 480 high;
 * double-resolution interlaced modes at their size), and the controls below it:
 *
 *        ^             C
 *     <     >       B
 *        v       A
 *          START
 *
 * Turned sideways (either way), three times its size (960 px wide) in the middle of
 * a black screen, the D-pad on the left, the buttons and Start on the right, back
 * and restart at the top left. phone-ui turns the display (ui_screen_turns(), full
 * screen: no status bar) and the screen is laid out again in LVGL's 1440 x 720.
 *
 * The D-pad goes by the angle from its center (diagonals in between), from wherever
 * the finger that started on it goes. A finger on the buttons presses the ones it
 * is near: between two, both. Several fingers at once: the touchscreen is read here
 * (LVGL only sees one pointer), next to LVGL, which ignores the controls.
 *
 * The core (genesis_plus_gx_libretro.so in lib/) is loaded when the app first opens
 * and stays: it is a single instance. It runs in a thread, paced by the sound
 * output (48 kHz, as Doom's; phone-ui's ui_sound_write(): the phone's, or a Bluetooth
 * speaker), or by the clock when the output is busy or missing. It
 * waits while covered (the lock screen, asleep or woken, the quick settings): the
 * sound stopped, touches dropped.
 * Each frame (RGB565) is copied for LVGL's thread, which uploads it into a GL
 * texture shown by an lv_3dtexture.
 *
 * Leaving a game saves where it was (a save state) and its battery-backed RAM into
 * /var/lib/genesis/saves; opening it again resumes there. The header's button
 * restarts the game. BIOS files go in /var/lib/genesis (`just bios`): Sega CD games
 * need bios_CD_U.bin, bios_CD_E.bin or bios_CD_J.bin for their region; the core
 * keeps their backup RAM in saves/ itself.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include <linux/input.h>

#include "abi.h"
#include "i18n.h"
#include "libretro.h"
#include "ui.h"

#define HOME "/var/lib/genesis"
#define ROMS HOME "/roms"
#define SAVES HOME "/saves"
#define CORE "genesis_plus_gx_libretro.so"

#define RATE 48000 /* the core's, changed from 44100 by the Makefile */
#define PERIOD 512
#define PERIODS 4

/* Frames as the core makes them: its bitmap is 720 wide, at most 576 high (PAL,
 * interlaced) */
#define MAX_W 720
#define MAX_H 576

#define SCREEN_W 720
#define SCREEN_H 1440
#define BODY_Y (UI_STATUS_H + UI_HEADER_H) /* where the screen's body starts, upright */
#define DPAD_DEAD 28 /* no direction this close to the D-pad's center */
#define SLOTS 10

/*
 * Orientations: upright, or sideways with the phone's top to the left (Power and
 * the volume keys on top, as Doom: the display turned 90 degrees) or to the right
 * (270). phone-ui turns the display (turn.c); a layout is in LVGL's coordinates
 * then, 1440 x 720 sideways, all of it (full screen).
 */
enum { PORTRAIT, LEFT, RIGHT };

struct layout {
	int w, h;
	int scale;             /* of the game; its width is 320 * scale, whatever the mode */
	int game_y;            /* of its top; sideways it is centered */
	int dpad_x, dpad_y;    /* center */
	int dpad_reach;        /* a finger landing this close is on the D-pad */
	int dpad_key;          /* each arrow's square */
	int dpad_off;          /* from the center to each arrow's */
	int button_d;          /* diameter */
	int button_hit;        /* a finger presses the buttons whose center is this close */
	struct { int x, y; } buttons[3]; /* A, B, C */
	int start_x, start_y, start_w, start_h;
};

static const struct layout layouts[] = {
	[PORTRAIT] = { SCREEN_W, SCREEN_H, 2, BODY_Y + 16, 190, 1040, 230, 104, 108, 112, 92,
	               { { 410, 1110 }, { 530, 1040 }, { 650, 970 } }, 360, 1330, 168, 64 },
	/* The game 960 wide in the middle, 240 on each side for the controls; back and
	 * restart at the top left */
	[LEFT] = { SCREEN_H, SCREEN_W, 3, 0, 124, 430, 200, 70, 74, 84, 69,
	           { { 1262, 560 }, { 1322, 450 }, { 1382, 340 } }, 1320, 668, 140, 48 },
};

#define LAND_BACK_X 56 /* sideways: the back and restart buttons' centers */
#define LAND_RESTART_X 150
#define LAND_TOP_Y 56
#define LAND_ICON_D 76

static const struct layout *layout_of(int orient)
{
	return &layouts[orient == PORTRAIT ? PORTRAIT : LEFT];
}

/* Touches as the panel reports them (upright) into a layout's coordinates, as LVGL
 * turns its own back (lv_display_rotate_point_ccw(), see turn.c) */
static void from_screen(int orient, int sx, int sy, int *x, int *y)
{
	switch (orient) {
	case LEFT: *x = sy; *y = SCREEN_W - 1 - sx; break;
	case RIGHT: *x = SCREEN_H - 1 - sy; *y = sx; break;
	default: *x = sx; *y = sy; break;
	}
}

/* The game's size in a layout: 4:3-ish whatever the mode, lines doubled (tripled)
 * unless interlaced */
static void game_size(const struct layout *l, int w, int h, int *gw, int *gh)
{
	(void)w;
	*gw = 320 * l->scale;
	*gh = h <= 288 ? h * l->scale : h * l->scale / 2;
	if (*gh > l->h)
		*gh = l->h;
}

#define SRAM_SAVE_MS 30000 /* battery RAM written this often if changed, not only when leaving */
#define PAUSE_POLL_MS 20  /* while covered, how often the game looks whether it still is */

/* The core's functions, from dlopen */
static struct {
	void (*init)(void);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*run)(void);
	void (*reset)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} core;

static bool core_ready;

/* The keys drawn, in this order: arrows (dx, dy from the D-pad's center), the
 * buttons (A is the core's libretro Y, B its B, C its A), Start */
enum { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_C, PAD_START, PADS };

static const struct {
	unsigned id; /* RETRO_DEVICE_ID_JOYPAD_* */
	const char *text;
	int dx, dy;
} keys[PADS] = {
	{ RETRO_DEVICE_ID_JOYPAD_UP, LV_SYMBOL_UP, 0, -1 },
	{ RETRO_DEVICE_ID_JOYPAD_DOWN, LV_SYMBOL_DOWN, 0, 1 },
	{ RETRO_DEVICE_ID_JOYPAD_LEFT, LV_SYMBOL_LEFT, -1, 0 },
	{ RETRO_DEVICE_ID_JOYPAD_RIGHT, LV_SYMBOL_RIGHT, 1, 0 },
	{ RETRO_DEVICE_ID_JOYPAD_Y, "A", 0, 0 },
	{ RETRO_DEVICE_ID_JOYPAD_B, "B", 0, 0 },
	{ RETRO_DEVICE_ID_JOYPAD_A, "C", 0, 0 },
	{ RETRO_DEVICE_ID_JOYPAD_START, "START", 0, 0 },
};

static struct {
	/* The list */
	char **roms; /* file names, sorted */
	unsigned count;

	/* The game's screen (LVGL's thread), laid out again when the display turns */
	lv_obj_t *scr, *header, *body;
	lv_obj_t *view;
	lv_obj_t *controls;     /* the keys and, sideways, back and restart */
	lv_obj_t *key[PADS];
	lv_timer_t *timer;
	GLuint tex;
	int tex_w, tex_h;
	unsigned shown_seq, shown_pad;

	/* The game */
	char rom[PATH_MAX], state[PATH_MAX], srm[PATH_MAX];
	char title[256];
	pthread_t thread;
	bool running;
	double fps;
	int playing; /* its index in roms, -1: none */

	/* Shared with the thread */
	pthread_mutex_t lock;
	int frame_w, frame_h;
	unsigned seq;
	unsigned pad; /* buttons held, a bit per RETRO_DEVICE_ID_JOYPAD_* (atomic) */
	bool quit, reset; /* atomic */
	bool paused;      /* covered: the game waits (atomic) */
	int orient;       /* of the layout, for touches (atomic) */

	/* The thread's */
	struct ui_sound *sound;
	bool sounding;           /* the last write played: paced by the sound, else by the clock */
	int touch;
	int touch_max_x, touch_max_y, slot;
	struct {
		int id; /* -1: no finger */
		int x, y;
		bool new, dpad;
	} slots[SLOTS];
	struct timespec next; /* frame due, when paced by the clock */
	uint8_t sram_copy[0x10000];
	size_t sram_size;
	uint64_t sram_checked;
} gen = { .lock = PTHREAD_MUTEX_INITIALIZER, .touch = -1, .playing = -1 };

static uint16_t frame[MAX_W * MAX_H]; /* the newest, under gen.lock */

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

/* Files */

static void *file_read(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	void *data = NULL;
	struct stat st;
	if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && (data = malloc(st.st_size))) {
		if (fread(data, 1, st.st_size, f) == (size_t)st.st_size) {
			*size = st.st_size;
		} else {
			free(data);
			data = NULL;
		}
	}
	fclose(f);
	return data;
}

/* Written beside, then renamed over: a crash halfway keeps the previous one */
static bool file_write(const char *path, const void *data, size_t size)
{
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
	if (fclose(f) != 0)
		ok = false;
	if (ok && rename(tmp, path) == 0)
		return true;
	unlink(tmp);
	return false;
}

/* The core */

static bool core_load(void)
{
	static void *lib;
	if (lib)
		return true;

	/* lib/ next to this plugin */
	Dl_info info;
	char path[PATH_MAX];
	if (!dladdr((void *)core_load, &info) || !info.dli_fname)
		return false;
	snprintf(path, sizeof(path), "%s", info.dli_fname);
	char *slash = strrchr(path, '/');
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? slash + 1 - path : 0), "lib/" CORE);

	lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("genesis: %s\n", dlerror());
		return false;
	}
	struct {
		void **fn;
		const char *name;
	} syms[] = {
		{ (void **)&core.init, "retro_init" },
		{ (void **)&core.set_environment, "retro_set_environment" },
		{ (void **)&core.set_video_refresh, "retro_set_video_refresh" },
		{ (void **)&core.set_audio_sample, "retro_set_audio_sample" },
		{ (void **)&core.set_audio_sample_batch, "retro_set_audio_sample_batch" },
		{ (void **)&core.set_input_poll, "retro_set_input_poll" },
		{ (void **)&core.set_input_state, "retro_set_input_state" },
		{ (void **)&core.set_controller_port_device, "retro_set_controller_port_device" },
		{ (void **)&core.load_game, "retro_load_game" },
		{ (void **)&core.unload_game, "retro_unload_game" },
		{ (void **)&core.get_system_av_info, "retro_get_system_av_info" },
		{ (void **)&core.run, "retro_run" },
		{ (void **)&core.reset, "retro_reset" },
		{ (void **)&core.serialize_size, "retro_serialize_size" },
		{ (void **)&core.serialize, "retro_serialize" },
		{ (void **)&core.unserialize, "retro_unserialize" },
		{ (void **)&core.get_memory_data, "retro_get_memory_data" },
		{ (void **)&core.get_memory_size, "retro_get_memory_size" },
	};
	for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
		if (!(*syms[i].fn = dlsym(lib, syms[i].name))) {
			printf("genesis: %s has no %s\n", CORE, syms[i].name);
			dlclose(lib);
			lib = NULL;
			return false;
		}
	}
	return true;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
	if (level < RETRO_LOG_WARN)
		return;
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("genesis: %s", line);
	if (!*line || line[strlen(line) - 1] != '\n')
		putchar('\n');
}

static bool environment_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = HOME;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = SAVES;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		/* Every option at its default */
		((struct retro_variable *)data)->value = NULL;
		return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		gen.fps = ((const struct retro_system_av_info *)data)->timing.fps;
		return true;
	default:
		return false;
	}
}

/* The thread's callbacks */

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data || w > MAX_W || h > MAX_H)
		return; /* the frame is the same */
	pthread_mutex_lock(&gen.lock);
	for (unsigned y = 0; y < h; y++)
		memcpy(frame + y * w, (const uint8_t *)data + y * pitch, w * 2);
	gen.frame_w = w;
	gen.frame_h = h;
	__atomic_add_fetch(&gen.seq, 1, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&gen.lock);
}

static void audio_sample_cb(int16_t left, int16_t right)
{
	LV_UNUSED(left);
	LV_UNUSED(right);
}

static void input_poll_cb(void)
{
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	LV_UNUSED(index);
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	unsigned pad = __atomic_load_n(&gen.pad, __ATOMIC_RELAXED);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return pad;
	return (pad >> id) & 1;
}

/* Blocks while the device's buffer is full: this paces the game */
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	bool was = gen.sounding;
	gen.sounding = ui_sound_write(gen.sound, data, (unsigned)frames);
	if (was && !gen.sounding)
		clock_gettime(CLOCK_MONOTONIC, &gen.next); /* by the clock from now */
	return frames;
}

/* Touch */

static void touch_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && gen.touch < 0 && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		struct input_absinfo x, y;
		if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) == 0 &&
		    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) == 0 && x.maximum > 0) {
			gen.touch = fd;
			gen.touch_max_x = x.maximum;
			gen.touch_max_y = y.maximum;
		} else if (fd >= 0) {
			close(fd);
		}
	}
	if (d)
		closedir(d);
	if (gen.touch < 0)
		printf("genesis: no touchscreen\n");
	for (int i = 0; i < SLOTS; i++)
		gen.slots[i].id = -1;
	gen.slot = 0;
}

/* After a report: the buttons the fingers hold now, in the layout of the orientation */
static void touch_update(void)
{
	int orient = __atomic_load_n(&gen.orient, __ATOMIC_RELAXED);
	const struct layout *l = layout_of(orient);
	unsigned pad = 0;
	for (int i = 0; i < SLOTS; i++) {
		if (gen.slots[i].id < 0)
			continue;
		int x, y;
		from_screen(orient, gen.slots[i].x, gen.slots[i].y, &x, &y);
		int dx = x - l->dpad_x, dy = y - l->dpad_y;
		if (gen.slots[i].new) {
			gen.slots[i].new = false;
			gen.slots[i].dpad = dx * dx + dy * dy <= l->dpad_reach * l->dpad_reach;
		}
		if (gen.slots[i].dpad) {
			/* Eight directions, 45 degrees each: tan(22.5) = 0.414 */
			int ax = abs(dx), ay = abs(dy);
			if (ax * ax + ay * ay < DPAD_DEAD * DPAD_DEAD)
				continue;
			if (ax > ay * 414 / 1000)
				pad |= 1u << (dx < 0 ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_RIGHT);
			if (ay > ax * 414 / 1000)
				pad |= 1u << (dy < 0 ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_DOWN);
			continue;
		}
		for (int b = 0; b < 3; b++) {
			int bx = x - l->buttons[b].x, by = y - l->buttons[b].y;
			if (bx * bx + by * by <= l->button_hit * l->button_hit)
				pad |= 1u << keys[PAD_A + b].id;
		}
		if (abs(x - l->start_x) <= l->start_w / 2 + 20 && abs(y - l->start_y) <= l->start_h / 2 + 20)
			pad |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
	}
	__atomic_store_n(&gen.pad, pad, __ATOMIC_RELAXED);
}

static void touch_read(void)
{
	struct input_event ev;
	while (gen.touch >= 0 && read(gen.touch, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			touch_update();
			continue;
		}
		if (ev.type != EV_ABS)
			continue;
		if (ev.code == ABS_MT_SLOT) {
			gen.slot = ev.value >= 0 && ev.value < SLOTS ? ev.value : 0;
		} else if (ev.code == ABS_MT_TRACKING_ID) {
			gen.slots[gen.slot].new = ev.value >= 0 && gen.slots[gen.slot].id < 0;
			gen.slots[gen.slot].id = ev.value;
		} else if (ev.code == ABS_MT_POSITION_X) {
			gen.slots[gen.slot].x = ev.value * SCREEN_W / (gen.touch_max_x + 1);
		} else if (ev.code == ABS_MT_POSITION_Y) {
			gen.slots[gen.slot].y = ev.value * SCREEN_H / (gen.touch_max_y + 1);
		}
	}
}

/* Saves */

static void sram_load(void)
{
	void *ram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t max = core.get_memory_size(RETRO_MEMORY_SAVE_RAM), size;
	void *data = ram && max ? file_read(gen.srm, &size) : NULL;
	if (data) {
		memcpy(ram, data, size < max ? size : max);
		free(data);
	}
	gen.sram_size = 0;
	gen.sram_checked = now_ms();
}

/* If it changed since it was last written (or loaded) */
static void sram_save(void)
{
	void *ram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!ram || !size || size > sizeof(gen.sram_copy))
		return;
	if (size == gen.sram_size && memcmp(ram, gen.sram_copy, size) == 0)
		return;
	if (gen.sram_size == 0) {
		/* Nothing written yet this time: it may be what was loaded */
		size_t old_size;
		void *old = file_read(gen.srm, &old_size);
		bool same = old && old_size == size && memcmp(old, ram, size) == 0;
		free(old);
		if (same) {
			memcpy(gen.sram_copy, ram, size);
			gen.sram_size = size;
			return;
		}
	}
	if (file_write(gen.srm, ram, size)) {
		memcpy(gen.sram_copy, ram, size);
		gen.sram_size = size;
	} else {
		printf("genesis: cannot write %s\n", gen.srm);
	}
}

static void state_load(void)
{
	size_t size;
	void *data = file_read(gen.state, &size);
	if (!data)
		return;
	if (!core.unserialize(data, size))
		printf("genesis: cannot resume from %s\n", gen.state);
	free(data);
}

static void state_save(void)
{
	size_t size = core.serialize_size();
	void *data = size ? malloc(size) : NULL;
	if (data && core.serialize(data, size) && file_write(gen.state, data, size))
		printf("genesis: saved %s\n", gen.state);
	else
		printf("genesis: cannot save %s\n", gen.state);
	free(data);
}

/* The thread */

static void timespec_add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000) {
		t->tv_nsec -= 1000000000;
		t->tv_sec++;
	}
}

/* While covered (the lock screen, the quick settings): nothing held, the sound stopped,
 * touches meant for what covers it dropped; then on from where it was */
static void pause_wait(void)
{
	if (!__atomic_load_n(&gen.paused, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&gen.pad, 0, __ATOMIC_RELAXED);
	ui_sound_release(gen.sound); /* let go meanwhile */
	while (__atomic_load_n(&gen.paused, __ATOMIC_ACQUIRE) && !__atomic_load_n(&gen.quit, __ATOMIC_ACQUIRE))
		usleep(PAUSE_POLL_MS * 1000);
	struct input_event ev;
	while (gen.touch >= 0 && read(gen.touch, &ev, sizeof(ev)) == sizeof(ev))
		;
	for (int i = 0; i < SLOTS; i++)
		gen.slots[i].id = -1;
	clock_gettime(CLOCK_MONOTONIC, &gen.next);
}

static void *game_main(void *arg)
{
	LV_UNUSED(arg);
	clock_gettime(CLOCK_MONOTONIC, &gen.next);
	while (!__atomic_load_n(&gen.quit, __ATOMIC_ACQUIRE)) {
		pause_wait();
		if (__atomic_exchange_n(&gen.reset, false, __ATOMIC_ACQ_REL))
			core.reset();
		touch_read();
		core.run(); /* video_cb, audio_batch_cb (which blocks) */

		if (now_ms() - gen.sram_checked >= SRAM_SAVE_MS) {
			gen.sram_checked = now_ms();
			sram_save();
		}
		if (gen.sounding)
			continue;
		/* No sound: by the clock, catching up at most a few frames */
		timespec_add_ns(&gen.next, (long)(1e9 / gen.fps));
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - gen.next.tv_sec > 0)
			gen.next = now;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &gen.next, NULL);
	}
	return NULL;
}

/* The game's screen */

static void key_set_pressed(lv_obj_t *obj, bool pressed)
{
	lv_obj_set_style_bg_color(obj, lv_color_hex(pressed ? UI_COLOR_BUTTON_PRESSED : UI_COLOR_BUTTON), 0);
}

/* A w x h rounded box centered on (x, y) of the layout, in `parent`, whose top is at
 * y `origin` of the layout */
static lv_obj_t *box_create(lv_obj_t *parent, int origin, int x, int y, int w, int h, int32_t radius)
{
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_pos(obj, x - w / 2, y - h / 2 - origin);
	lv_obj_set_size(obj, w, h);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	key_set_pressed(obj, false);
	return obj;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_center(label);
	return label;
}

static void restart_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	__atomic_store_n(&gen.reset, true, __ATOMIC_RELEASE);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

/* Sideways (no header): a round button with a symbol at the top left */
static void side_button(int x, const char *symbol, lv_event_cb_t cb)
{
	lv_obj_t *b = box_create(gen.controls, 0, x, LAND_TOP_Y, LAND_ICON_D, LAND_ICON_D, LV_RADIUS_CIRCLE);
	lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_clickable(b, true);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	label_create(b, symbol, &ui_font_26);
}

/* The keys of a layout and, sideways, back and restart, in gen.controls (made again) */
static void controls_create(int orient, int origin)
{
	const struct layout *l = layout_of(orient);
	if (gen.controls)
		lv_obj_delete(gen.controls);
	gen.controls = ui_box(gen.body);
	lv_obj_set_size(gen.controls, LV_PCT(100), LV_PCT(100));
	for (int i = PAD_UP; i <= PAD_RIGHT; i++) {
		gen.key[i] = box_create(gen.controls, origin, l->dpad_x + keys[i].dx * l->dpad_off,
		                        l->dpad_y + keys[i].dy * l->dpad_off, l->dpad_key, l->dpad_key, 20);
		label_create(gen.key[i], keys[i].text, &ui_font_32);
	}
	bool side = orient != PORTRAIT;
	for (int b = 0; b < 3; b++) {
		gen.key[PAD_A + b] = box_create(gen.controls, origin, l->buttons[b].x, l->buttons[b].y, l->button_d,
		                                l->button_d, LV_RADIUS_CIRCLE);
		label_create(gen.key[PAD_A + b], keys[PAD_A + b].text, side ? &ui_font_32 : &ui_font_48);
	}
	gen.key[PAD_START] = box_create(gen.controls, origin, l->start_x, l->start_y, l->start_w, l->start_h,
	                                LV_RADIUS_CIRCLE);
	label_create(gen.key[PAD_START], keys[PAD_START].text, side ? &ui_font_20 : &ui_font_24);
	if (side) {
		side_button(LAND_BACK_X, LV_SYMBOL_LEFT, back_cb);
		side_button(LAND_RESTART_X, LV_SYMBOL_REFRESH, restart_cb);
	}
	gen.shown_pad = ~0u; /* drawn as pressed or not on the next frame */
}

/* The game at its size in the layout, once there is a frame */
static void view_place(int orient)
{
	if (!gen.tex_w)
		return;
	const struct layout *l = layout_of(orient);
	int gw, gh;
	game_size(l, gen.tex_w, gen.tex_h, &gw, &gh);
	if (orient == PORTRAIT)
		lv_obj_set_pos(gen.view, (l->w - gw) / 2, l->game_y - BODY_Y);
	else
		lv_obj_set_pos(gen.view, (l->w - gw) / 2, (l->h - gh) / 2);
	lv_obj_set_size(gen.view, gw, gh);
	lv_obj_set_hidden(gen.view, false);
}

/* The orientation the display is turned to */
static int orient_now(void)
{
	switch (lv_display_get_rotation(lv_display_get_default())) {
	case LV_DISPLAY_ROTATION_90: return LEFT;
	case LV_DISPLAY_ROTATION_270: return RIGHT;
	default: return PORTRAIT;
	}
}

/* As the display is turned: upright, below the status bar and the header; sideways,
 * all of the screen (phone-ui hides the status bar), black */
static void game_layout(void)
{
	int o = orient_now();
	bool side = o != PORTRAIT;
	lv_obj_set_hidden(gen.header, side);
	lv_obj_set_style_pad_top(lv_obj_get_parent(gen.body), side ? 0 : UI_STATUS_H, 0);
	lv_obj_set_style_bg_color(gen.body, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(gen.body, side ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	controls_create(o, side ? 0 : BODY_Y);
	view_place(o);
	__atomic_store_n(&gen.orient, o, __ATOMIC_RELAXED);
}

static void resolution_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (gen.scr)
		game_layout();
}

/* Each frame the thread made, into the texture; the pressed keys */
static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	/* Covered (the lock screen, asleep or woken, the quick settings): the game waits */
	bool covered = ui_current() != gen.scr || ui_locked() || ui_shade_shown();
	__atomic_store_n(&gen.paused, covered, __ATOMIC_RELEASE);
	if (covered)
		return;
	unsigned pad = __atomic_load_n(&gen.pad, __ATOMIC_RELAXED);
	if (pad != gen.shown_pad) {
		for (int i = 0; i < PADS; i++)
			key_set_pressed(gen.key[i], pad >> keys[i].id & 1);
		gen.shown_pad = pad;
	}

	if (__atomic_load_n(&gen.seq, __ATOMIC_RELAXED) == gen.shown_seq)
		return;
	pthread_mutex_lock(&gen.lock);
	int w = gen.frame_w, h = gen.frame_h;
	bool created = !gen.tex;
	if (created) {
		glGenTextures(1, &gen.tex);
		glBindTexture(GL_TEXTURE_2D, gen.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	} else {
		glBindTexture(GL_TEXTURE_2D, gen.tex);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
	if (w != gen.tex_w || h != gen.tex_h)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame);
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	gen.shown_seq = gen.seq;
	pthread_mutex_unlock(&gen.lock);

	if (created)
		lv_3dtexture_set_src(gen.view, gen.tex);
	if (w != gen.tex_w || h != gen.tex_h) {
		gen.tex_w = w;
		gen.tex_h = h;
		view_place(orient_now());
	}
	lv_obj_invalidate(gen.view);
}

static void list_game_left(void);

static void game_stop(void)
{
	if (!gen.running)
		return;
	__atomic_store_n(&gen.quit, true, __ATOMIC_RELEASE);
	pthread_join(gen.thread, NULL);
	gen.running = false;
	state_save();
	sram_save();
	core.unload_game();
	ui_sound_close(gen.sound);
	gen.sound = NULL;
	gen.sounding = false;
	if (gen.touch >= 0)
		close(gen.touch);
	gen.touch = -1;
	__atomic_store_n(&gen.pad, 0, __ATOMIC_RELAXED);
}

static void game_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_display_remove_event_cb_with_user_data(lv_display_get_default(), resolution_cb, NULL);
	game_stop();
	if (gen.timer)
		lv_timer_delete(gen.timer);
	gen.timer = NULL;
	if (gen.tex)
		glDeleteTextures(1, &gen.tex);
	gen.tex = 0;
	gen.scr = gen.header = gen.body = gen.view = gen.controls = NULL;
	memset(gen.key, 0, sizeof(gen.key));
	ui_cpu_boost(false);
	list_game_left();
}

static bool game_start(void)
{
	if (!core_load())
		return false;
	if (!core_ready) {
		core.set_environment(environment_cb);
		core.init();
		core.set_video_refresh(video_cb);
		core.set_audio_sample(audio_sample_cb);
		core.set_audio_sample_batch(audio_batch_cb);
		core.set_input_poll(input_poll_cb);
		core.set_input_state(input_state_cb);
		core_ready = true;
	}
	mkdir(SAVES, 0755);
	struct retro_game_info info = { .path = gen.rom };
	if (!core.load_game(&info)) {
		printf("genesis: cannot load %s\n", gen.rom);
		return false;
	}
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	struct retro_system_av_info av;
	core.get_system_av_info(&av);
	gen.fps = av.timing.fps > 0 ? av.timing.fps : 60;
	sram_load();
	state_load();

	gen.sound = ui_sound_open("genesis", RATE, 2, PERIOD * PERIODS * 1000000LL / RATE);
	touch_open();
	gen.quit = gen.reset = false;
	if (pthread_create(&gen.thread, NULL, game_main, NULL) != 0) {
		core.unload_game();
		return false;
	}
	gen.running = true;
	printf("genesis: playing %s at %.2f fps\n", gen.rom, gen.fps);
	return true;
}

static void game_open(const char *file)
{
	snprintf(gen.rom, sizeof(gen.rom), ROMS "/%s", file);
	snprintf(gen.title, sizeof(gen.title), "%s", file);
	char *dot = strrchr(gen.title, '.');
	if (dot && dot != gen.title)
		*dot = '\0';
	snprintf(gen.state, sizeof(gen.state), SAVES "/%s.state", gen.title);
	snprintf(gen.srm, sizeof(gen.srm), SAVES "/%s.srm", gen.title);

	gen.scr = ui_app_screen_create(gen.title, &gen.header, &gen.body);
	ui_header_button(gen.header, LV_SYMBOL_REFRESH, restart_cb, NULL);
	lv_obj_add_event_cb(gen.scr, game_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_layout(gen.body, LV_LAYOUT_NONE);
	lv_obj_set_scrollable(gen.body, false);

	gen.view = lv_3dtexture_create(gen.body);
	lv_obj_set_clickable(gen.view, false);
	lv_obj_set_hidden(gen.view, true); /* until the first frame */
	lv_3dtexture_set_rb_swap(gen.view, true); /* RGB565, red in the high bits */
	gen.tex_w = gen.tex_h = 0;
	gen.shown_seq = gen.seq;
	gen.controls = NULL;

	/* Turns with the phone, sideways without the status bar */
	ui_screen_turns(gen.scr, true);
	game_layout();
	lv_display_add_event_cb(lv_display_get_default(), resolution_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);

	if (!game_start()) {
		lv_obj_t *label = ui_empty_label(gen.body, _("Cannot start this game"));
		lv_obj_set_pos(label, 0, 0);
	} else {
		gen.timer = lv_timer_create(frame_cb, 4, NULL);
		ui_cpu_boost_when_busy(gen.thread); /* the highest once the emulation is too much */
	}
	ui_push(gen.scr);
}

/* The list */

static bool has_ext(const char *name, const char *ext)
{
	const char *dot = strrchr(name, '.');
	return dot && strcasecmp(dot + 1, ext) == 0;
}

static bool rom_wanted(const char *name)
{
	/* Cartridges, then discs (Sega CD, which needs a BIOS: `just bios`) and
	 * playlists of them (one game on several discs) */
	static const char *const exts[] = { "md", "gen", "smd", "bin", "sms", "gg", "sg", "68k",
	                                    "cue", "chd", "iso", "m3u" };
	if (name[0] == '.')
		return false;
	for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (has_ext(name, exts[i]))
			return true;
	return false;
}

/* Takes `name` off the list (not sorted yet): a disc's track or a playlist's disc,
 * not a game itself */
static void rom_hide(const char *name)
{
	for (unsigned i = 0; i < gen.count; i++) {
		if (strcmp(gen.roms[i], name) == 0) {
			free(gen.roms[i]);
			gen.roms[i] = gen.roms[--gen.count];
			return;
		}
	}
}

/* The files a .cue (FILE "track.bin" BINARY) or .m3u (a file per line) names */
static void rom_hide_parts(const char *list)
{
	char path[PATH_MAX], line[PATH_MAX];
	snprintf(path, sizeof(path), ROMS "/%s", list);
	FILE *f = fopen(path, "r");
	bool cue = has_ext(list, "cue");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *name = line;
		if (cue) {
			while (*name == ' ' || *name == '\t')
				name++;
			if (strncasecmp(name, "FILE ", 5) != 0)
				continue;
			name += 5;
			char *end = *name == '"' ? strchr(++name, '"') : strchr(name, ' ');
			if (end)
				*end = '\0';
		} else if (name[0] == '#') {
			continue;
		}
		char *slash = strrchr(name, '/');
		rom_hide(slash ? slash + 1 : name);
	}
	if (f)
		fclose(f);
}

static int rom_compare(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void roms_free(void)
{
	for (unsigned i = 0; i < gen.count; i++)
		free(gen.roms[i]);
	free(gen.roms);
	gen.roms = NULL;
	gen.count = 0;
}

static void roms_scan(void)
{
	roms_free();
	DIR *d = opendir(ROMS);
	struct dirent *e;
	unsigned cap = 0;
	while (d && (e = readdir(d))) {
		if (!rom_wanted(e->d_name))
			continue;
		if (gen.count == cap) {
			cap = cap ? cap * 2 : 32;
			gen.roms = realloc(gen.roms, cap * sizeof(*gen.roms));
		}
		gen.roms[gen.count++] = strdup(e->d_name);
	}
	if (d)
		closedir(d);
	/* What .cue and .m3u files name, all of them read before any is hidden */
	char **lists = malloc((gen.count ? gen.count : 1) * sizeof(*lists));
	unsigned n = 0;
	for (unsigned i = 0; i < gen.count; i++)
		if (has_ext(gen.roms[i], "cue") || has_ext(gen.roms[i], "m3u"))
			lists[n++] = strdup(gen.roms[i]);
	for (unsigned i = 0; i < n; i++) {
		rom_hide_parts(lists[i]);
		free(lists[i]);
	}
	free(lists);
	if (gen.count)
		qsort(gen.roms, gen.count, sizeof(*gen.roms), rom_compare);
}

/* The names shown: the file names without their extension; their rows */
static char **titles;
static lv_obj_t **rows;

static lv_obj_t *list;

static void titles_free(void)
{
	for (unsigned i = 0; titles && i < gen.count; i++)
		free(titles[i]);
	free(titles);
	free(rows);
	titles = NULL;
	rows = NULL;
}

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	titles_free();
	roms_free();
	list = NULL;
}

/* "Saved" after the name, once there is a save state */
static void row_update(unsigned i)
{
	char state[PATH_MAX];
	snprintf(state, sizeof(state), SAVES "/%s.state", titles[i]);
	if (lv_obj_get_child_count(rows[i]) == 1 && access(state, F_OK) == 0)
		ui_dim_label(rows[i], _("Saved"), UI_FONT_CAPTION);
}

static void list_game_left(void)
{
	if (rows && gen.playing >= 0 && (unsigned)gen.playing < gen.count)
		row_update(gen.playing);
	gen.playing = -1;
}

static void rom_cb(lv_event_t *e)
{
	gen.playing = (uintptr_t)lv_event_get_user_data(e);
	game_open(gen.roms[gen.playing]);
}

/* The games in ROMS, read (again) into the list */
static void list_fill(void)
{
	titles_free();
	roms_scan();
	lv_obj_clean(list);

	if (!gen.count) {
		ui_empty_label(list, _("No games"));
		char copy[128];
		snprintf(copy, sizeof(copy), _("Copy ROMs to %s"), ROMS);
		lv_obj_t *hint = ui_dim_label(list, ui_intern(copy), UI_FONT_CAPTION); /* static text: kept */
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
		return;
	}

	titles = calloc(gen.count, sizeof(*titles));
	rows = calloc(gen.count, sizeof(*rows));
	lv_obj_t *card = ui_card_create(list);
	for (unsigned i = 0; i < gen.count; i++) {
		titles[i] = strdup(gen.roms[i]);
		char *dot = strrchr(titles[i], '.');
		if (dot && dot != titles[i])
			*dot = '\0';
		rows[i] = ui_row_create(card, 96, i + 1 < gen.count, rom_cb, (void *)(uintptr_t)i);
		lv_obj_t *name = ui_one_line_label(rows[i], titles[i], UI_FONT_TITLE);
		lv_obj_set_flex_grow(name, 1);
		row_update(i);
	}
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	list_fill();
	ui_pull_refresh_done(list);
}

static void list_open(void)
{
	mkdir(HOME, 0755);
	mkdir(ROMS, 0755);

	lv_obj_t *scr, *body;
	scr = ui_app_screen_create("Genesis", NULL, &body); /* NON-NLS: app.json's name */
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);
	ui_screen_turns(scr, false); /* a flex column: laid out again by itself */

	list = ui_box(body);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_top(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	ui_pull_refresh(list, pull_cb, NULL);
	list_fill();
	ui_push(scr);
}

/* What phone-ui looks for (see external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	list_open();
}
