/*
 * Game controllers (ui.h): the first input device with a gamepad's buttons (BTN_GAMEPAD),
 * opened beside LVGL's devices, which leave it alone (only a device on the display is a
 * pointer there, 0022-…). Its events read without blocking into the buttons held, the
 * D-pad from the hat, BTN_DPAD_* or the left stick: an 8BitDo M30 posing as a DualShock 4
 * sends its D-pad as the stick's ends. Without one, /dev/input is looked through again once
 * a second; one disconnected reads ENODEV (the tests' FIFO: the end of the file).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#include "ui.h"

#define SCAN_MS 1000

struct ui_gamepad {
	int fd;             /* -1: none connected */
	char name[64];      /* the controller's */
	char log[32];       /* the app's, for the log */
	bool scanned;       /* looked for one at `scan_ms` */
	uint64_t scan_ms;
	unsigned keys;      /* held, from buttons (BTN_DPAD_* too) */
	unsigned hat;       /* the D-pad's directions from the hat */
	unsigned stick;     /* and from the left stick */
	unsigned pressed;   /* went down during the last read */
	int lo[2], hi[2];   /* the left stick's X and Y: below and above, past half-way */
};

static const struct {
	int code;
	enum ui_pad_button button;
} buttons[] = {
	{ BTN_SOUTH, UI_PAD_SOUTH }, { BTN_EAST, UI_PAD_EAST }, { BTN_WEST, UI_PAD_WEST },
	{ BTN_NORTH, UI_PAD_NORTH }, { BTN_C, UI_PAD_C }, { BTN_Z, UI_PAD_Z },
	{ BTN_TL, UI_PAD_L1 }, { BTN_TR, UI_PAD_R1 }, { BTN_TL2, UI_PAD_L2 }, { BTN_TR2, UI_PAD_R2 },
	{ BTN_SELECT, UI_PAD_SELECT }, { BTN_START, UI_PAD_START }, { BTN_MODE, UI_PAD_HOME },
	{ BTN_THUMBL, UI_PAD_L3 }, { BTN_THUMBR, UI_PAD_R3 },
	{ BTN_DPAD_UP, UI_PAD_UP }, { BTN_DPAD_DOWN, UI_PAD_DOWN },
	{ BTN_DPAD_LEFT, UI_PAD_LEFT }, { BTN_DPAD_RIGHT, UI_PAD_RIGHT },
};

#define BIT(b) (1u << (b))

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool is_gamepad(int fd)
{
	unsigned char keys[KEY_MAX / 8 + 1] = { 0 };
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
	       keys[BTN_GAMEPAD / 8] & 1 << BTN_GAMEPAD % 8;
}

/* The first controller in /dev/input, if any */
static void scan(struct ui_gamepad *g)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && g->fd < 0 && (e = readdir(d))) {
		char path[300];
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_gamepad(fd)) {
			close(fd);
			continue;
		}
		g->fd = fd;
		if (ioctl(fd, EVIOCGNAME(sizeof(g->name)), g->name) < 0)
			snprintf(g->name, sizeof(g->name), "%.32s", e->d_name);
		for (int i = 0; i < 2; i++) {
			struct input_absinfo a;
			if (ioctl(fd, EVIOCGABS(ABS_X + i), &a) == 0 && a.maximum > a.minimum) {
				g->lo[i] = a.minimum + (a.maximum - a.minimum) / 4;
				g->hi[i] = a.maximum - (a.maximum - a.minimum) / 4;
			} else {
				g->lo[i] = INT_MIN; /* no stick: never past */
				g->hi[i] = INT_MAX;
			}
		}
		printf("%s: game controller %s (%s)\n", g->log, g->name, path);
	}
	if (d)
		closedir(d);
}

static void gone(struct ui_gamepad *g)
{
	printf("%s: game controller %s gone\n", g->log, g->name);
	close(g->fd);
	g->fd = -1;
	g->keys = g->hat = g->stick = 0;
	g->name[0] = '\0';
}

/* Of a pair of directions (`neg`, then the next one): `neg` if less, the other if more */
static unsigned direction(enum ui_pad_button neg, bool less, bool more)
{
	return less ? BIT(neg) : more ? BIT(neg + 1) : 0;
}

static void event(struct ui_gamepad *g, const struct input_event *ev)
{
	unsigned before = g->keys | g->hat | g->stick;
	if (ev->type == EV_KEY) {
		for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); i++)
			if (buttons[i].code == ev->code) {
				if (ev->value)
					g->keys |= BIT(buttons[i].button);
				else
					g->keys &= ~BIT(buttons[i].button);
			}
	} else if (ev->type == EV_ABS && (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y)) {
		enum ui_pad_button neg = ev->code == ABS_HAT0X ? UI_PAD_LEFT : UI_PAD_UP;
		g->hat = (g->hat & ~(BIT(neg) | BIT(neg + 1))) | direction(neg, ev->value < 0, ev->value > 0);
	} else if (ev->type == EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y)) {
		int i = ev->code == ABS_Y;
		enum ui_pad_button neg = i ? UI_PAD_UP : UI_PAD_LEFT;
		g->stick = (g->stick & ~(BIT(neg) | BIT(neg + 1))) |
		           direction(neg, ev->value < g->lo[i], ev->value > g->hi[i]);
	}
	g->pressed |= (g->keys | g->hat | g->stick) & ~before;
}

struct ui_gamepad *ui_gamepad_open(const char *name)
{
	struct ui_gamepad *g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->fd = -1;
	snprintf(g->log, sizeof(g->log), "%s", name);
	return g;
}

unsigned ui_gamepad_read(struct ui_gamepad *g)
{
	if (!g)
		return 0;
	g->pressed = 0;
	if (g->fd < 0) {
		uint64_t t = now_ms();
		if (g->scanned && t - g->scan_ms < SCAN_MS)
			return 0;
		g->scanned = true;
		g->scan_ms = t;
		scan(g);
	}
	struct input_event ev;
	ssize_t n;
	while (g->fd >= 0) {
		n = read(g->fd, &ev, sizeof(ev));
		if (n == sizeof(ev)) {
			event(g, &ev);
		} else {
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				gone(g);
				g->scan_ms = now_ms();
			}
			break;
		}
	}
	return g->keys | g->hat | g->stick;
}

unsigned ui_gamepad_pressed(const struct ui_gamepad *g)
{
	return g ? g->pressed : 0;
}

const char *ui_gamepad_name(const struct ui_gamepad *g)
{
	return g && g->fd >= 0 ? g->name : NULL;
}

void ui_gamepad_close(struct ui_gamepad *g)
{
	if (!g)
		return;
	if (g->fd >= 0)
		close(g->fd);
	free(g);
}
