/*
 * Doom on the PinePhone: doomgeneric's platform layer on KMS and the touchscreen,
 * as an external app of phone-ui (see external.c there).
 *
 * The phone is held sideways, its top to the left (Power and the volume keys on
 * top). In that 1440 x 720 landscape screen the game, 320 x 200 scaled 3 times,
 * is in the middle, with the controls around it:
 *
 *   +--------+-------- Y ----------- N --------+------+
 *   | ENTER  |                                 | MENU |
 *   +--------+                                 +------+
 *   |        |                                 | MAP  |
 *   |  MOVE  |             game                +------+
 *   | stick  |                                 | USE  |
 *   |        |                                 +------+
 *   |        |                                 | FIRE |
 *   +--------+--- 1   2   3   4   5   6   7 ---+------+
 *
 * The stick is wherever the thumb lands in its area: moving it from there walks
 * and turns, and far (RUN_PX) runs. Each finger keeps what it first touched: the
 * stick, or the buttons (it can slide from one to another).
 *
 * Frames are scaled and rotated into one of two dumb buffers and page flipped.
 * There is no sound. Settings and saves go in $APP_DATA.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/input.h>

#include "doomgeneric.h"

/* doomkeys.h's names collide with linux/input.h's */
#define DOOM_KEY_ENTER 13
#define DOOM_KEY_ESCAPE 27
#define DOOM_KEY_TAB 9
#define DOOM_KEY_UPARROW 0xad
#define DOOM_KEY_DOWNARROW 0xaf
#define DOOM_KEY_LEFTARROW 0xac
#define DOOM_KEY_RIGHTARROW 0xae
#define DOOM_KEY_USE 0xa2
#define DOOM_KEY_FIRE 0xa3
#define DOOM_KEY_RSHIFT (0x80 + 0x36)

/* Landscape screen */
#define W 1440
#define H 720
#define SCALE 3
#define GAME_X 240 /* left of the game */
#define GAME_Y 60  /* top of the game */
#define GAME_W (DOOMGENERIC_RESX * SCALE)
#define GAME_H (DOOMGENERIC_RESY * SCALE)

#define STICK_DEAD 36 /* px from where the thumb landed before it moves */
#define RUN_PX 110    /* px: running */

#define COLOR_BG 0x101010
#define COLOR_BUTTON 0x2a2a2a
#define COLOR_PRESSED 0x6a1a12
#define COLOR_TEXT 0xd0d0d0

struct button {
	int x, y, w, h;
	const char *label;
	int key;
};

static const struct button buttons[] = {
	{ 0, 0, GAME_X, 120, "ENTER", DOOM_KEY_ENTER },
	{ GAME_X + GAME_W, 0, W - GAME_X - GAME_W, 120, "MENU", DOOM_KEY_ESCAPE },
	{ GAME_X + GAME_W, 120, W - GAME_X - GAME_W, 120, "MAP", DOOM_KEY_TAB },
	{ GAME_X + GAME_W, 240, W - GAME_X - GAME_W, 180, "USE", DOOM_KEY_USE },
	{ GAME_X + GAME_W, 420, W - GAME_X - GAME_W, H - 420, "FIRE", DOOM_KEY_FIRE },
	{ GAME_X, 0, GAME_W / 2, GAME_Y, "Y", 'y' },
	{ GAME_X + GAME_W / 2, 0, GAME_W / 2, GAME_Y, "N", 'n' },
#define WEAPON(i) { GAME_X + GAME_W * (i - 1) / 7, GAME_Y + GAME_H, GAME_W / 7, H - GAME_Y - GAME_H, #i, '0' + i }
	WEAPON(1), WEAPON(2), WEAPON(3), WEAPON(4), WEAPON(5), WEAPON(6), WEAPON(7),
};
#define NBUTTONS (sizeof(buttons) / sizeof(buttons[0]))

/* The stick's area: the rest of the left side */
#define STICK_X 0
#define STICK_Y 120
#define STICK_W GAME_X
#define STICK_H (H - 120)

