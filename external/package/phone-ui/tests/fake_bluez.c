/*
 * BlueZ for the UI tests (ui_test.h, "Bluetooth"): a dbus-daemon of the test's own on
 * /tmp/bus (bluetooth.o's BT_BUS, the Makefile's), on which this file owns org.bluez
 * and answers from a thread what bluetoothd 5.86 answers to what bluetooth.c asks:
 * ObjectManager's GetManagedObjects, the adapter hci0's properties and discovery, the
 * devices' Pair, Connect and Disconnect, AgentManager1. Devices in range are listed
 * once discovering; pairing one with a passkey asks phone-ui's agent to confirm it.
 * bluez-alsa (org.bluealsa) is answered too: a PCM for each audio device connected.
 * Started by the first ui_test_bt_*() call: without it, there is no bus (not running).
 */

#include <dbus/dbus.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui_test.h"

#define BUS "/tmp/bus"
#define CONF "/tmp/bus.conf"
#define BLUEZ "org.bluez"
#define ADAPTER "/org/bluez/hci0"
#define MAX 16

struct device {
	char address[18];
	char name[64]; /* "" none */
	int rssi;
	unsigned passkey; /* asked to confirm on pairing, 0: pairs without asking */
	char asks[32];    /* the agent's method its pairing calls, "" RequestConfirmation */
	char code[16];    /* DisplayPinCode's */
	bool no_profiles; /* nothing to connect to (a phone, a computer) */
	unsigned audio;   /* an A2DP sink: its stream's rate while connected (bluez-alsa's PCM), 0 none */
	unsigned volume;  /* its PCM's, 0-127 (both channels) */
	bool hfp;         /* a headset for calls: bluez-alsa's HFP-AG PCMs while connected */
	bool muted;
	bool paired, connected, trusted, known; /* known: listed (paired, or found) */
	DBusMessage *pairing; /* its Pair call, answered once the agent did */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static DBusConnection *conn;
static pid_t daemon_pid;
static pthread_t thread;
static bool started, owned; /* the bus running; org.bluez on it (bluetoothd) */
static bool powered, discovering, discoverable;
static struct device devices[MAX];
static unsigned device_count;
static char agent_owner[64], agent_path[128], agent_capability[32];

static void path_of(const struct device *d, char *out, size_t size)
{
	char addr[18];
	snprintf(addr, sizeof(addr), "%s", d->address);
	for (char *c = addr; *c; c++)
		if (*c == ':')
			*c = '_';
	snprintf(out, size, ADAPTER "/dev_%s", addr);
}

static struct device *device_at(const char *path)
{
	char p[128];
	for (unsigned i = 0; i < device_count; i++) {
		path_of(&devices[i], p, sizeof(p));
		if (strcmp(p, path) == 0)
			return &devices[i];
	}
	return NULL;
}

static struct device *device_find(const char *address)
{
	for (unsigned i = 0; i < device_count; i++)
		if (strcmp(devices[i].address, address) == 0)
			return &devices[i];
	return NULL;
}

/* Properties, as a{sv} entries */

static void prop_open(DBusMessageIter *dict, const char *name, const char *sig, DBusMessageIter *entry,
                      DBusMessageIter *v)
{
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, entry);
	dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT, sig, v);
}

static void prop_close(DBusMessageIter *dict, DBusMessageIter *entry, DBusMessageIter *v)
{
	dbus_message_iter_close_container(entry, v);
	dbus_message_iter_close_container(dict, entry);
}

static void prop_basic(DBusMessageIter *dict, const char *name, int type, const void *value)
{
	char sig[2] = { (char)type, 0 };
	DBusMessageIter entry, v;
	prop_open(dict, name, sig, &entry, &v);
	dbus_message_iter_append_basic(&v, type, value);
	prop_close(dict, &entry, &v);
}

static void prop_string(DBusMessageIter *dict, const char *name, const char *value)
{
	prop_basic(dict, name, DBUS_TYPE_STRING, &value);
}

static void prop_bool(DBusMessageIter *dict, const char *name, bool value)
{
	dbus_bool_t b = value;
	prop_basic(dict, name, DBUS_TYPE_BOOLEAN, &b);
}

/* An object's entry of GetManagedObjects: path, then one interface with its properties
 * filled by `fill` */
