/*
 * Bluetooth through BlueZ (bluetoothd) on the system bus (bluetooth.c, libdbus): the
 * first adapter, the devices it knows (paired ones, those found while discovering) and
 * pairing with them, the codes confirmed through an agent of phone-ui's. Nothing is
 * read in the background: bt_refresh() asks BlueZ for its objects, bt_poll() answers
 * what it asked meanwhile (the agent) and finishes pairing and connecting.
 */

#ifndef PHONE_UI_BLUETOOTH_H
#define PHONE_UI_BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

#define BT_MAX_DEVICES 32

struct bt_device {
	char path[80];    /* BlueZ's object, /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF */
	char address[18];
	char name[64];    /* its Name, "" without one (its Alias is the address then) */
	int rssi;         /* dBm, 0 when not heard in this discovery */
	bool paired, connected;
};

enum bt_state { BT_UNAVAILABLE, BT_OFF, BT_ON };

/* Asks BlueZ for the adapter and devices: false when it cannot be reached */
bool bt_refresh(void);
/* What the last refresh found */
enum bt_state bt_state(void);
const char *bt_name(void); /* the adapter's name as others see it, "" if none */
/* Its devices: paired first (by name), then the others with a name, strongest first */
unsigned bt_devices(struct bt_device *out, unsigned max);
const struct bt_device *bt_device(const char *path); /* NULL if gone */

/* On or off, remembered ("bluetooth" setting, off by default) */
void bt_set_powered(bool on);
bool bt_enabled(void);
/* At startup (settings_apply()): the saved state, set by bt_tick() once the adapter is up */
void bt_apply(void);
void bt_tick(void); /* each second (status.c) */
/* For the status bar, as of the last bt_tick(): -1 off (or no adapter), 0 on, 1 a device connected */
int bt_status(void);

/* Where phone-ui's sound plays: the ALSA device of the Bluetooth audio device connected
 * (an A2DP sink through bluez-alsa: "phone_bt:DEV=AA:BB:CC:DD:EE:FF,RATE=48000", the board's
 * ALSA configuration), NULL for the phone's own. Checked each second by bt_tick() while
 * Bluetooth is on. */
const char *bt_audio_device(void);
/* A headset for calls connected: bluez-alsa has a Hands-Free sink (HFP-AG) of a device, as
 * of the last bt_tick(); modem_call_headset() is told when it changes */
bool bt_call_audio(void);
/* Its volume, 0 (muted) to 127 (bluez-alsa's, as of the last check: its own buttons change it
 * too), -1 without one; set on both channels */
int bt_audio_volume(void);
void bt_audio_volume_set(int volume);

/* The Bluetooth page is open: discovering, visible to others, pairing answered */
void bt_active(bool on);
/* Answers BlueZ's calls, finishes pairing and connecting: often while active */
void bt_poll(void);

/* Pairing, one device at a time: pairing then connecting (bt_poll()) */
enum bt_pairing {
	BT_PAIR_IDLE,
	BT_PAIR_BUSY,    /* asked, waiting for the device */
	BT_PAIR_CONFIRM, /* the user should confirm (code: shown on both, "" none) */
	BT_PAIR_SHOW,    /* the code to type on the device */
	BT_PAIR_DONE,
	BT_PAIR_FAILED,
};
struct bt_pairing_info {
	enum bt_pairing state;
	char path[80];    /* the device */
	char code[17];
};
void bt_pair(const char *path);
void bt_pair_confirm(bool yes);
const struct bt_pairing_info *bt_pairing(void);
void bt_pairing_clear(void); /* DONE or FAILED seen */
/* Connecting a paired device, or disconnecting it */
void bt_connect(const char *path, bool on);
const char *bt_connecting(void); /* the device being connected, "" none */

#endif
