/* The codes of Linux's <linux/input-event-codes.h> phone-ui uses */
#ifndef PHONE_UI_WASM_INPUT_EVENT_CODES_H
#define PHONE_UI_WASM_INPUT_EVENT_CODES_H

/* A device on the display, a touchscreen (Settings › Display's tap to wake looks for it) */
#define INPUT_PROP_DIRECT 0x01

#define EV_KEY 0x01
#define EV_FF 0x15
#define FF_RUMBLE 0x50

#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115
#define KEY_POWER 116
/* A Bluetooth headset's media buttons (Music) */
#define KEY_NEXTSONG 163
#define KEY_PLAYPAUSE 164
#define KEY_PREVIOUSSONG 165
#define KEY_STOPCD 166
#define KEY_PLAYCD 200
#define KEY_PAUSECD 201

#endif