static void object_open(DBusMessageIter *objects, const char *path, const char *iface, DBusMessageIter *entry,
                        DBusMessageIter *ifaces, DBusMessageIter *ientry, DBusMessageIter *props)
{
	dbus_message_iter_open_container(objects, DBUS_TYPE_DICT_ENTRY, NULL, entry);
	dbus_message_iter_append_basic(entry, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(entry, DBUS_TYPE_ARRAY, "{sa{sv}}", ifaces);
	dbus_message_iter_open_container(ifaces, DBUS_TYPE_DICT_ENTRY, NULL, ientry);
	dbus_message_iter_append_basic(ientry, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_open_container(ientry, DBUS_TYPE_ARRAY, "{sv}", props);
}

static void object_close(DBusMessageIter *objects, DBusMessageIter *entry, DBusMessageIter *ifaces,
                         DBusMessageIter *ientry, DBusMessageIter *props)
{
	dbus_message_iter_close_container(ientry, props);
	dbus_message_iter_close_container(ifaces, ientry);
	dbus_message_iter_close_container(entry, ifaces);
	dbus_message_iter_close_container(objects, entry);
}

static DBusMessage *managed_objects(DBusMessage *m)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter it, objects, entry, ifaces, ientry, props;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects);

	object_open(&objects, "/org/bluez", "org.bluez.AgentManager1", &entry, &ifaces, &ientry, &props);
	object_close(&objects, &entry, &ifaces, &ientry, &props);

	object_open(&objects, ADAPTER, "org.bluez.Adapter1", &entry, &ifaces, &ientry, &props);
	prop_string(&props, "Address", "00:E0:4C:23:99:87");
	prop_string(&props, "Name", "pinephone");
	prop_string(&props, "Alias", "PinePhone");
	prop_bool(&props, "Powered", powered);
	prop_bool(&props, "Discovering", discovering);
	prop_bool(&props, "Discoverable", discoverable);
	object_close(&objects, &entry, &ifaces, &ientry, &props);

	for (unsigned i = 0; i < device_count; i++) {
		struct device *d = &devices[i];
		char path[128];
		const char *adapter = ADAPTER;
		if (!d->known)
			continue;
		path_of(d, path, sizeof(path));
		object_open(&objects, path, "org.bluez.Device1", &entry, &ifaces, &ientry, &props);
		prop_string(&props, "Address", d->address);
		if (d->name[0])
			prop_string(&props, "Name", d->name);
		prop_string(&props, "Alias", d->name[0] ? d->name : d->address);
		prop_basic(&props, "Adapter", DBUS_TYPE_OBJECT_PATH, &adapter);
		prop_bool(&props, "Paired", d->paired);
		prop_bool(&props, "Bonded", d->paired);
		prop_bool(&props, "Trusted", d->trusted);
		prop_bool(&props, "Connected", d->connected);
		if (discovering) {
			dbus_int16_t rssi = (dbus_int16_t)d->rssi;
			prop_basic(&props, "RSSI", DBUS_TYPE_INT16, &rssi);
		}
		object_close(&objects, &entry, &ifaces, &ientry, &props);
	}
	dbus_message_iter_close_container(&it, &objects);
	return r;
}

/* bluez-alsa: org.bluealsa's PCMs, one per audio device connected */
static DBusMessage *bluealsa_objects(DBusMessage *m)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	DBusMessageIter it, objects, entry, ifaces, ientry, props;
	dbus_message_iter_init_append(r, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects);
	for (unsigned i = 0; i < device_count; i++) {
		struct device *d = &devices[i];
		char dev[128], path[160];
		if (!d->connected)
			continue;
		path_of(d, dev, sizeof(dev));
		const char *p = dev;
		/* A headset's call audio: a sink and a source, 8 kHz CVSD, its gain 0-15 */
		for (int source = 0; d->hfp && source < 2; source++) {
			dbus_uint32_t rate = 8000;
			dbus_uint16_t volume = 15 << 8 | 15;
			snprintf(path, sizeof(path), "/org/bluealsa/hci0/%s/hfpag/%s", strrchr(dev, '/') + 1,
			         source ? "source" : "sink");
			object_open(&objects, path, "org.bluealsa.PCM1", &entry, &ifaces, &ientry, &props);
			prop_basic(&props, "Device", DBUS_TYPE_OBJECT_PATH, &p);
			prop_string(&props, "Transport", "HFP-AG");
			prop_string(&props, "Mode", source ? "source" : "sink");
			prop_basic(&props, "Sampling", DBUS_TYPE_UINT32, &rate);
			prop_string(&props, "Codec", "CVSD");
			prop_basic(&props, "Volume", DBUS_TYPE_UINT16, &volume);
			object_close(&objects, &entry, &ifaces, &ientry, &props);
		}
		if (!d->audio)
			continue;
		snprintf(path, sizeof(path), "/org/bluealsa/hci0/%s/a2dpsrc/sink", strrchr(dev, '/') + 1);
		dbus_uint32_t rate = d->audio;
		object_open(&objects, path, "org.bluealsa.PCM1", &entry, &ifaces, &ientry, &props);
		prop_basic(&props, "Device", DBUS_TYPE_OBJECT_PATH, &p);
		prop_string(&props, "Transport", "A2DP-source");
		prop_string(&props, "Mode", "sink");
		prop_basic(&props, "Sampling", DBUS_TYPE_UINT32, &rate);
		prop_string(&props, "Codec", "SBC");
		dbus_uint16_t volume = (dbus_uint16_t)(((d->muted ? 0x80 : 0) | d->volume) << 8 | (d->muted ? 0x80 : 0) | d->volume);
		prop_basic(&props, "Volume", DBUS_TYPE_UINT16, &volume);
		object_close(&objects, &entry, &ifaces, &ientry, &props);
	}
	dbus_message_iter_close_container(&it, &objects);
	return r;
}