/* 5 x 7 font for the labels: rows from the top, bit 4 on the left */
static const struct {
	char c;
	unsigned char rows[7];
} glyphs[] = {
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
	{ 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
	{ 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
	{ 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
	{ 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
	{ 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
};

/* Display: two dumb buffers in portrait (the panel's own orientation) */
static int drm_fd;
static uint32_t crtc_id, conn_id;
static struct drm_mode_modeinfo mode;
static struct {
	uint32_t fb_id;
	uint32_t *pixels;
	unsigned dirty; /* the controls need drawing */
} bufs[2];
static uint32_t stride; /* in pixels */
static unsigned back;
static bool flip_pending, crtc_set;

/* Input */
static int touch_fd = -1;
static int touch_max_x = 719, touch_max_y = 1439;
#define SLOTS 10
enum owner { NONE, STICK, BUTTONS };
static struct {
	int id;           /* tracking ID, -1: no finger */
	int x, y;         /* landscape */
	bool new;         /* landed since the last report */
	enum owner owner;
	int ox, oy;       /* where it landed (the stick's center) */
} slots[SLOTS];
static int cur_slot;
static bool key_down[256];
static bool pressed[NBUTTONS];
static unsigned short queue[64];
static unsigned queue_r, queue_w;

static void die(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void drm_ioctl(unsigned long request, void *arg, const char *what)
{
	while (ioctl(drm_fd, request, arg) < 0)
		if (errno != EINTR && errno != EAGAIN)
			die(what);
}

/* A pixel of the landscape screen in a buffer: the panel's top is on the left */
static inline uint32_t *at(uint32_t *pixels, int x, int y)
{
	return &pixels[x * stride + (H - 1 - y)];
}

static void fill(uint32_t *pixels, int x, int y, int w, int h, uint32_t color)
{
	for (int i = x; i < x + w; i++) {
		uint32_t *p = at(pixels, i, y + h - 1);
		for (int j = 0; j < h; j++)
			p[j] = color;
	}
}

static void text(uint32_t *pixels, int cx, int cy, const char *s, int scale)
{
	int len = strlen(s);
	int x = cx - (len * 6 - 1) * scale / 2, y = cy - 7 * scale / 2;
	for (; *s; s++, x += 6 * scale) {
		for (unsigned g = 0; g < sizeof(glyphs) / sizeof(glyphs[0]); g++) {
			if (glyphs[g].c != *s)
				continue;
			for (int r = 0; r < 7; r++)
				for (int c = 0; c < 5; c++)
					if (glyphs[g].rows[r] >> (4 - c) & 1)
						fill(pixels, x + c * scale, y + r * scale, scale, scale, COLOR_TEXT);
		}
	}
}

static void controls_draw(uint32_t *pixels)
{
	fill(pixels, STICK_X, STICK_Y, STICK_W, STICK_H, COLOR_BG);
	text(pixels, STICK_X + STICK_W / 2, STICK_Y + STICK_H / 2, "MOVE", 4);
	for (unsigned i = 0; i < NBUTTONS; i++) {
		const struct button *b = &buttons[i];
		fill(pixels, b->x, b->y, b->w, b->h, COLOR_BG);
		fill(pixels, b->x + 4, b->y + 4, b->w - 8, b->h - 8, pressed[i] ? COLOR_PRESSED : COLOR_BUTTON);
		text(pixels, b->x + b->w / 2, b->y + b->h / 2, b->label, b->h >= 100 ? 4 : 3);
	}
}

static void display_open(void)
{
	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0)
		die("/dev/dri/card0");

	struct drm_mode_card_res res = { 0 };
	drm_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "resources");
	uint32_t *conns = calloc(res.count_connectors, sizeof(uint32_t));
	uint32_t *crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
	uint32_t *encs = calloc(res.count_encoders, sizeof(uint32_t));
	uint32_t *fbs = calloc(res.count_fbs + 1, sizeof(uint32_t));
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.fb_id_ptr = (uintptr_t)fbs;
	drm_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "resources");

	struct drm_mode_get_connector conn;
	for (uint32_t i = 0; i < res.count_connectors && !mode.hdisplay; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conns[i];
		drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "connector");
		if (conn.connection != 1 || !conn.count_modes) /* 1: connected */
			continue;
		struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(*modes));
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_props = conn.count_encoders = 0;
		drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "connector");
		mode = modes[0];
		conn_id = conn.connector_id;
		free(modes);
	}
	if (mode.hdisplay != H || mode.vdisplay != W) {
		fprintf(stderr, "doom: no %dx%d display\n", H, W);
		exit(EXIT_FAILURE);
	}
	struct drm_mode_get_encoder enc = { .encoder_id = conn.encoder_id };
	crtc_id = crtcs[0];
	if (enc.encoder_id && ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
		crtc_id = enc.crtc_id;
	free(conns);
	free(crtcs);
	free(encs);
	free(fbs);

	for (int i = 0; i < 2; i++) {
		struct drm_mode_create_dumb dumb = { .width = H, .height = W, .bpp = 32 };
		drm_ioctl(DRM_IOCTL_MODE_CREATE_DUMB, &dumb, "dumb buffer");
		struct drm_mode_fb_cmd fb = { .width = H, .height = W, .pitch = dumb.pitch,
		                              .bpp = 32, .depth = 24, .handle = dumb.handle };
		drm_ioctl(DRM_IOCTL_MODE_ADDFB, &fb, "framebuffer");
		struct drm_mode_map_dumb map = { .handle = dumb.handle };
		drm_ioctl(DRM_IOCTL_MODE_MAP_DUMB, &map, "map");
		bufs[i].pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
		if (bufs[i].pixels == MAP_FAILED)
			die("mmap");
		bufs[i].fb_id = fb.fb_id;
		bufs[i].dirty = true;
		stride = dumb.pitch / 4;
		memset(bufs[i].pixels, 0, dumb.size);
	}
}

/* Until the last flip is done: its buffer is shown, the other one is free */
static void flip_wait(void)
{
	char buf[256];
	while (flip_pending) {
		ssize_t n = read(drm_fd, buf, sizeof(buf));
		if (n < 0 && errno != EINTR)
			die("DRM events");
		for (ssize_t i = 0; i + (ssize_t)sizeof(struct drm_event) <= n;) {
			struct drm_event *e = (struct drm_event *)(buf + i);
			if (e->type == DRM_EVENT_FLIP_COMPLETE)
				flip_pending = false;
			i += e->length;
		}
	}
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
		fprintf(stderr, "doom: no touchscreen\n");
	for (int i = 0; i < SLOTS; i++)
		slots[i].id = -1;
}

static int button_at(int x, int y)
{
	for (unsigned i = 0; i < NBUTTONS; i++)
		if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w &&
		    y >= buttons[i].y && y < buttons[i].y + buttons[i].h)
			return i;
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
	bool want[256] = { false };
	bool now_pressed[NBUTTONS] = { false };

	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].id < 0)
			continue;
		int x = slots[i].x, y = slots[i].y;
		if (slots[i].new) {
			slots[i].new = false;
			bool stick = x >= STICK_X && x < STICK_X + STICK_W && y >= STICK_Y && y < STICK_Y + STICK_H;
			slots[i].owner = stick ? STICK : BUTTONS;
			slots[i].ox = x;
			slots[i].oy = y;
		}
		if (slots[i].owner == STICK) {
			int dx = x - slots[i].ox, dy = y - slots[i].oy;
			want[DOOM_KEY_UPARROW] |= dy < -STICK_DEAD;
			want[DOOM_KEY_DOWNARROW] |= dy > STICK_DEAD;
			want[DOOM_KEY_LEFTARROW] |= dx < -STICK_DEAD;
			want[DOOM_KEY_RIGHTARROW] |= dx > STICK_DEAD;
			want[DOOM_KEY_RSHIFT] |= dx * dx + dy * dy > RUN_PX * RUN_PX;
		} else {
			int b = button_at(x, y);
			if (b >= 0) {
				want[buttons[b].key] = true;
				now_pressed[b] = true;
			}
		}
	}
	for (int k = 0; k < 256; k++)
		key_set(k, want[k]);
	if (memcmp(pressed, now_pressed, sizeof(pressed)) != 0) {
		memcpy(pressed, now_pressed, sizeof(pressed));
		bufs[0].dirty = bufs[1].dirty = true;
	}
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
			/* Panel x (left to right, upright) is landscape y from the bottom */
			slots[cur_slot].y = H - 1 - ev.value * H / (touch_max_x + 1);
		} else if (ev.code == ABS_MT_POSITION_Y) {
			slots[cur_slot].x = ev.value * W / (touch_max_y + 1);
		}
	}
}

