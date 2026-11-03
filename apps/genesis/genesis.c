/*
 * Genesis: Sega Genesis / Mega Drive, Sega CD (and Master System, Game Gear, SG-1000)
 * games, a plugin app (see external.c in phone-ui) around Genesis Plus GX's libretro core.
 *
 * Games are ROM files or disc images (.cue with its tracks, .chd, .iso; .m3u for
 * several discs) in /var/lib/genesis/roms, listed by name. Held upright, as a
 * handheld: the game at the top, twice its size (640 px wide, 448 or 480 high;
 * double-resolution interlaced modes at their size), and the controls below it:
 *
 *        ^             C
 *     <     >       B
 *        v       A
 *          START
 *
 * Turned sideways (either way, from the accelerometer), three times its size (960
 * px wide) in the middle, the D-pad on the left, the buttons and Start on the
 * right, back and restart at the top left. LVGL cannot turn the display: the frame
 * is turned as it is copied, and the controls are placed and their text turned.
 *
 * The D-pad goes by the angle from its center (diagonals in between), from wherever
 * the finger that started on it goes. A finger on the buttons presses the ones it
 * is near: between two, both. Several fingers at once: the touchscreen is read here
 * (LVGL only sees one pointer), next to LVGL, which ignores the controls.
 *
 * The core (genesis_plus_gx_libretro.so in lib/) is loaded when the app first opens
 * and stays: it is a single instance. It runs in a thread, paced by the sound
 * output (48 kHz, as Doom's), or by the clock when the output is busy or missing.
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
#include <sound/asound.h>

#include "abi.h"
#include "libretro.h"
#include "ui.h"

#define HOME "/var/lib/genesis"
#define ROMS HOME "/roms"
#define SAVES HOME "/saves"
#define CORE "genesis_plus_gx_libretro.so"

#define PCM "/dev/snd/pcmC0D0p"
#define RATE 48000 /* the core's, changed from 44100 by the Makefile */
#define PERIOD 512
#define PERIODS 4

/* Frames as the core makes them: its bitmap is 720 wide, at most 576 high (PAL,
 * interlaced) */
#define MAX_W 720
#define MAX_H 576

#define SCREEN_W 720
#define SCREEN_H 1440
#define BODY_Y (UI_STATUS_H + UI_HEADER_H) /* where the screen's body starts */
#define DPAD_DEAD 28 /* no direction this close to the D-pad's center */
#define SLOTS 10

/* The accelerometer, read this often by the game's thread; a new orientation is
 * taken once it held for ORIENT_READS reads */
#define ACCEL_MS 200
#define ORIENT_READS 2

/*
 * Orientations: upright, or sideways with the phone's top to the left (Power and
 * the volume keys on top, as Doom) or to the right. A layout is in the coordinates
 * of its orientation (x to the right, y down, as the user sees it); sideways, the
 * screen's 1440 x 720.
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

/* From a layout's coordinates to the screen's, and back */
static void to_screen(int orient, int x, int y, int *sx, int *sy)
{
	switch (orient) {
	case LEFT: *sx = SCREEN_W - y; *sy = x; break;
	case RIGHT: *sx = y; *sy = SCREEN_H - x; break;
	default: *sx = x; *sy = y; break;
	}
}

static void from_screen(int orient, int sx, int sy, int *x, int *y)
{
	switch (orient) {
	case LEFT: *x = sy; *y = SCREEN_W - sx; break;
	case RIGHT: *x = SCREEN_H - sy; *y = sx; break;
	default: *x = sx; *y = sy; break;
	}
}

