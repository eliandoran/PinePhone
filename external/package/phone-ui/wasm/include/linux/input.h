/* The part of Linux's <linux/input.h> phone-ui uses (the vibrator's force feedback,
 * input devices' names): Emscripten has no Linux headers. Nothing answers them. */
#ifndef PHONE_UI_WASM_LINUX_INPUT_H
#define PHONE_UI_WASM_LINUX_INPUT_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/input-event-codes.h>

struct input_event {
	struct timeval time;
	uint16_t type, code;
	int32_t value;
};

struct ff_replay {
	uint16_t length, delay;
};

struct ff_trigger {
	uint16_t button, interval;
};

struct ff_rumble_effect {
	uint16_t strong_magnitude, weak_magnitude;
};

struct ff_effect {
	uint16_t type;
	int16_t id;
	uint16_t direction;
	struct ff_trigger trigger;
	struct ff_replay replay;
	union {
		struct ff_rumble_effect rumble;
	} u;
};

#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCSFF _IOW('E', 0x80, struct ff_effect)

#endif