static DBusMessage *error(DBusMessage *m, const char *name)
{
	return dbus_message_new_error(m, name, name);
}

/* Properties.Set(s interface, s name, v value) of a boolean */
static DBusMessage *set_property(DBusMessage *m, const char *path)
{
	DBusMessageIter it, v;
	const char *iface, *name;
	dbus_bool_t value;
	if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
		return error(m, "org.freedesktop.DBus.Error.InvalidArgs");
	dbus_message_iter_get_basic(&it, &iface);
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &name);
	dbus_message_iter_next(&it);
	dbus_message_iter_recurse(&it, &v);
	if (strncmp(path, "/org/bluealsa/", 14) == 0 && strcmp(name, "Volume") == 0 &&
	    dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT16) {
		/* bluez-alsa's PCM: /org/bluealsa/hci0/dev_AA_BB.../a2dpsrc/sink, the left channel's */
		dbus_uint16_t raw;
		char dev[128];
		dbus_message_iter_get_basic(&v, &raw);
		snprintf(dev, sizeof(dev), ADAPTER "/%.*s", 21, path + 19);
		struct device *d = device_at(dev);
		if (!d || !d->connected)
			return error(m, "org.freedesktop.DBus.Error.UnknownObject");
		d->volume = (raw >> 8) & 0x7f;
		d->muted = (raw >> 8) & 0x80;
		return dbus_message_new_method_return(m);
	}
	if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_BOOLEAN)
		return error(m, "org.freedesktop.DBus.Error.InvalidArgs");
	dbus_message_iter_get_basic(&v, &value);

	if (strcmp(path, ADAPTER) == 0 && strcmp(name, "Powered") == 0) {
		powered = value;
		if (!powered) {
			discovering = discoverable = false;
			for (unsigned i = 0; i < device_count; i++)
				devices[i].connected = false;
		}
	} else if (strcmp(path, ADAPTER) == 0 && strcmp(name, "Discoverable") == 0) {
		if (!powered)
			return error(m, "org.bluez.Error.NotReady");
		discoverable = value;
	} else if (device_at(path) && strcmp(name, "Trusted") == 0) {
		device_at(path)->trusted = value;
	} else {
		return error(m, "org.freedesktop.DBus.Error.PropertyReadOnly");
	}
	return dbus_message_new_method_return(m);
}

/* The agent's answer to what pairing asked: the device's Pair answered, but for a code
 * to type on the device (then ui_test_bt_typed() does) */
static void confirmed(DBusPendingCall *call, void *data)
{
	struct device *d = data;
	DBusMessage *r = dbus_pending_call_steal_reply(call);
	pthread_mutex_lock(&lock);
	if (d->pairing && strncmp(d->asks, "Display", 7) != 0) {
		DBusMessage *answer;
		if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
			d->paired = d->trusted = true;
			answer = dbus_message_new_method_return(d->pairing);
		} else {
			answer = error(d->pairing, "org.bluez.Error.AuthenticationRejected");
		}
		dbus_connection_send(conn, answer, NULL);
		dbus_message_unref(answer);
		dbus_message_unref(d->pairing);
		d->pairing = NULL;
	}
	pthread_mutex_unlock(&lock);
	if (r)
		dbus_message_unref(r);
	dbus_pending_call_unref(call);
}

