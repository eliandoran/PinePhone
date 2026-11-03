/*
 * Doom on the PinePhone, the engine side: doomgeneric's platform layer, built
 * with doomgeneric into lib/doomengine.so, which the plugin (plugin.c) loads and
 * runs in a thread with doom_run(). See doom.h for what they share.
 *
 * - Frames: DG_DrawFrame() copies Doom's into host->frame, opaque (Doom leaves
 *   the alpha byte 0), for the plugin to upload as a texture.
 * - Touch: the touchscreen is read here (LVGL only sees one finger), as the usual
 *   touch controls of Doom ports (Delta Touch, the official one), in the layout
 *   the plugin shows: left of its middle a stick wherever the thumb lands (moving
 *   from there walks and strafes, far runs), right of it dragging sideways turns
 *   (as a mouse does); FIRE turns too, so the same thumb fires and aims. Each
 *   finger keeps what it first touched: the stick, turning, or the buttons (it
 *   can slide from one to another).
 * - Pausing (host->paused, the app not shown): Doom's clock stops (no catching up
 *   afterwards), the keys held are let go, the sound device is let go (sound.c).
 * - Quitting: Doom calls exit() (its menu, I_Error()), built as doom_exit(), which
 *   returns from doom_run(); the plugin then unloads the library, whose state
 *   cannot be used again (doomgeneric has no way to start over).
 * - Settings and saves go in host->configdir (m_config.c, changed by the Makefile).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#include "doom.h"
#include "doomgeneric.h"

#define SCREEN_W 720  /* the panel, upright */
#define SCREEN_H 1440
#define SLOTS 10

#define STICK_DEAD 36 /* px from where the thumb landed before it moves */
#define RUN_PX 110    /* px: running */
#define TURN_SCALE 6  /* mouse units per px dragged: 8192 turn around */
#define PAUSE_POLL_MS 50

/* g_game.c: mouse turning for the next tic, cleared once used */
extern int mousex;
extern int key_prevweapon, key_nextweapon;

/* sound.c */
void doom_sound_pause(bool paused);
void doom_sound_stop(void);

/* m_config.c (the Makefile makes its default configuration directory this) */
char *doom_configdir;

static struct doom_host *host;
static jmp_buf quit_jmp;
static uint32_t paused_ms; /* left out of Doom's clock */

/* Input */
static int touch_fd = -1;
static int touch_max_x = SCREEN_W - 1, touch_max_y = SCREEN_H - 1;
enum owner { NONE, STICK, TURN, BUTTONS };
static struct {
	int id;           /* tracking ID, -1: no finger */
	int sx, sy;       /* on the panel, upright */
	bool new;         /* landed since the last report */
	enum owner owner;
	bool fire;        /* turning from FIRE: fires too */
	int ox, oy;       /* where it landed in the layout (the stick's center); turning: last x */
} slots[SLOTS];
static int cur_slot;
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

static void touch_open(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && touch_fd < 0 && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		struct input_absinfo x, y;
		if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) == 0 &&
		    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) == 0 && x.maximum > 0) {
			touch_fd = fd;
			touch_max_x = x.maximum;
			touch_max_y = y.maximum;
		} else if (fd >= 0) {
			close(fd);
		}
	}
	if (d)
		closedir(d);
	if (touch_fd < 0)
		printf("doom: no touchscreen\n");
	for (int i = 0; i < SLOTS; i++)
		slots[i].id = -1;
}

/* From the panel (upright) into the layout's coordinates, as LVGL turns its own
 * touches back (see phone-ui's turn.c) */
static void from_screen(int orient, int sx, int sy, int *x, int *y)
{
	switch (orient) {
	case DOOM_LEFT: *x = sy; *y = SCREEN_W - 1 - sx; break;
	case DOOM_RIGHT: *x = SCREEN_H - 1 - sy; *y = sx; break;
	default: *x = sx; *y = sy; break;
	}
}