void DG_Init(void)
{
	display_open();
	touch_open();
}

void DG_DrawFrame(void)
{
	touch_read();
	flip_wait();
	uint32_t *pixels = bufs[back].pixels;
	if (bufs[back].dirty) {
		controls_draw(pixels);
		bufs[back].dirty = false;
	}

	/* A column of the game is a row of the panel: each source column makes SCALE
	 * of them, every pixel SCALE long */
	for (int sx = 0; sx < DOOMGENERIC_RESX; sx++) {
		uint32_t *row = at(pixels, GAME_X + sx * SCALE, GAME_Y + GAME_H - 1);
		const uint32_t *src = DG_ScreenBuffer + (DOOMGENERIC_RESY - 1) * DOOMGENERIC_RESX + sx;
		uint32_t *p = row;
		for (int sy = 0; sy < DOOMGENERIC_RESY; sy++, src -= DOOMGENERIC_RESX)
			for (int k = 0; k < SCALE; k++)
				*p++ = *src;
		for (int k = 1; k < SCALE; k++)
			memcpy(row + k * stride, row, GAME_H * sizeof(*row));
	}

	if (!crtc_set) {
		struct drm_mode_crtc crtc = { .crtc_id = crtc_id, .fb_id = bufs[back].fb_id,
		                              .set_connectors_ptr = (uintptr_t)&conn_id,
		                              .count_connectors = 1, .mode = mode, .mode_valid = 1 };
		drm_ioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "set CRTC");
		crtc_set = true;
	} else {
		struct drm_mode_crtc_page_flip flip = { .crtc_id = crtc_id, .fb_id = bufs[back].fb_id,
		                                        .flags = DRM_MODE_PAGE_FLIP_EVENT };
		drm_ioctl(DRM_IOCTL_MODE_PAGE_FLIP, &flip, "page flip");
		flip_pending = true;
	}
	back ^= 1;
}

void DG_SleepMs(uint32_t ms)
{
	usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* In $APP_DATA (settings and saves go in the current folder), with the IWAD from
 * the app's folder (the current one at start) unless one is given */
int main(int argc, char **argv)
{
	/* stdout is phone-ui's log: every line as it comes */
	setvbuf(stdout, NULL, _IOLBF, 0);
	char iwad[PATH_MAX];
	if (!getcwd(iwad, sizeof(iwad) - 16))
		die("getcwd");
	strcat(iwad, "/doom1.wad");
	const char *data = getenv("APP_DATA");
	if (data && chdir(data) < 0)
		perror(data);

	char **args = calloc(argc + 3, sizeof(*args));
	int n = 0;
	bool has_iwad = false;
	for (int i = 0; i < argc; i++) {
		has_iwad |= strcmp(argv[i], "-iwad") == 0;
		args[n++] = argv[i];
	}
	if (!has_iwad) {
		args[n++] = "-iwad";
		args[n++] = iwad;
	}
	doomgeneric_Create(n, args);
	for (;;)
		doomgeneric_Tick();
}
