/*
 * Genesis: Sega Genesis / Mega Drive (and Master System, Game Gear, SG-1000) games,
 * a plugin app (see external.c in phone-ui) around Genesis Plus GX's libretro core.
 *
 * Games are ROM files in /var/lib/genesis/roms, listed by name. The phone is held
 * upright, as a handheld: the game at the top, twice its size (640 px wide, 448 or
 * 480 high; double-resolution interlaced modes at their size), and the controls
 * below it:
 *
 *        ^             C
 *     <     >       B
 *        v       A
 *          START
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
 * restarts the game. BIOS files (optional, e.g. bios_MD.bin) go in /var/lib/genesis.
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
#define GAME_W 640
#define GAME_Y (BODY_Y + 16)

/* Controls, in screen coordinates */
#define DPAD_X 190
#define DPAD_Y 1040
#define DPAD_REACH 230 /* a finger landing this close is on the D-pad */
#define DPAD_DEAD 28   /* no direction this close to the center */
#define DPAD_KEY 104   /* each arrow's square */
#define DPAD_OFF 108   /* from the center to each arrow's */
#define BUTTON_D 112
#define BUTTON_HIT 92 /* a finger presses the buttons whose center is this close */
#define START_W 168
#define START_H 64
#define START_X 360
#define START_Y 1330
#define SLOTS 10

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

struct pad_key {
	int x, y;    /* center, screen coordinates */
	unsigned id; /* RETRO_DEVICE_ID_JOYPAD_* */
	const char *text;
	lv_obj_t *obj;
};

static struct pad_key arrows[] = {
	{ DPAD_X, DPAD_Y - DPAD_OFF, RETRO_DEVICE_ID_JOYPAD_UP, LV_SYMBOL_UP, NULL },
	{ DPAD_X, DPAD_Y + DPAD_OFF, RETRO_DEVICE_ID_JOYPAD_DOWN, LV_SYMBOL_DOWN, NULL },
	{ DPAD_X - DPAD_OFF, DPAD_Y, RETRO_DEVICE_ID_JOYPAD_LEFT, LV_SYMBOL_LEFT, NULL },
	{ DPAD_X + DPAD_OFF, DPAD_Y, RETRO_DEVICE_ID_JOYPAD_RIGHT, LV_SYMBOL_RIGHT, NULL },
};

/* The core's 3-button pad: A is libretro's Y, B its B, C its A */
static struct pad_key buttons[] = {
	{ 410, 1110, RETRO_DEVICE_ID_JOYPAD_Y, "A", NULL },
	{ 530, 1040, RETRO_DEVICE_ID_JOYPAD_B, "B", NULL },
	{ 650, 970, RETRO_DEVICE_ID_JOYPAD_A, "C", NULL },
};

static struct pad_key start = { START_X, START_Y, RETRO_DEVICE_ID_JOYPAD_START, "START", NULL };

static struct {
	/* The list */
	char **roms; /* file names, sorted */
	unsigned count;

	/* The game's screen (LVGL's thread) */
	lv_obj_t *scr, *view;
	lv_timer_t *timer;
	GLuint tex;
	int tex_w, tex_h;
	unsigned shown_seq, shown_pad;
	uint32_t boosted;

	/* The game */
	char rom[PATH_MAX], state[PATH_MAX], srm[PATH_MAX];
	char title[256];
	pthread_t thread;
	bool running;
	double fps;

	/* Shared with the thread */
	pthread_mutex_t lock;
	int frame_w, frame_h;
	unsigned seq;
	unsigned pad; /* buttons held, a bit per RETRO_DEVICE_ID_JOYPAD_* (atomic) */
	bool quit, reset; /* atomic */

	/* The thread's */
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
} gen = { .lock = PTHREAD_MUTEX_INITIALIZER, .pcm = -1, .touch = -1 };

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

static unsigned keys_near(const struct pad_key *keys, unsigned n, int x, int y)
{
	unsigned pad = 0;
	for (unsigned i = 0; i < n; i++) {
		int dx = x - keys[i].x, dy = y - keys[i].y;
		if (dx * dx + dy * dy <= BUTTON_HIT * BUTTON_HIT)
			pad |= 1u << keys[i].id;
	}
	return pad;
}

