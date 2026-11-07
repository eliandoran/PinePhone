/*
 * Bluetooth through BlueZ (bluetooth.h): bluetoothd's objects on the system bus, asked
 * with libdbus on a private connection (one that does not exit the program when the
 * bus goes, as dbus_bus_get()'s does), opened when first needed and again after
 * bluetoothd or the bus went away, at most every few seconds while they are missing.
 *
 * Quick calls (properties, discovery, GetManagedObjects) wait for their answer, a
 * second at most, as wifi.c does wpa_supplicant's. Pairing and connecting take as long
 * as the device does (a person confirming a code): sent, and their answers picked up
 * by bt_poll(). The agent (org.bluez.Agent1, DisplayYesNo: a code is shown and
 * confirmed with a tap) is registered while the page is open; its RequestConfirmation
 * is answered when the user taps, the call kept until then.
 *
 * bluetoothd starts with the adapter off (AutoEnable=false in the board's main.conf):
 * phone-ui turns it on when the "bluetooth" setting says so, once it shows up after boot.
 */

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bluetooth.h"
#include "ui.h"

/* NON-NLS-BEGIN: BlueZ's names and the log, no text a user reads (settings/bluetooth.c's) */

#ifndef BT_BUS
#define BT_BUS "unix:path=/run/dbus/system_bus_socket"
#endif
#define BLUEZ "org.bluez"
#define AGENT_PATH "/phoneui/agent"
#define CALL_MS 1000
#define PAIR_MS 90000    /* a person typing or confirming a code, as BlueZ's own limit */
#define CONNECT_MS 30000
#define RETRY_MS 3000    /* the bus or bluetoothd missing: tried again after that */
#define ENSURE_MS 5000   /* discovery and visibility set again (BlueZ ends both) */
#define APPLY_TICKS 60   /* seconds after startup for the adapter to show up */

static DBusConnection *conn;
static uint64_t opened_ms; /* the last attempt to open */
static bool logged;        /* the bus missing, said once */

/* What the last bt_refresh() found */
static struct {
	bool adapter;
	char path[80];
	char name[64];
	bool powered, discovering, discoverable;
	struct bt_device devices[BT_MAX_DEVICES];
	unsigned count;
} found;

static bool active;        /* the page is open */
static bool agent;         /* registered */
static uint64_t ensured_ms;
static int apply_ticks;    /* left to turn the adapter on at startup */

static struct bt_pairing_info pairing;
static DBusPendingCall *pair_call;
static DBusMessage *request; /* the agent's question, answered by bt_pair_confirm() */
static DBusPendingCall *connect_call;
static char connecting[80];

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void forget_calls(void)
{
	if (pair_call) {
		dbus_pending_call_cancel(pair_call);
		dbus_pending_call_unref(pair_call);
		pair_call = NULL;
	}
	if (connect_call) {
		dbus_pending_call_cancel(connect_call);
		dbus_pending_call_unref(connect_call);
		connect_call = NULL;
	}
	if (request) {
		dbus_message_unref(request);
		request = NULL;
	}
	connecting[0] = '\0';
	if (pairing.state != BT_PAIR_IDLE && pairing.state != BT_PAIR_DONE)
		pairing.state = BT_PAIR_FAILED;
}

static void bus_close(void)
{
	if (!conn)
		return;
	forget_calls();
	dbus_connection_close(conn);
	dbus_connection_unref(conn);
	conn = NULL;
	agent = false;
	found.adapter = false;
}

static DBusHandlerResult agent_message(DBusConnection *c, DBusMessage *m, void *data);

static bool bus_open(void)
{
	if (conn && dbus_connection_get_is_connected(conn))
		return true;
	bus_close();
	uint64_t now = now_ms();
	if (opened_ms && now - opened_ms < RETRY_MS)
		return false;
	opened_ms = now;

	DBusError err;
	dbus_error_init(&err);
	conn = dbus_connection_open_private(BT_BUS, &err);
	if (conn && !dbus_bus_register(conn, &err)) {
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		conn = NULL;
	}
	if (!conn) {
		if (!logged)
			printf("phone-ui: no system bus for Bluetooth: %s\n", err.message ? err.message : "?");
		logged = true;
		dbus_error_free(&err);
		return false;
	}
	logged = false;
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	static const DBusObjectPathVTable vtable = { .message_function = agent_message };
	dbus_connection_register_object_path(conn, AGENT_PATH, &vtable, NULL);
	return true;
}