static DBusMessage *pair(DBusMessage *m, struct device *d)
{
	char path[128];
	if (d->paired)
		return error(m, "org.bluez.Error.AlreadyExists");
	if (d->pairing)
		return error(m, "org.bluez.Error.InProgress");
	if (!d->passkey && !d->asks[0]) {
		d->paired = true;
		return dbus_message_new_method_return(m);
	}
	if (!agent_owner[0])
		return error(m, "org.bluez.Error.AuthenticationFailed");
	path_of(d, path, sizeof(path));
	const char *method = d->asks[0] ? d->asks : "RequestConfirmation";
	DBusMessage *ask = dbus_message_new_method_call(agent_owner, agent_path, "org.bluez.Agent1", method);
	const char *p = path, *code = d->code;
	dbus_uint32_t passkey = d->passkey;
	dbus_uint16_t entered = 0;
	if (strcmp(method, "RequestConfirmation") == 0)
		dbus_message_append_args(ask, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
	else if (strcmp(method, "DisplayPasskey") == 0)
		dbus_message_append_args(ask, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_UINT16,
		                         &entered, DBUS_TYPE_INVALID);
	else if (strcmp(method, "DisplayPinCode") == 0)
		dbus_message_append_args(ask, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_STRING, &code, DBUS_TYPE_INVALID);
	else /* RequestAuthorization, RequestPinCode, RequestPasskey */
		dbus_message_append_args(ask, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
	DBusPendingCall *call;
	if (!dbus_connection_send_with_reply(conn, ask, &call, 60000) || !call) {
		dbus_message_unref(ask);
		return error(m, "org.bluez.Error.AuthenticationFailed");
	}
	dbus_message_unref(ask);
	d->pairing = dbus_message_ref(m);
	dbus_pending_call_set_notify(call, confirmed, d, NULL);
	return NULL; /* answered by confirmed() */
}

static DBusHandlerResult handle(DBusConnection *c, DBusMessage *m, void *data)
{
	(void)data;
	const char *path = dbus_message_get_path(m), *iface = dbus_message_get_interface(m),
	           *member = dbus_message_get_member(m);
	DBusMessage *r = NULL;
	struct device *d;
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !path || !iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	pthread_mutex_lock(&lock);
	if (strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 && strcmp(member, "GetManagedObjects") == 0 &&
	    strcmp(path, "/org/bluealsa") == 0) {
		r = bluealsa_objects(m);
	} else if (strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 && strcmp(member, "GetManagedObjects") == 0) {
		r = managed_objects(m);
	} else if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0 && strcmp(member, "Set") == 0) {
		r = set_property(m, path);
	} else if (strcmp(iface, "org.bluez.AgentManager1") == 0) {
		const char *agent = "", *capability = "";
		dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &agent, DBUS_TYPE_STRING, &capability,
		                      DBUS_TYPE_INVALID);
		if (strcmp(member, "RegisterAgent") == 0) {
			if (agent_owner[0])
				r = error(m, "org.bluez.Error.AlreadyExists");
			else {
				snprintf(agent_owner, sizeof(agent_owner), "%s", dbus_message_get_sender(m));
				snprintf(agent_path, sizeof(agent_path), "%s", agent);
				snprintf(agent_capability, sizeof(agent_capability), "%s", capability);
			}
		} else if (strcmp(member, "UnregisterAgent") == 0) {
			if (!agent_owner[0])
				r = error(m, "org.bluez.Error.DoesNotExist");
			agent_owner[0] = agent_path[0] = agent_capability[0] = '\0';
		} else if (strcmp(member, "RequestDefaultAgent") == 0 && !agent_owner[0]) {
			r = error(m, "org.bluez.Error.DoesNotExist");
		}
		if (!r)
			r = dbus_message_new_method_return(m);
	} else if (strcmp(iface, "org.bluez.Adapter1") == 0 && strcmp(path, ADAPTER) == 0) {
		if (strcmp(member, "StartDiscovery") == 0) {
			if (!powered) {
				r = error(m, "org.bluez.Error.NotReady");
			} else {
				discovering = true;
				for (unsigned i = 0; i < device_count; i++)
					devices[i].known = true;
				r = dbus_message_new_method_return(m);
			}
		} else if (strcmp(member, "StopDiscovery") == 0) {
			r = discovering ? dbus_message_new_method_return(m) : error(m, "org.bluez.Error.Failed");
			discovering = false;
		}
	} else if (strcmp(iface, "org.bluez.Device1") == 0 && (d = device_at(path))) {
		if (!powered)
			r = error(m, "org.bluez.Error.NotReady");
		else if (strcmp(member, "Pair") == 0) {
			r = pair(m, d);
			if (!r) {
				pthread_mutex_unlock(&lock);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
		} else if (strcmp(member, "Connect") == 0) {
			if (d->paired && !d->no_profiles)
				d->connected = true;
			r = d->connected ? dbus_message_new_method_return(m) : error(m, "org.bluez.Error.NotAvailable");
		} else if (strcmp(member, "Disconnect") == 0) {
			d->connected = false;
			r = dbus_message_new_method_return(m);
		}
	}
	pthread_mutex_unlock(&lock);
	if (!r)
		r = error(m, "org.freedesktop.DBus.Error.UnknownMethod");
	dbus_connection_send(c, r, NULL);
	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void *serve(void *arg)
{
	(void)arg;
	while (dbus_connection_read_write_dispatch(conn, 20))
		;
	return NULL;
}

/* The bus, then org.bluez on it */
static void start(void)
{
	if (started) {
		if (!owned)
			dbus_bus_request_name(conn, BLUEZ, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
		owned = true;
		return;
	}
	started = owned = true;

	FILE *f = fopen(CONF, "w");
	if (!f) {
		perror("fake_bluez: " CONF);
		exit(EXIT_FAILURE);
	}
	fputs("<busconfig>\n"
	      "  <listen>unix:path=" BUS "</listen>\n"
	      "  <auth>EXTERNAL</auth>\n"
	      "  <policy context=\"default\">\n"
	      "    <allow user=\"*\"/>\n"
	      "    <allow own=\"*\"/>\n"
	      "    <allow send_destination=\"*\"/>\n"
	      "    <allow receive_sender=\"*\"/>\n"
	      "  </policy>\n"
	      "</busconfig>\n", f);
	fclose(f);
	unlink(BUS);
	pid_t pid = daemon_pid = fork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL); /* gone with the test */
		execlp("dbus-daemon", "dbus-daemon", "--config-file=" CONF, "--nofork", "--nopidfile", (char *)NULL);
		perror("fake_bluez: dbus-daemon");
		_exit(127);
	}
	struct stat st;
	for (int i = 0; i < 500 && stat(BUS, &st) < 0; i++)
		usleep(10000);

	DBusError err;
	dbus_error_init(&err);
	dbus_threads_init_default();
	conn = dbus_connection_open_private("unix:path=" BUS, &err);
	if (!conn || !dbus_bus_register(conn, &err) ||
	    dbus_bus_request_name(conn, BLUEZ, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) !=
	    DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "fake_bluez: no bus: %s\n", err.message ? err.message : "?");
		exit(EXIT_FAILURE);
	}
	dbus_bus_request_name(conn, "org.bluealsa", DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	static const DBusObjectPathVTable vtable = { .message_function = handle };
	dbus_connection_register_fallback(conn, "/", &vtable, NULL);
	pthread_create(&thread, NULL, serve, NULL);
}

/* An Agent1 call without arguments to phone-ui's agent (Cancel, Release), its answer not
 * awaited */
static void agent_tell(const char *method)
{
	if (!agent_owner[0])
		return;
	DBusMessage *m = dbus_message_new_method_call(agent_owner, agent_path, "org.bluez.Agent1", method);
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(conn, m, NULL);
	dbus_message_unref(m);
}

void ui_test_bt_exit(void)
{
	if (!started || !owned)
		return;
	pthread_mutex_lock(&lock);
	agent_tell("Release");
	agent_owner[0] = agent_path[0] = agent_capability[0] = '\0';
	discovering = discoverable = false;
	pthread_mutex_unlock(&lock);
	dbus_connection_flush(conn);
	dbus_bus_release_name(conn, BLUEZ, NULL);
	owned = false;
}

void ui_test_bt_bus_lost(void)
{
	if (!started)
		return;
	kill(daemon_pid, SIGKILL);
	waitpid(daemon_pid, NULL, 0);
	pthread_join(thread, NULL); /* its connection closed */
	dbus_connection_close(conn);
	dbus_connection_unref(conn);
	conn = NULL;
	pthread_mutex_lock(&lock);
	agent_owner[0] = agent_path[0] = agent_capability[0] = '\0';
	discovering = discoverable = false;
	for (unsigned i = 0; i < device_count; i++)
		if (devices[i].pairing) {
			dbus_message_unref(devices[i].pairing);
			devices[i].pairing = NULL;
		}
	pthread_mutex_unlock(&lock);
	started = owned = false;
}

void ui_test_bt_running(bool on)
{
	start();
	pthread_mutex_lock(&lock);
	powered = on;
	pthread_mutex_unlock(&lock);
}

static struct device *device_add(const char *address, const char *name)
{
	struct device *d = device_find(address);
	if (!d && device_count < MAX)
		d = &devices[device_count++];
	if (!d)
		return NULL;
	snprintf(d->address, sizeof(d->address), "%s", address);
	snprintf(d->name, sizeof(d->name), "%s", name ? name : "");
	return d;
}

void ui_test_bt_device(const char *address, const char *name, int rssi, unsigned passkey)
{
	start();
	pthread_mutex_lock(&lock);
	struct device *d = device_add(address, name);
	if (d) {
		d->rssi = rssi;
		d->passkey = passkey;
		d->known = d->known || discovering;
	}
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_paired(const char *address, const char *name, bool connected)
{
	start();
	pthread_mutex_lock(&lock);
	struct device *d = device_add(address, name);
	if (d) {
		d->paired = d->trusted = d->known = true;
		d->connected = connected;
		d->rssi = -60;
	}
	pthread_mutex_unlock(&lock);
}

#define GETTER(name, expr) \
	bool name(void) \
	{ \
		pthread_mutex_lock(&lock); \
		bool v = (expr); \
		pthread_mutex_unlock(&lock); \
		return v; \
	}
GETTER(ui_test_bt_powered, powered)
GETTER(ui_test_bt_discovering, discovering)
GETTER(ui_test_bt_discoverable, discoverable)

bool ui_test_bt_is_paired(const char *address)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	bool v = d && d->paired;
	pthread_mutex_unlock(&lock);
	return v;
}

bool ui_test_bt_is_connected(const char *address)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	bool v = d && d->connected;
	pthread_mutex_unlock(&lock);
	return v;
}