static int button_at(const struct doom_layout *l, int x, int y)
{
	for (unsigned i = 0; i < l->count; i++) {
		const struct doom_button *b = &l->buttons[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
			return i;
	}
	return -1;
}

static void key_set(int key, bool down)
{
	if (key_down[key] == down)
		return;
	key_down[key] = down;
	if (queue_w - queue_r < sizeof(queue) / sizeof(queue[0]))
		queue[queue_w++ % (sizeof(queue) / sizeof(queue[0]))] = down << 8 | key;
}

/* After a report: the keys the fingers hold now */
static void touch_update(void)
{
	int orient = __atomic_load_n(&host->orient, __ATOMIC_RELAXED);
	const struct doom_layout *l = doom_layout_of(orient);
	unsigned fire = l->count - 1;
	bool want[256] = { false };
	unsigned pressed = 0;

	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].id < 0)
			continue;
		int x, y;
		from_screen(orient, slots[i].sx, slots[i].sy, &x, &y);
		if (slots[i].new) {
			slots[i].new = false;
			int b = button_at(l, x, y);
			slots[i].fire = b == (int)fire;
			slots[i].owner = b == (int)fire ? TURN : b >= 0 ? BUTTONS : x < l->middle ? STICK : TURN;
			slots[i].ox = x;
			slots[i].oy = y;
		}
		if (slots[i].owner == STICK) {
			int dx = x - slots[i].ox, dy = y - slots[i].oy;
			want[DOOM_KEY_UPARROW] |= dy < -STICK_DEAD;
			want[DOOM_KEY_DOWNARROW] |= dy > STICK_DEAD;
			want[DOOM_KEY_STRAFE_L] |= dx < -STICK_DEAD;
			want[DOOM_KEY_STRAFE_R] |= dx > STICK_DEAD;
			want[DOOM_KEY_RSHIFT] |= dx * dx + dy * dy > RUN_PX * RUN_PX;
		} else if (slots[i].owner == TURN) {
			/* Added up until the next tic uses it */
			mousex += (x - slots[i].ox) * TURN_SCALE;
			slots[i].ox = x;
			if (slots[i].fire) {
				want[DOOM_KEY_FIRE] = true;
				pressed |= 1u << fire;
			}
		} else {
			int b = button_at(l, x, y);
			if (b >= 0) {
				/* The back button is the plugin's (LVGL takes its taps) */
				if (l->buttons[b].key != DOOM_KEY_BACK)
					want[l->buttons[b].key] = true;
				pressed |= 1u << b;
			}
		}
	}
	for (int k = 1; k < 256; k++)
		key_set(k, want[k]);
	__atomic_store_n(&host->pressed, pressed, __ATOMIC_RELAXED);
}

static void touch_read(void)
{
	struct input_event ev;
	while (touch_fd >= 0 && read(touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			touch_update();
			continue;
		}
		if (ev.type != EV_ABS)
			continue;
		if (ev.code == ABS_MT_SLOT) {
			cur_slot = ev.value >= 0 && ev.value < SLOTS ? ev.value : 0;
		} else if (ev.code == ABS_MT_TRACKING_ID) {
			slots[cur_slot].new = ev.value >= 0 && slots[cur_slot].id < 0;
			slots[cur_slot].id = ev.value;
		} else if (ev.code == ABS_MT_POSITION_X) {
			slots[cur_slot].sx = ev.value * SCREEN_W / (touch_max_x + 1);
		} else if (ev.code == ABS_MT_POSITION_Y) {
			slots[cur_slot].sy = ev.value * SCREEN_H / (touch_max_y + 1);
		}
	}
}

/* While the app is not shown: nothing held, the clock stopped, the sound let go.
 * Touches made meanwhile (in other screens) are dropped. */
static void pause_wait(void)
{
	if (!__atomic_load_n(&host->paused, __ATOMIC_ACQUIRE))
		return;
	for (int k = 1; k < 256; k++)
		key_set(k, false);
	__atomic_store_n(&host->pressed, 0, __ATOMIC_RELAXED);
	doom_sound_pause(true);
	uint32_t start = now_ms();
	while (__atomic_load_n(&host->paused, __ATOMIC_ACQUIRE))
		usleep(PAUSE_POLL_MS * 1000);
	paused_ms += now_ms() - start;
	struct input_event ev;
	while (touch_fd >= 0 && read(touch_fd, &ev, sizeof(ev)) == sizeof(ev))
		;
	for (int i = 0; i < SLOTS; i++)
		slots[i].id = -1;
	doom_sound_pause(false);
}

void DG_Init(void)
{
	touch_open();
}

void DG_DrawFrame(void)
{
	touch_read();
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
		touch_read();
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
			doomgeneric_Tick();
		}
	}
	doom_sound_stop();
	if (touch_fd >= 0)
		close(touch_fd);
	touch_fd = -1;
	__atomic_store_n(&host->exited, true, __ATOMIC_RELEASE);
}