/* Sends a call and waits for its answer: false (logged) on an error or none */
static bool call_message(DBusMessage *m, DBusMessage **reply)
{
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(conn, m, CALL_MS, &err);
	if (!r) {
		/* Answers BlueZ refuses with are expected (StopDiscovery when stopped) */
		if (strncmp(err.name ? err.name : "", "org.bluez.Error.", 16) != 0)
			printf("phone-ui: Bluetooth: %s.%s: %s\n", dbus_message_get_interface(m),
			       dbus_message_get_member(m), err.message ? err.message : "?");
		dbus_error_free(&err);
		if (!dbus_connection_get_is_connected(conn))
			bus_close();
		return false;
	}
	if (reply)
		*reply = r;
	else
		dbus_message_unref(r);
	return true;
}

/* A method of BlueZ's without arguments, or with an object path and a string (`s`
 * NULL: the path only) */
static bool call(const char *path, const char *iface, const char *method, const char *o, const char *s)
{
	if (!conn)
		return false;
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, iface, method);
	if (!m)
		return false;
	if (o && s)
		dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &o, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
	else if (o)
		dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &o, DBUS_TYPE_INVALID);
	bool ok = call_message(m, NULL);
	dbus_message_unref(m);
	return ok;
}

static bool set_bool(const char *path, const char *iface, const char *name, bool value)
{
	if (!conn)
		return false;
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, DBUS_INTERFACE_PROPERTIES, "Set");
	if (!m)
		return false;
	DBusMessageIter it, v;
	dbus_bool_t b = value;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
	dbus_message_iter_close_container(&it, &v);
	bool ok = call_message(m, NULL);
	dbus_message_unref(m);
	return ok;
}

/* Sends a call whose answer bt_poll() picks up */
static DBusPendingCall *call_later(const char *path, const char *iface, const char *method, int ms)
{
	DBusPendingCall *pending = NULL;
	DBusMessage *m = conn ? dbus_message_new_method_call(BLUEZ, path, iface, method) : NULL;
	if (!m)
		return NULL;
	if (!dbus_connection_send_with_reply(conn, m, &pending, ms))
		pending = NULL;
	dbus_message_unref(m);
	dbus_connection_flush(conn);
	return pending;
}

/* GetManagedObjects' answer: a{oa{sa{sv}}} */

static void copy_string(DBusMessageIter *v, char *out, size_t size)
{
	const char *s;
	int type = dbus_message_iter_get_arg_type(v);
	if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
		return;
	dbus_message_iter_get_basic(v, &s);
	snprintf(out, size, "%s", s);
}

static bool get_bool(DBusMessageIter *v)
{
	dbus_bool_t b = FALSE;
	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN)
		dbus_message_iter_get_basic(v, &b);
	return b;
}

struct object {
	char path[80];
	char adapter[80]; /* a device's */
	char name[64], alias[64];
	struct bt_device d;
};

/* An interface's properties, of the adapter or a device */
static void properties(DBusMessageIter *props, bool adapter, struct object *o)
{
	while (dbus_message_iter_get_arg_type(props) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, v;
		const char *name;
		dbus_message_iter_recurse(props, &entry);
		dbus_message_iter_get_basic(&entry, &name);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &v);
		if (adapter) {
			if (strcmp(name, "Alias") == 0)
				copy_string(&v, o->alias, sizeof(o->alias));
			else if (strcmp(name, "Powered") == 0)
				found.powered = get_bool(&v);
			else if (strcmp(name, "Discovering") == 0)
				found.discovering = get_bool(&v);
			else if (strcmp(name, "Discoverable") == 0)
				found.discoverable = get_bool(&v);
		} else if (strcmp(name, "Address") == 0) {
			copy_string(&v, o->d.address, sizeof(o->d.address));
		} else if (strcmp(name, "Name") == 0) {
			copy_string(&v, o->d.name, sizeof(o->d.name));
		} else if (strcmp(name, "Adapter") == 0) {
			copy_string(&v, o->adapter, sizeof(o->adapter));
		} else if (strcmp(name, "Paired") == 0) {
			o->d.paired = get_bool(&v);
		} else if (strcmp(name, "Connected") == 0) {
			o->d.connected = get_bool(&v);
		} else if (strcmp(name, "RSSI") == 0 && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_INT16) {
			dbus_int16_t rssi;
			dbus_message_iter_get_basic(&v, &rssi);
			o->d.rssi = rssi;
		}
		dbus_message_iter_next(props);
	}
}