const char *ui_test_bt_agent(void)
{
	static char capability[32];
	pthread_mutex_lock(&lock);
	snprintf(capability, sizeof(capability), "%s", agent_capability);
	pthread_mutex_unlock(&lock);
	return capability;
}

void ui_test_bt_pairing(const char *address, const char *method, const char *code)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d) {
		snprintf(d->asks, sizeof(d->asks), "%s", method);
		snprintf(d->code, sizeof(d->code), "%s", code ? code : "");
		if (code && strcmp(method, "DisplayPinCode") != 0)
			d->passkey = (unsigned)atoi(code);
	}
	pthread_mutex_unlock(&lock);
}

/* Pair's answer for a device that is pairing */
static void pair_answer(struct device *d, bool ok, const char *error_name)
{
	if (!d || !d->pairing)
		return;
	if (ok)
		d->paired = d->trusted = true;
	DBusMessage *answer = ok ? dbus_message_new_method_return(d->pairing) : error(d->pairing, error_name);
	dbus_connection_send(conn, answer, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(answer);
	dbus_message_unref(d->pairing);
	d->pairing = NULL;
}

void ui_test_bt_typed(const char *address)
{
	pthread_mutex_lock(&lock);
	pair_answer(device_find(address), true, NULL);
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_cancel(const char *address)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d && d->pairing) {
		agent_tell("Cancel");
		pair_answer(d, false, "org.bluez.Error.AuthenticationCanceled");
	}
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_no_profiles(const char *address)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d)
		d->no_profiles = true;
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_audio(const char *address, unsigned rate)
{
	start();
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d) {
		d->audio = rate;
		d->volume = 127;
	}
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_headset(const char *address)
{
	start();
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d)
		d->hfp = true;
	pthread_mutex_unlock(&lock);
}

void ui_test_bt_connected(const char *address, bool connected)
{
	start();
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d && d->paired)
		d->connected = connected;
	pthread_mutex_unlock(&lock);
}

int ui_test_bt_volume(const char *address)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	int v = !d ? -2 : d->muted ? -1 : (int)d->volume;
	pthread_mutex_unlock(&lock);
	return v;
}

void ui_test_bt_set_volume(const char *address, int volume)
{
	pthread_mutex_lock(&lock);
	struct device *d = device_find(address);
	if (d) {
		d->volume = (unsigned)volume;
		d->muted = false;
	}
	pthread_mutex_unlock(&lock);
}