/* After a report: the buttons the fingers hold now */
static void touch_update(void)
{
	unsigned pad = 0;
	for (int i = 0; i < SLOTS; i++) {
		if (gen.slots[i].id < 0)
			continue;
		int x = gen.slots[i].x, y = gen.slots[i].y;
		int dx = x - DPAD_X, dy = y - DPAD_Y;
		if (gen.slots[i].new) {
			gen.slots[i].new = false;
			gen.slots[i].dpad = dx * dx + dy * dy <= DPAD_REACH * DPAD_REACH;
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
		} else {
			pad |= keys_near(buttons, sizeof(buttons) / sizeof(buttons[0]), x, y);
			if (abs(x - START_X) <= START_W / 2 + 20 && abs(y - START_Y) <= START_H / 2 + 20)
				pad |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
		}
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

static void *game_main(void *arg)
{
	LV_UNUSED(arg);
	clock_gettime(CLOCK_MONOTONIC, &gen.next);
	while (!__atomic_load_n(&gen.quit, __ATOMIC_ACQUIRE)) {
		if (__atomic_exchange_n(&gen.reset, false, __ATOMIC_ACQ_REL))
			core.reset();
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

/* Each frame the thread made, into the texture; the pressed keys */
static void frame_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	unsigned pad = __atomic_load_n(&gen.pad, __ATOMIC_RELAXED);
	if (pad != gen.shown_pad) {
		for (unsigned i = 0; i < sizeof(arrows) / sizeof(arrows[0]); i++)
			key_set_pressed(arrows[i].obj, pad >> arrows[i].id & 1);
		for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
			key_set_pressed(buttons[i].obj, pad >> buttons[i].id & 1);
		key_set_pressed(start.obj, pad >> start.id & 1);
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
	int w = gen.frame_w, h = gen.frame_h;
	if (!gen.tex) {
		glGenTextures(1, &gen.tex);
		glBindTexture(GL_TEXTURE_2D, gen.tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		lv_3dtexture_set_src(gen.view, gen.tex);
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

	if (w != gen.tex_w || h != gen.tex_h) {
		gen.tex_w = w;
		gen.tex_h = h;
		/* Same width whatever the mode (as on a TV), lines doubled unless interlaced */
		lv_obj_set_size(gen.view, GAME_W, h <= 288 ? h * 2 : h);
		lv_obj_set_hidden(gen.view, false);
	}
	lv_obj_invalidate(gen.view);
}

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
	__atomic_store_n(&gen.pad, 0, __ATOMIC_RELAXED);
}

static void game_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	game_stop();
	if (gen.timer)
		lv_timer_delete(gen.timer);
	gen.timer = NULL;
	if (gen.tex)
		glDeleteTextures(1, &gen.tex);
	gen.tex = 0;
	gen.scr = gen.view = NULL;
	ui_cpu_boost(false);
}

static void restart_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	__atomic_store_n(&gen.reset, true, __ATOMIC_RELEASE);
}

static lv_obj_t *key_create(lv_obj_t *parent, struct pad_key *k, int32_t w, int32_t h, int32_t radius,
                            const lv_font_t *font)
{
	lv_obj_t *obj = ui_box(parent);
	lv_obj_set_pos(obj, k->x - w / 2, k->y - h / 2 - BODY_Y);
	lv_obj_set_size(obj, w, h);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	key_set_pressed(obj, false);
	lv_obj_t *label = lv_label_create(obj);
	lv_label_set_text_static(label, k->text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_center(label);
	k->obj = obj;
	return obj;
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

	lv_obj_t *header, *body;
	gen.scr = ui_app_screen_create(gen.title, &header, &body);
	ui_header_button(header, LV_SYMBOL_REFRESH, restart_cb, NULL);
	lv_obj_add_event_cb(gen.scr, game_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_layout(body, LV_LAYOUT_NONE);
	lv_obj_set_scrollable(body, false);

	gen.view = lv_3dtexture_create(body);
	lv_obj_set_pos(gen.view, (SCREEN_W - GAME_W) / 2, GAME_Y - BODY_Y);
	lv_obj_set_clickable(gen.view, false);
	lv_obj_set_hidden(gen.view, true); /* until the first frame */
	lv_3dtexture_set_rb_swap(gen.view, true); /* RGB565, red in the high bits */
	gen.tex_w = gen.tex_h = 0;
	gen.shown_seq = gen.seq;

	for (unsigned i = 0; i < sizeof(arrows) / sizeof(arrows[0]); i++)
		key_create(body, &arrows[i], DPAD_KEY, DPAD_KEY, 20, &ui_font_32);
	for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
		key_create(body, &buttons[i], BUTTON_D, BUTTON_D, LV_RADIUS_CIRCLE, &ui_font_48);
	key_create(body, &start, START_W, START_H, LV_RADIUS_CIRCLE, &ui_font_24);
	gen.shown_pad = 0;

	if (!game_start()) {
		lv_obj_t *label = ui_empty_label(body, "Cannot start this game");
		lv_obj_set_pos(label, 0, 0);
	} else {
		gen.timer = lv_timer_create(frame_cb, 4, NULL);
		ui_cpu_boost(true);
		gen.boosted = lv_tick_get();
	}
	ui_push(gen.scr);
}

/* The list */

static bool rom_wanted(const char *name)
{
	static const char *const exts[] = { "md", "gen", "smd", "bin", "sms", "gg", "sg", "68k" };
	const char *dot = strrchr(name, '.');
	if (!dot || name[0] == '.')
		return false;
	for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (strcasecmp(dot + 1, exts[i]) == 0)
			return true;
	return false;
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
	if (gen.count)
		qsort(gen.roms, gen.count, sizeof(*gen.roms), rom_compare);
}

/* The names shown: the file names without their extension */
static char **titles;

static void list_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	for (unsigned i = 0; i < gen.count; i++)
		free(titles[i]);
	free(titles);
	titles = NULL;
	roms_free();
}

static void rom_cb(lv_event_t *e)
{
	game_open(gen.roms[(uintptr_t)lv_event_get_user_data(e)]);
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
	lv_obj_t *card = ui_card_create(list);
	for (unsigned i = 0; i < gen.count; i++) {
		titles[i] = strdup(gen.roms[i]);
		char *dot = strrchr(titles[i], '.');
		if (dot && dot != titles[i])
			*dot = '\0';
		lv_obj_t *row = ui_row_create(card, 96, i + 1 < gen.count, rom_cb, (void *)(uintptr_t)i);
		lv_obj_t *name = ui_one_line_label(row, titles[i], &ui_font_26);
		lv_obj_set_flex_grow(name, 1);
		char state[PATH_MAX];
		snprintf(state, sizeof(state), SAVES "/%s.state", titles[i]);
		if (access(state, F_OK) == 0)
			ui_dim_label(row, "Saved", &ui_font_20);
	}
	ui_push(scr);
}

/* What phone-ui looks for (see external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	list_open();
}