static int by_kind(const void *a, const void *b)
{
	const struct bt_device *x = a, *y = b;
	if (x->paired != y->paired)
		return x->paired ? -1 : 1;
	if (x->paired)
		return strcmp(x->name[0] ? x->name : x->address, y->name[0] ? y->name : y->address);
	return y->rssi - x->rssi;
}

static bool objects_read(DBusMessage *r)
{
	static struct object devices[64];
	unsigned count = 0;
	DBusMessageIter it, objects;
	if (!dbus_message_iter_init(r, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
		return false;
	found.adapter = false;
	found.powered = found.discovering = found.discoverable = false;
	found.name[0] = '\0';
	dbus_message_iter_recurse(&it, &objects);
	while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, ifaces;
		struct object o = { 0 };
		dbus_message_iter_recurse(&objects, &entry);
		copy_string(&entry, o.path, sizeof(o.path));
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &ifaces);
		while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter ientry, props;
			const char *iface;
			dbus_message_iter_recurse(&ifaces, &ientry);
			dbus_message_iter_get_basic(&ientry, &iface);
			dbus_message_iter_next(&ientry);
			dbus_message_iter_recurse(&ientry, &props);
			if (strcmp(iface, "org.bluez.Adapter1") == 0 && !found.adapter) {
				found.adapter = true;
				snprintf(found.path, sizeof(found.path), "%s", o.path);
				properties(&props, true, &o);
				snprintf(found.name, sizeof(found.name), "%s", o.alias);
			} else if (strcmp(iface, "org.bluez.Device1") == 0 && count < sizeof(devices) / sizeof(devices[0])) {
				properties(&props, false, &o);
				snprintf(o.d.path, sizeof(o.d.path), "%s", o.path);
				devices[count++] = o;
			}
			dbus_message_iter_next(&ifaces);
		}
		dbus_message_iter_next(&objects);
	}

	/* The adapter's devices: paired ones, and those with a name heard in this discovery */
	found.count = 0;
	for (unsigned i = 0; i < count && found.count < BT_MAX_DEVICES; i++) {
		const struct object *o = &devices[i];
		if (!found.adapter || strcmp(o->adapter, found.path) != 0)
			continue;
		if (o->d.paired || (o->d.name[0] && o->d.rssi))
			found.devices[found.count++] = o->d;
	}
	if (found.count)
		qsort(found.devices, found.count, sizeof(found.devices[0]), by_kind);
	return true;
}

bool bt_refresh(void)
{
	if (!bus_open()) {
		found.adapter = false;
		return false;
	}
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
	DBusMessage *r = NULL;
	bool ok = m && call_message(m, &r) && objects_read(r);
	if (m)
		dbus_message_unref(m);
	if (r)
		dbus_message_unref(r);
	if (!ok) {
		found.adapter = false;
		agent = false; /* bluetoothd gone: registered again when it is back */
	}
	return ok;
}

enum bt_state bt_state(void)
{
	return !found.adapter ? BT_UNAVAILABLE : found.powered ? BT_ON : BT_OFF;
}

const char *bt_name(void)
{
	return found.adapter ? found.name : "";
}

unsigned bt_devices(struct bt_device *out, unsigned max)
{
	unsigned n = found.adapter ? found.count : 0;
	if (n > max)
		n = max;
	memcpy(out, found.devices, n * sizeof(*out));
	return n;
}

const struct bt_device *bt_device(const char *path)
{
	for (unsigned i = 0; found.adapter && i < found.count; i++)
		if (strcmp(found.devices[i].path, path) == 0)
			return &found.devices[i];
	return NULL;
}

/* On and off */

bool bt_enabled(void)
{
	return ui_setting_get_int("bluetooth", 0) != 0;
}

void bt_set_powered(bool on)
{
	ui_setting_set_int("bluetooth", on);
	apply_ticks = 0;
	if (!found.adapter && !bt_refresh())
		return;
	if (found.adapter && set_bool(found.path, "org.bluez.Adapter1", "Powered", on))
		found.powered = on;
	ensured_ms = 0;
}

void bt_apply(void)
{
	apply_ticks = bt_enabled() ? APPLY_TICKS : 0;
}

/* Sound: bluez-alsa's PCMs */

static char audio[96];       /* bt_audio_device(), "" none */
static char audio_pcm[128];  /* its bluez-alsa PCM */
static int audio_volume = -1;