/* A w x h rectangle centered on (x, y) of a layout, on the screen */
static lv_area_t rect_to_screen(int orient, int x, int y, int w, int h)
{
	int x1, y1, x2, y2;
	to_screen(orient, x - w / 2, y - h / 2, &x1, &y1);
	to_screen(orient, x - w / 2 + w, y - h / 2 + h, &x2, &y2);
	lv_area_t a = { LV_MIN(x1, x2), LV_MIN(y1, y2), LV_MAX(x1, x2), LV_MAX(y1, y2) };
	return a;
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

	/* The game's screen (LVGL's thread): upright, its keys in the body; sideways,
	 * another view and keys in `land`, on LVGL's top layer over the status bar and
	 * the back gesture's strip (black, all of the screen) */
	lv_obj_t *scr, *body, *land;
	lv_obj_t *view[2];      /* upright, sideways */
	lv_obj_t *key[2][PADS];
	unsigned land_tex[16];  /* textures of `land`'s turned text */
	unsigned land_tex_count;
	lv_timer_t *timer;
	GLuint tex;
	int tex_w, tex_h;       /* as uploaded (turned sideways: the game's height, width) */
	int game_w, game_h;     /* of the frame shown, as the core made it */
	int shown_orient;       /* of the frame shown, and of the controls */
	unsigned shown_seq, shown_pad;
	uint32_t boosted;

	/* The game */
	char rom[PATH_MAX], state[PATH_MAX], srm[PATH_MAX];
	char title[256];
	pthread_t thread;
	bool running;
	double fps;
	int playing; /* its index in roms, -1: none */

	/* Shared with the thread */
	pthread_mutex_t lock;
	int frame_w, frame_h; /* as the core made it */
	int frame_orient;     /* turned for this orientation */
	unsigned seq;
	unsigned pad; /* buttons held, a bit per RETRO_DEVICE_ID_JOYPAD_* (atomic) */
	bool quit, reset; /* atomic */

	/* The thread's */
	int orient;
	int orient_next, orient_reads; /* a new orientation, read this many times */
	int accel[3];                  /* /sys/.../in_accel_{x,y,z}_raw, -1: none */
	float accel_m[2][3];           /* mount matrix rows: the phone's x, y */
	uint64_t accel_read;
	int pcm, touch;
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
} gen = { .lock = PTHREAD_MUTEX_INITIALIZER, .pcm = -1, .touch = -1, .playing = -1 };

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
	const uint16_t *src = data;
	size_t stride = pitch / 2;
	if (gen.orient == PORTRAIT) {
		for (unsigned y = 0; y < h; y++)
			memcpy(frame + y * w, src + y * stride, w * 2);
	} else {
		/* Turned sideways (h wide, w high): its top to the screen's right edge
		 * (the phone's top to the left) or to its left */
		bool left = gen.orient == LEFT;
		for (unsigned y = 0; y < h; y++) {
			const uint16_t *row = src + y * stride;
			uint16_t *out = left ? frame + (h - 1 - y) : frame + y + (size_t)(w - 1) * h;
			ptrdiff_t step = left ? (ptrdiff_t)h : -(ptrdiff_t)h;
			for (unsigned x = 0; x < w; x++, out += step)
				*out = row[x];
		}
	}
	gen.frame_w = w;
	gen.frame_h = h;
	gen.frame_orient = gen.orient;
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

/* Sound: the codec's ALSA device written directly, as in apps/doom/sound.c */

static void param_mask(struct snd_pcm_hw_params *p, int param, unsigned value)
{
	struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
	memset(m, 0, sizeof(*m));
	m->bits[value >> 5] = 1u << (value & 31);
}

static void param_int(struct snd_pcm_hw_params *p, int param, unsigned value)
{
	struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
	i->min = i->max = value;
	i->integer = 1;
}