/* The first A2DP sink bluez-alsa has a PCM of (org.bluealsa's objects: a{oa{sa{sv}}}), as
 * the ALSA device of the board's configuration; "" without one, or without bluez-alsa */
static void audio_refresh(void)
{
	char device[96] = "";
	char pcm[128] = "";
	int volume = -1;
	DBusMessage *m = conn ? dbus_message_new_method_call("org.bluealsa", "/org/bluealsa",
	                                                     "org.freedesktop.DBus.ObjectManager",
	                                                     "GetManagedObjects") : NULL;
	DBusMessage *r = m ? dbus_connection_send_with_reply_and_block(conn, m, CALL_MS, NULL) : NULL;
	DBusMessageIter it, objects;
	if (r && dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &objects);
		for (; !device[0] && dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY;
		     dbus_message_iter_next(&objects)) {
			DBusMessageIter entry, ifaces, ientry, props;
			dbus_message_iter_recurse(&objects, &entry);
			char path[128] = "";
			copy_string(&entry, path, sizeof(path));
			dbus_message_iter_next(&entry);
			dbus_message_iter_recurse(&entry, &ifaces);
			if (dbus_message_iter_get_arg_type(&ifaces) != DBUS_TYPE_DICT_ENTRY)
				continue;
			dbus_message_iter_recurse(&ifaces, &ientry);
			dbus_message_iter_next(&ientry);
			dbus_message_iter_recurse(&ientry, &props);
			char dev[80] = "", transport[32] = "", mode[16] = "";
			dbus_uint32_t rate = 0;
			dbus_uint16_t raw = 0;
			for (; dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&props)) {
				DBusMessageIter e, v;
				const char *name;
				dbus_message_iter_recurse(&props, &e);
				dbus_message_iter_get_basic(&e, &name);
				dbus_message_iter_next(&e);
				dbus_message_iter_recurse(&e, &v);
				if (strcmp(name, "Device") == 0)
					copy_string(&v, dev, sizeof(dev));
				else if (strcmp(name, "Transport") == 0)
					copy_string(&v, transport, sizeof(transport));
				else if (strcmp(name, "Mode") == 0)
					copy_string(&v, mode, sizeof(mode));
				else if (strcmp(name, "Sampling") == 0 && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT32)
					dbus_message_iter_get_basic(&v, &rate);
				else if (strcmp(name, "Volume") == 0 && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT16)
					dbus_message_iter_get_basic(&v, &raw); /* left channel high: muted (bit 7), level */
			}
			/* /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF */
			const char *addr = strstr(dev, "/dev_");
			if (!addr || strlen(addr + 5) != 17 || !strstr(transport, "A2DP-source") || strcmp(mode, "sink") != 0 ||
			    !rate)
				continue;
			char address[18];
			snprintf(address, sizeof(address), "%s", addr + 5);
			for (char *c = address; *c; c++)
				if (*c == '_')
					*c = ':';
			snprintf(device, sizeof(device), "phone_bt:DEV=%s,RATE=%u", address, (unsigned)rate);
			snprintf(pcm, sizeof(pcm), "%s", path);
			volume = raw & 0x8000 ? 0 : (raw >> 8) & 0x7f;
		}
	}
	if (m)
		dbus_message_unref(m);
	if (r)
		dbus_message_unref(r);
	if (strcmp(device, audio) != 0)
		printf("phone-ui: sound to %s\n", device[0] ? device : "the phone");
	snprintf(audio, sizeof(audio), "%s", device);
	snprintf(audio_pcm, sizeof(audio_pcm), "%s", pcm);
	audio_volume = volume;
}

const char *bt_audio_device(void)
{
	return audio[0] ? audio : NULL;
}

int bt_audio_volume(void)
{
	return audio[0] ? audio_volume : -1;
}

/* bluez-alsa's Volume: each channel's level (0-127) and muted bit, the left one high */
void bt_audio_volume_set(int volume)
{
	if (!audio[0] || !conn)
		return;
	volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
	unsigned channel = volume ? (unsigned)volume : 0x80;
	dbus_uint16_t raw = (dbus_uint16_t)(channel << 8 | channel);
	const char *iface = "org.bluealsa.PCM1", *name = "Volume";
	DBusMessage *m = dbus_message_new_method_call("org.bluealsa", audio_pcm, DBUS_INTERFACE_PROPERTIES, "Set");
	if (!m)
		return;
	DBusMessageIter it, v;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT16_AS_STRING, &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT16, &raw);
	dbus_message_iter_close_container(&it, &v);
	if (call_message(m, NULL))
		audio_volume = volume;
	dbus_message_unref(m);
}

void bt_tick(void)
{
	if (apply_ticks > 0) {
		apply_ticks--;
		if (bt_refresh()) {
			if (!found.powered && set_bool(found.path, "org.bluez.Adapter1", "Powered", true))
				printf("phone-ui: Bluetooth on\n");
			apply_ticks = 0;
		}
	}
	/* Bluetooth on, or turned off with a device still playing */
	if (bt_enabled() || audio[0]) {
		if (!bus_open())
			audio[0] = '\0';
		else
			audio_refresh();
	}
}

/* The page */

/* The agent registered, discovering and visible while the page is open and the adapter on;
 * set again every few seconds: BlueZ ends visibility after its timeout */
static void ensure(void)
{
	if (!active || !conn || !found.adapter)
		return;
	if (!agent) {
		call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", AGENT_PATH, NULL); /* an earlier one's */
		agent = call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", AGENT_PATH, "DisplayYesNo") &&
		        call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent", AGENT_PATH, NULL);
	}
	uint64_t now = now_ms();
	if (!found.powered || (ensured_ms && now - ensured_ms < ENSURE_MS))
		return;
	ensured_ms = now;
	if (!found.discovering && call(found.path, "org.bluez.Adapter1", "StartDiscovery", NULL, NULL))
		found.discovering = true;
	if (!found.discoverable && set_bool(found.path, "org.bluez.Adapter1", "Discoverable", true))
		found.discoverable = true;
}

void bt_active(bool on)
{
	if (on == active)
		return;
	active = on;
	ensured_ms = 0;
	if (on) {
		bt_refresh();
		ensure();
		return;
	}
	if (request)
		bt_pair_confirm(false);
	if (!conn || !found.adapter)
		return;
	if (found.discovering)
		call(found.path, "org.bluez.Adapter1", "StopDiscovery", NULL, NULL);
	if (found.discoverable)
		set_bool(found.path, "org.bluez.Adapter1", "Discoverable", false);
	if (agent)
		call("/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent", AGENT_PATH, NULL);
	agent = false;
	found.discovering = found.discoverable = false;
}

/* The agent: BlueZ's questions while pairing */

static void reply_empty(DBusMessage *m)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	if (r) {
		dbus_connection_send(conn, r, NULL);
		dbus_message_unref(r);
	}
}

static void reply_rejected(DBusMessage *m)
{
	DBusMessage *r = dbus_message_new_error(m, "org.bluez.Error.Rejected", "Rejected");
	if (r) {
		dbus_connection_send(conn, r, NULL);
		dbus_message_unref(r);
	}
}

static DBusHandlerResult agent_message(DBusConnection *c, DBusMessage *m, void *data)
{
	LV_UNUSED(c);
	LV_UNUSED(data);
	const char *device = "", *pin = "";
	dbus_uint32_t passkey = 0;
	if (!dbus_message_has_interface(m, "org.bluez.Agent1"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_method_call(m, "org.bluez.Agent1", "RequestConfirmation") ||
	    dbus_message_is_method_call(m, "org.bluez.Agent1", "RequestAuthorization")) {
		/* Confirmed by a tap; a code with RequestConfirmation (on both screens) */
		dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
		                      DBUS_TYPE_INVALID);
		if (request)
			bt_pair_confirm(false);
		request = dbus_message_ref(m);
		pairing.state = BT_PAIR_CONFIRM;
		snprintf(pairing.path, sizeof(pairing.path), "%s", device);
		if (dbus_message_has_member(m, "RequestConfirmation"))
			snprintf(pairing.code, sizeof(pairing.code), "%06u", (unsigned)passkey);
		else
			pairing.code[0] = '\0';
	} else if (dbus_message_is_method_call(m, "org.bluez.Agent1", "DisplayPasskey") ||
	           dbus_message_is_method_call(m, "org.bluez.Agent1", "DisplayPinCode")) {
		/* A code to type on the device (a keyboard) */
		if (dbus_message_has_member(m, "DisplayPasskey")) {
			dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
			                      DBUS_TYPE_INVALID);
			snprintf(pairing.code, sizeof(pairing.code), "%06u", (unsigned)passkey);
		} else {
			dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING, &pin,
			                      DBUS_TYPE_INVALID);
			snprintf(pairing.code, sizeof(pairing.code), "%s", pin);
		}
		pairing.state = BT_PAIR_SHOW;
		snprintf(pairing.path, sizeof(pairing.path), "%s", device);
		reply_empty(m);
	} else if (dbus_message_is_method_call(m, "org.bluez.Agent1", "AuthorizeService")) {
		reply_empty(m); /* a paired device's profile */
	} else if (dbus_message_is_method_call(m, "org.bluez.Agent1", "Cancel")) {
		if (request) {
			dbus_message_unref(request);
			request = NULL;
		}
		if (pairing.state == BT_PAIR_CONFIRM || pairing.state == BT_PAIR_SHOW)
			pairing.state = pair_call ? BT_PAIR_BUSY : BT_PAIR_IDLE;
	} else if (dbus_message_is_method_call(m, "org.bluez.Agent1", "Release")) {
		agent = false;
		reply_empty(m);
	} else if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
		/* RequestPinCode, RequestPasskey: a code typed here, not yet */
		reply_rejected(m);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