static void pcm_open(void)
{
	gen.pcm = open(PCM, O_WRONLY | O_CLOEXEC);
	if (gen.pcm < 0) {
		printf("genesis: no sound: " PCM ": %s\n", strerror(errno));
		return;
	}
	struct snd_pcm_hw_params p;
	memset(&p, 0, sizeof(p));
	for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&p.masks[i], 0xff, sizeof(p.masks[i]));
	for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		p.intervals[i].max = ~0u;
	p.rmask = ~0u;
	p.info = ~0u;
	param_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	param_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	param_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	param_int(&p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	param_int(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
	param_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	param_int(&p, SNDRV_PCM_HW_PARAM_RATE, RATE);
	param_int(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	param_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(gen.pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0 || ioctl(gen.pcm, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		printf("genesis: no sound: setting up " PCM ": %s\n", strerror(errno));
		close(gen.pcm);
		gen.pcm = -1;
	}
}

/* Blocks while the device's buffer is full: this paces the game */
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	size_t left = frames;
	while (gen.pcm >= 0 && left) {
		struct snd_xferi x = { .buf = (void *)data, .frames = left };
		if (ioctl(gen.pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
			/* Underrun, or after the phone slept: start again */
			if (errno == EPIPE || errno == ESTRPIPE) {
				ioctl(gen.pcm, SNDRV_PCM_IOCTL_PREPARE);
			} else if (errno != EINTR && errno != EAGAIN) {
				printf("genesis: sound stopped: %s\n", strerror(errno));
				close(gen.pcm);
				gen.pcm = -1;
				clock_gettime(CLOCK_MONOTONIC, &gen.next);
			}
			continue;
		}
		data += x.result * 2;
		left -= x.result;
	}
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
	const struct layout *l = layout_of(gen.orient);
	unsigned pad = 0;
	for (int i = 0; i < SLOTS; i++) {
		if (gen.slots[i].id < 0)
			continue;
		int x, y;
		from_screen(gen.orient, gen.slots[i].x, gen.slots[i].y, &x, &y);
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

/* The accelerometer (MPU6050), read through sysfs: a few reads a second need no
 * buffer (Compass uses one, but it is not open at the same time) */

static void accel_open(void)
{
	static const char axis[3] = { 'x', 'y', 'z' };
	char path[300];
	DIR *d = opendir("/sys/bus/iio/devices");
	struct dirent *e;
	for (int i = 0; i < 3; i++)
		gen.accel[i] = -1;
	while (d && gen.accel[0] < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10) != 0)
			continue;
		for (int i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/in_accel_%c_raw", e->d_name, axis[i]);
			gen.accel[i] = open(path, O_RDONLY | O_CLOEXEC);
		}
		if (gen.accel[0] < 0 || gen.accel[1] < 0 || gen.accel[2] < 0) {
			for (int i = 0; i < 3; i++) {
				if (gen.accel[i] >= 0)
					close(gen.accel[i]);
				gen.accel[i] = -1;
			}
			continue;
		}
		/* To the phone's axes (x to the right, y to the top): identity if none */
		float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/in_accel_mount_matrix", e->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			float r[9];
			if (fscanf(f, "%f, %f, %f; %f, %f, %f; %f, %f, %f", &r[0], &r[1], &r[2], &r[3], &r[4],
			           &r[5], &r[6], &r[7], &r[8]) == 9)
				memcpy(m, r, sizeof(m));
			fclose(f);
		}
		for (int i = 0; i < 6; i++)
			gen.accel_m[i / 3][i % 3] = m[i];
	}
	if (d)
		closedir(d);
	if (gen.accel[0] < 0)
		printf("genesis: no accelerometer, upright only\n");
}

static void accel_close(void)
{
	for (int i = 0; i < 3; i++) {
		if (gen.accel[i] >= 0)
			close(gen.accel[i]);
		gen.accel[i] = -1;
	}
}

/* The orientation gravity shows; `current` while the phone lies flat, is upside
 * down or between two */
static int accel_orient(int current)
{
	float raw[3];
	for (int i = 0; i < 3; i++) {
		char buf[32];
		ssize_t n = gen.accel[i] >= 0 ? pread(gen.accel[i], buf, sizeof(buf) - 1, 0) : -1;
		if (n <= 0)
			return current;
		buf[n] = '\0';
		raw[i] = strtof(buf, NULL);
	}
	/* Upward, in the phone's axes (x to the right, y to the top). The mount matrix
	 * gives x and y the other way around in this reading (tested: sideways turned
	 * the wrong way, upright read as upside down), so both are negated. */
	float x = 0, y = 0;
	for (int i = 0; i < 3; i++) {
		x -= gen.accel_m[0][i] * raw[i];
		y -= gen.accel_m[1][i] * raw[i];
	}
	float g2 = raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2];
	if (x * x + y * y < g2 * 0.25f)
		return current; /* within 30 degrees of flat */
	if (y > fabsf(x) * 1.3f)
		return PORTRAIT;
	if (x > fabsf(y) * 1.3f)
		return LEFT; /* the right edge up: the top to the left */
	if (-x > fabsf(y) * 1.3f)
		return RIGHT;
	return current;
}

/* Every ACCEL_MS in the game's thread: the new orientation once it held */
static void accel_update(void)
{
	uint64_t now = now_ms();
	if (gen.accel[0] < 0 || now - gen.accel_read < ACCEL_MS)
		return;
	gen.accel_read = now;
	int o = accel_orient(gen.orient);
	if (o == gen.orient) {
		gen.orient_reads = 0;
		return;
	}
	if (o != gen.orient_next) {
		gen.orient_next = o;
		gen.orient_reads = 0;
	}
	if (++gen.orient_reads >= ORIENT_READS) {
		gen.orient = o;
		gen.orient_reads = 0;
		touch_update(); /* the fingers down, in the new layout */
	}
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

static void *game_main(void *arg)
{
	LV_UNUSED(arg);
	clock_gettime(CLOCK_MONOTONIC, &gen.next);
	while (!__atomic_load_n(&gen.quit, __ATOMIC_ACQUIRE)) {
		if (__atomic_exchange_n(&gen.reset, false, __ATOMIC_ACQ_REL))
			core.reset();
		accel_update();
		touch_read();
		core.run(); /* video_cb, audio_batch_cb (which blocks) */

		if (now_ms() - gen.sram_checked >= SRAM_SAVE_MS) {
			gen.sram_checked = now_ms();
			sram_save();
		}
		if (gen.pcm >= 0)
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

/* The next character of UTF-8 text, 0 at its end */
static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	if (!*p)
		return 0;
	int n = *p >= 0xf0 ? 3 : *p >= 0xe0 ? 2 : *p >= 0xc0 ? 1 : 0;
	uint32_t c = n ? *p & (0x3f >> n) : *p;
	for (p++; n > 0 && (*p & 0xc0) == 0x80; n--, p++)
		c = c << 6 | (*p & 0x3f);
	*s = (const char *)p;
	return c;
}

/*
 * Sideways, text turned with the layout. Not with a transform style: the OpenGL
 * ES draw unit fills turned labels' layers with something else (the performance
 * monitor's text). Drawn here instead, glyph by glyph from the font (symbols from
 * its fallback), turned, into a texture (gen.land_tex, deleted with `land`).
 */
static lv_obj_t *text_turned_create(lv_obj_t *parent, int orient, const char *text, const lv_font_t *font)
{
	int lh = lv_font_get_line_height(font), base = font->base_line;
	int w = 0;
	for (const char *s = text; *s;) {
		uint32_t c = utf8_next(&s);
		const char *t = s;
		w += lv_font_get_glyph_width(font, c, utf8_next(&t));
	}
	if (w <= 0 || lh <= 0 || gen.land_tex_count >= sizeof(gen.land_tex) / sizeof(gen.land_tex[0]))
		return NULL;

	uint8_t *a8 = calloc((size_t)w * lh, 1);
	int pen = 0;
	for (const char *s = text; *s;) {
		uint32_t c = utf8_next(&s);
		const char *t = s;
		lv_font_glyph_dsc_t g;
		memset(&g, 0, sizeof(g));
		if (!lv_font_get_glyph_dsc(font, &g, c, utf8_next(&t)))
			continue;
		if (g.box_w && g.box_h) {
			lv_draw_buf_t *buf = lv_draw_buf_create(g.box_w, g.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
			/* The draw buffer it decoded into (A8), or the raw bitmap for fonts
			 * that keep them decoded */
			const void *r = buf ? lv_font_get_glyph_bitmap(&g, buf) : NULL;
			const uint8_t *bm = r == (const void *)buf ? buf->data : r;
			int stride = r == (const void *)buf ? (int)buf->header.stride : g.box_w;
			int x0 = pen + g.ofs_x, y0 = lh - base - g.box_h - g.ofs_y;
			for (int y = 0; bm && y < g.box_h; y++)
				for (int x = 0; x < g.box_w; x++)
					if (x0 + x >= 0 && x0 + x < w && y0 + y >= 0 && y0 + y < lh)
						a8[(y0 + y) * w + x0 + x] = bm[y * stride + x];
			lv_font_glyph_release_draw_data(&g);
			if (buf)
				lv_draw_buf_destroy(buf);
		}
		pen += g.adv_w;
	}

	/* Turned as the frame is (video_cb): lh wide, w high; BGRA in the text color */
	uint32_t color = UI_COLOR_TEXT;
	uint8_t *bgra = malloc((size_t)w * lh * 4);
	for (int v = 0; v < lh; v++) {
		for (int u = 0; u < w; u++) {
			int px = orient == LEFT ? lh - 1 - v : v, py = orient == LEFT ? u : w - 1 - u;
			uint8_t *p = bgra + ((size_t)py * lh + px) * 4;
			p[0] = color & 0xff;
			p[1] = color >> 8 & 0xff;
			p[2] = color >> 16 & 0xff;
			p[3] = a8[v * w + u];
		}
	}
	unsigned tex = ui_texture_create(bgra, lh, w);
	free(a8);
	free(bgra);
	if (!tex)
		return NULL;
	gen.land_tex[gen.land_tex_count++] = tex;
	lv_obj_t *obj = lv_3dtexture_create(parent);
	lv_3dtexture_set_src(obj, tex);
	lv_obj_set_size(obj, lh, w);
	lv_obj_set_clickable(obj, false);
	lv_obj_center(obj);
	return obj;
}

/* A w x h rounded box centered on (x, y) of the layout, in `parent` at screen y `origin` */
static lv_obj_t *box_create(lv_obj_t *parent, int orient, int origin, int x, int y, int w, int h,
                            int32_t radius)
{
	lv_area_t a = rect_to_screen(orient, x, y, w, h);
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_pos(obj, a.x1, a.y1 - origin);
	lv_obj_set_size(obj, lv_area_get_width(&a), lv_area_get_height(&a));
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	key_set_pressed(obj, false);
	return obj;
}

static lv_obj_t *label_create(lv_obj_t *parent, int orient, const char *text, const lv_font_t *font)
{
	if (orient != PORTRAIT)
		return text_turned_create(parent, orient, text, font);
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_center(label);
	return label;
}

/* The keys of a layout, into gen.key[0] (upright) or [1] (sideways) */
static void controls_create(lv_obj_t *parent, int orient, int origin)
{
	const struct layout *l = layout_of(orient);
	lv_obj_t **key = gen.key[orient != PORTRAIT];
	for (int i = PAD_UP; i <= PAD_RIGHT; i++) {
		key[i] = box_create(parent, orient, origin, l->dpad_x + keys[i].dx * l->dpad_off,
		                    l->dpad_y + keys[i].dy * l->dpad_off, l->dpad_key, l->dpad_key, 20);
		label_create(key[i], orient, keys[i].text, &ui_font_32);
	}
	for (int b = 0; b < 3; b++) {
		key[PAD_A + b] = box_create(parent, orient, origin, l->buttons[b].x, l->buttons[b].y,
		                            l->button_d, l->button_d, LV_RADIUS_CIRCLE);
		label_create(key[PAD_A + b], orient, keys[PAD_A + b].text,
		             orient == PORTRAIT ? &ui_font_48 : &ui_font_32);
	}
	key[PAD_START] = box_create(parent, orient, origin, l->start_x, l->start_y, l->start_w, l->start_h,
	                            LV_RADIUS_CIRCLE);
	label_create(key[PAD_START], orient, keys[PAD_START].text, orient == PORTRAIT ? &ui_font_24 : &ui_font_20);
	gen.shown_pad = ~0u; /* drawn as pressed or not on the next frame */
}

/* The view of the orientation of the frame shown, at the game's size */
static void view_place(void)
{
	int o = gen.shown_orient;
	const struct layout *l = layout_of(o);
	lv_obj_t *view = gen.view[o != PORTRAIT];
	int gw, gh;
	game_size(l, gen.game_w, gen.game_h, &gw, &gh);
	if (o == PORTRAIT) {
		lv_obj_set_pos(view, (SCREEN_W - gw) / 2, l->game_y - BODY_Y);
		lv_obj_set_size(view, gw, gh);
	} else {
		lv_area_t a = rect_to_screen(o, l->w / 2, l->h / 2, gw, gh);
		lv_obj_set_pos(view, a.x1, a.y1);
		lv_obj_set_size(view, lv_area_get_width(&a), lv_area_get_height(&a));
	}
	lv_obj_set_hidden(view, false);
}

static lv_obj_t *view_create(lv_obj_t *parent)
{
	lv_obj_t *view = lv_3dtexture_create(parent);
	lv_obj_set_clickable(view, false);
	lv_obj_set_hidden(view, true); /* until it has a frame */
	lv_3dtexture_set_rb_swap(view, true); /* RGB565, red in the high bits */
	if (gen.tex)
		lv_3dtexture_set_src(view, gen.tex);
	return view;
}

static void land_delete(void)
{
	if (!gen.land)
		return;
	lv_obj_delete(gen.land);
	gen.land = gen.view[1] = NULL;
	memset(gen.key[1], 0, sizeof(gen.key[1]));
	for (unsigned i = 0; i < gen.land_tex_count; i++)
		ui_texture_delete(gen.land_tex[i]);
	gen.land_tex_count = 0;
}

static void land_back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	land_delete();
	ui_pop();
}

static void restart_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	__atomic_store_n(&gen.reset, true, __ATOMIC_RELEASE);
}

/* A round button with a symbol, sideways at the top left */
static void land_button(int orient, int x, const char *symbol, lv_event_cb_t cb)
{
	lv_obj_t *b = box_create(gen.land, orient, 0, x, LAND_TOP_Y, LAND_ICON_D, LAND_ICON_D, LV_RADIUS_CIRCLE);
	lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_BUTTON_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_clickable(b, true);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	label_create(b, orient, symbol, &ui_font_26);
}

/* Sideways: all of the screen, over the status bar and the back gesture's strip
 * (which it takes the touches of) */
static void land_create(int orient)
{
	land_delete();
	gen.land = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(gen.land);
	lv_obj_set_size(gen.land, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(gen.land, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(gen.land, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(gen.land, false);
	lv_obj_set_clickable(gen.land, true);
	gen.view[1] = view_create(gen.land);
	controls_create(gen.land, orient, 0);
	land_button(orient, LAND_BACK_X, LV_SYMBOL_LEFT, land_back_cb);
	land_button(orient, LAND_RESTART_X, LV_SYMBOL_REFRESH, restart_cb);
}

/* Each frame the thread made, into the texture; the pressed keys; the layout of the
 * frame's orientation */
static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	unsigned pad = __atomic_load_n(&gen.pad, __ATOMIC_RELAXED);
	if (pad != gen.shown_pad) {
		lv_obj_t **key = gen.key[gen.shown_orient != PORTRAIT];
		for (int i = 0; i < PADS; i++)
			if (key[i])
				key_set_pressed(key[i], pad >> keys[i].id & 1);
		gen.shown_pad = pad;
	}

	/* main.c lowers the CPU's speed a second after the last touch: up again */
	if (lv_tick_elaps(gen.boosted) >= 500) {
		ui_cpu_boost(true);
		gen.boosted = lv_tick_get();
	}

	if (__atomic_load_n(&gen.seq, __ATOMIC_RELAXED) == gen.shown_seq)
		return;
	pthread_mutex_lock(&gen.lock);
	int w = gen.frame_w, h = gen.frame_h, o = gen.frame_orient;
	int tw = o == PORTRAIT ? w : h, th = o == PORTRAIT ? h : w;
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
	if (tw != gen.tex_w || th != gen.tex_h)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame);
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	gen.shown_seq = gen.seq;
	pthread_mutex_unlock(&gen.lock);

	if (created)
		lv_3dtexture_set_src(gen.view[0], gen.tex);
	bool turned = o != gen.shown_orient;
	if (turned) {
		if (o == PORTRAIT)
			land_delete();
		else
			land_create(o);
		gen.shown_orient = o;
	}
	if (turned || created || w != gen.game_w || h != gen.game_h) {
		gen.tex_w = tw;
		gen.tex_h = th;
		gen.game_w = w;
		gen.game_h = h;
		view_place();
	}
	lv_obj_invalidate(gen.view[o != PORTRAIT]);
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
	if (gen.pcm >= 0)
		close(gen.pcm);
	if (gen.touch >= 0)
		close(gen.touch);
	gen.pcm = gen.touch = -1;
	accel_close();
	__atomic_store_n(&gen.pad, 0, __ATOMIC_RELAXED);
}

static void game_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	game_stop();
	land_delete();
	if (gen.timer)
		lv_timer_delete(gen.timer);
	gen.timer = NULL;
	if (gen.tex)
		glDeleteTextures(1, &gen.tex);
	gen.tex = 0;
	gen.scr = gen.body = gen.view[0] = NULL;
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

	pcm_open();
	touch_open();
	/* Held as it is now from the first frame */
	accel_open();
	gen.orient = accel_orient(PORTRAIT);
	gen.orient_reads = 0;
	gen.accel_read = now_ms();
	gen.quit = gen.reset = false;
	if (pthread_create(&gen.thread, NULL, game_main, NULL) != 0) {
		core.unload_game();
		accel_close();
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

	lv_obj_t *header;
	gen.scr = ui_app_screen_create(gen.title, &header, &gen.body);
	ui_header_button(header, LV_SYMBOL_REFRESH, restart_cb, NULL);
	lv_obj_add_event_cb(gen.scr, game_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_layout(gen.body, LV_LAYOUT_NONE);
	lv_obj_set_scrollable(gen.body, false);

	gen.view[0] = view_create(gen.body);
	controls_create(gen.body, PORTRAIT, BODY_Y);
	gen.tex_w = gen.tex_h = gen.game_w = gen.game_h = 0;
	gen.shown_orient = PORTRAIT;
	gen.shown_seq = gen.seq;

	if (!game_start()) {
		lv_obj_t *label = ui_empty_label(gen.body, "Cannot start this game");
		lv_obj_set_pos(label, 0, 0);
	} else {
		gen.timer = lv_timer_create(frame_cb, 4, NULL);
		ui_cpu_boost(true);
		gen.boosted = lv_tick_get();
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

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	for (unsigned i = 0; i < gen.count; i++)
		free(titles[i]);
	free(titles);
	free(rows);
	titles = NULL;
	rows = NULL;
	roms_free();
}

/* "Saved" after the name, once there is a save state */
static void row_update(unsigned i)
{
	char state[PATH_MAX];
	snprintf(state, sizeof(state), SAVES "/%s.state", titles[i]);
	if (lv_obj_get_child_count(rows[i]) == 1 && access(state, F_OK) == 0)
		ui_dim_label(rows[i], "Saved", &ui_font_20);
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

static void list_open(void)
{
	mkdir(HOME, 0755);
	mkdir(ROMS, 0755);
	roms_scan();

	lv_obj_t *scr, *body;
	scr = ui_app_screen_create("Genesis", NULL, &body);
	lv_obj_add_event_cb(scr, list_delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *list = ui_box(body);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_top(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

	if (!gen.count) {
		ui_empty_label(list, "No games");
		lv_obj_t *hint = ui_dim_label(list, "Copy ROMs to " ROMS, &ui_font_20);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
		ui_push(scr);
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
		lv_obj_t *name = ui_one_line_label(rows[i], titles[i], &ui_font_26);
		lv_obj_set_flex_grow(name, 1);
		row_update(i);
	}
	ui_push(scr);
}

/* What phone-ui looks for (see external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	list_open();
}