void bt_pair_confirm(bool yes)
{
	if (!request)
		return;
	if (yes)
		reply_empty(request);
	else
		reply_rejected(request);
	dbus_connection_flush(conn);
	dbus_message_unref(request);
	request = NULL;
	/* Asked by another device: nothing of ours waits for an answer */
	pairing.state = pair_call ? BT_PAIR_BUSY : yes ? BT_PAIR_IDLE : BT_PAIR_FAILED;
}

void bt_pair(const char *path)
{
	if (pair_call || !conn)
		return;
	pair_call = call_later(path, "org.bluez.Device1", "Pair", PAIR_MS);
	pairing.state = pair_call ? BT_PAIR_BUSY : BT_PAIR_FAILED;
	snprintf(pairing.path, sizeof(pairing.path), "%s", path);
	pairing.code[0] = '\0';
}

const struct bt_pairing_info *bt_pairing(void)
{
	return &pairing;
}

void bt_pairing_clear(void)
{
	if (pairing.state == BT_PAIR_DONE || pairing.state == BT_PAIR_FAILED)
		pairing.state = BT_PAIR_IDLE;
}

void bt_connect(const char *path, bool on)
{
	if (connect_call || !conn)
		return;
	connect_call = call_later(path, "org.bluez.Device1", on ? "Connect" : "Disconnect", CONNECT_MS);
	snprintf(connecting, sizeof(connecting), "%s", connect_call && on ? path : "");
}

const char *bt_connecting(void)
{
	return connecting;
}

/* The answer of a call sent with call_later(), once there: NULL error name if it succeeded */
static bool answered(DBusPendingCall **pending, const char **error)
{
	static char name[96];
	if (!*pending || !dbus_pending_call_get_completed(*pending))
		return false;
	DBusMessage *r = dbus_pending_call_steal_reply(*pending);
	dbus_pending_call_unref(*pending);
	*pending = NULL;
	*error = NULL;
	if (!r || dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		snprintf(name, sizeof(name), "%s", r && dbus_message_get_error_name(r) ? dbus_message_get_error_name(r)
		                                                                       : "no answer");
		*error = name;
	}
	if (r)
		dbus_message_unref(r);
	return true;
}

void bt_poll(void)
{
	const char *error;
	if (!conn)
		return;
	dbus_connection_read_write(conn, 0);
	while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
		;
	if (!dbus_connection_get_is_connected(conn)) {
		bus_close();
		return;
	}

	if (answered(&pair_call, &error)) {
		if (error && strcmp(error, "org.bluez.Error.AlreadyExists") != 0) {
			printf("phone-ui: Bluetooth: pairing %s: %s\n", pairing.path, error);
			pairing.state = BT_PAIR_FAILED;
		} else {
			/* Trusted: it connects again by itself, without asking */
			set_bool(pairing.path, "org.bluez.Device1", "Trusted", true);
			pairing.state = BT_PAIR_DONE;
			bt_connect(pairing.path, true);
		}
		if (request) {
			dbus_message_unref(request);
			request = NULL;
		}
	}
	if (answered(&connect_call, &error)) {
		if (error) /* a phone or a computer: paired, nothing to connect to */
			printf("phone-ui: Bluetooth: connecting %s: %s\n", connecting, error);
		connecting[0] = '\0';
	}
	ensure();
}
/* NON-NLS-END */
