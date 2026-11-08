/*
 * Wi-Fi in the browser: ../src/apps/settings/wifi.c talks to wpa_supplicant over a Unix
 * datagram socket, which a browser has none of. Its socket (sockets.c's) is answered here
 * at once, as wpa_supplicant 2.12 would (../tests/fake_wpa.c, the tests' own, answers from
 * a thread). wlan0's flags (SIOCGIFFLAGS, SIOCSIFFLAGS) are here too: down, it
 * disconnects; and the interfaces' addresses (getifaddrs(), wrapped).
 *
 * Four networks around, "Home" saved and connected; any password of 8 characters or
 * more connects. Nothing is kept.
 */

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "modem/modem.h"
#include "sockets.h"

#define NETS 8

static const struct ap {
	const char *ssid;
	int freq, signal;
	const char *flags;
} aps[] = {
	{ "Home", 5180, -52, "[WPA2-PSK-CCMP][ESS]" },
	{ "Coffee Shop", 2437, -64, "[ESS]" },
	{ "Neighbours", 2462, -78, "[WPA2-PSK-CCMP][ESS]" },
	{ "Office", 5500, -71, "[WPA2-PSK+SAE-CCMP][ESS]" },
};
#define AP_COUNT (sizeof(aps) / sizeof(aps[0]))

static struct network {
	bool used;
	char ssid[33];
	bool open;
} nets[NETS] = { { .used = true, .ssid = "Home" } };
static int current = 0;
static bool link_down;

static void event(const char *msg)
{
	fake_push_all(FAKE_WPA, msg);
}

static const struct ap *ap_find(const char *ssid)
{
	for (unsigned i = 0; i < AP_COUNT; i++)
		if (strcmp(aps[i].ssid, ssid) == 0)
			return &aps[i];
	return NULL;
}

static void connect_to(int id)
{
	const struct ap *ap = ap_find(nets[id].ssid);
	if (!ap || link_down)
		return;
	current = id;
	event("<3>CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:01 completed [id=0 id_str=]");
}

static void disconnect(void)
{
	if (current < 0)
		return;
	current = -1;
	event("<3>CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:00:01 reason=3 locally_generated=1");
}

static int network_id(const char *arg)
{
	int id = atoi(arg);
	return arg[0] >= '0' && arg[0] <= '9' && id < NETS && nets[id].used ? id : -1;
}

/* "Home" from SET_NETWORK's ssid, hex as wifi.c sends it, or quoted */
static void ssid_parse(char *out, size_t size, const char *v)
{
	size_t n = 0;
	if (v[0] == '"') {
		for (v++; *v && *v != '"' && n + 1 < size; v++)
			out[n++] = *v;
	} else {
		for (unsigned b; sscanf(v, "%2x", &b) == 1 && n + 1 < size; v += 2)
			out[n++] = (char)b;
	}
	out[n] = '\0';
}

void wifi_command(struct fake_socket *s, const char *cmd, char *reply, size_t size)
{
	int id;
	snprintf(reply, size, "OK\n");
	if (strcmp(cmd, "PING") == 0) {
		snprintf(reply, size, "PONG\n");
	} else if (strcmp(cmd, "ATTACH") == 0) {
		s->attached = true;
	} else if (strcmp(cmd, "DETACH") == 0) {
		s->attached = false;
	} else if (strcmp(cmd, "SCAN") == 0) {
		if (link_down)
			snprintf(reply, size, "FAIL\n");
	} else if (strcmp(cmd, "SCAN_RESULTS") == 0) {
		size_t n = (size_t)snprintf(reply, size, "bssid / frequency / signal level / flags / ssid\n");
		for (unsigned i = 0; i < AP_COUNT && !link_down && n < size; i++)
			n += (size_t)snprintf(reply + n, size - n, "02:00:00:00:00:%02x\t%d\t%d\t%s\t%s\n", i + 1,
			                      aps[i].freq, aps[i].signal, aps[i].flags, aps[i].ssid);
	} else if (strcmp(cmd, "LIST_NETWORKS") == 0) {
		size_t n = (size_t)snprintf(reply, size, "network id / ssid / bssid / flags\n");
		for (int i = 0; i < NETS && n < size; i++)
			if (nets[i].used)
				n += (size_t)snprintf(reply + n, size - n, "%d\t%s\tany\t%s\n", i, nets[i].ssid,
				                      i == current ? "[CURRENT]" : "");
	} else if (strcmp(cmd, "STATUS") == 0) {
		const struct ap *ap = current >= 0 ? ap_find(nets[current].ssid) : NULL;
		if (ap)
			snprintf(reply, size,
			         "bssid=02:00:00:00:00:01\nfreq=%d\nssid=%s\nid=%d\nmode=station\n"
			         "pairwise_cipher=CCMP\nkey_mgmt=%s\nwpa_state=COMPLETED\n"
			         "ip_address=192.168.1.42\naddress=02:ba:7e:df:08:63\n",
			         ap->freq, ap->ssid, current,
			         nets[current].open ? "NONE" : strstr(ap->flags, "SAE") ? "SAE" : "WPA2-PSK");
		else
			snprintf(reply, size, "wpa_state=%s\naddress=02:ba:7e:df:08:63\n",
			         link_down ? "INTERFACE_DISABLED" : "DISCONNECTED");
	} else if (strcmp(cmd, "SIGNAL_POLL") == 0) {
		const struct ap *ap = current >= 0 ? ap_find(nets[current].ssid) : NULL;
		if (ap)
			snprintf(reply, size, "RSSI=%d\nLINKSPEED=%d\nNOISE=9999\nFREQUENCY=%d\n", ap->signal,
			         ap->freq > 5000 ? 433 : 72, ap->freq);
		else
			snprintf(reply, size, "FAIL\n");
	} else if (strcmp(cmd, "ADD_NETWORK") == 0) {
		for (id = 0; id < NETS && nets[id].used; id++)
			;
		if (id == NETS) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		nets[id] = (struct network){ .used = true };
		snprintf(reply, size, "%d\n", id);
	} else if (strncmp(cmd, "SET_NETWORK ", 12) == 0) {
		char name[32];
		int skip = 0;
		if ((id = network_id(cmd + 12)) < 0 || sscanf(cmd + 12, "%*d %31s %n", name, &skip) != 1 || !skip) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		const char *value = cmd + 12 + skip;
		if (strcmp(name, "ssid") == 0)
			ssid_parse(nets[id].ssid, sizeof(nets[id].ssid), value);
		else if (strcmp(name, "key_mgmt") == 0)
			nets[id].open = strcmp(value, "NONE") == 0;
	} else if (strncmp(cmd, "SELECT_NETWORK ", 15) == 0 || strncmp(cmd, "ENABLE_NETWORK ", 15) == 0) {
		if ((id = network_id(cmd + 15)) < 0) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		if (cmd[1] == 'E' || current < 0) /* SELECT: this one, ENABLE: if none */
			connect_to(id);
	} else if (strncmp(cmd, "REMOVE_NETWORK ", 15) == 0) {
		if ((id = network_id(cmd + 15)) < 0) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		nets[id].used = false;
		if (current == id)
			disconnect();
	} else if (strcmp(cmd, "SAVE_CONFIG") == 0) {
		/* nothing kept */
	} else {
		snprintf(reply, size, "UNKNOWN COMMAND\n");
	}
}

/* Its control interface: wlan0's only */
bool wifi_connect(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash && strcmp(slash, "/wlan0") == 0;
}

/* wlan0's flags: down, disconnected and waiting (INTERFACE_DISABLED); up, it connects
 * again to the first saved network in range */
static void link_set(bool up)
{
	if (up == !link_down)
		return;
	link_down = !up;
	if (!up)
		disconnect();
	for (int id = 0; up && current < 0 && id < NETS; id++)
		if (nets[id].used)
			connect_to(id);
}

bool wifi_ioctl(int request, struct ifreq *ifr)
{
	if (strcmp(ifr->ifr_name, "wlan0") != 0)
		return false;
	if (request == SIOCGIFFLAGS)
		ifr->ifr_flags = (short)(IFF_BROADCAST | IFF_MULTICAST | (link_down ? 0 : IFF_UP));
	else
		link_set(ifr->ifr_flags & IFF_UP);
	return true;
}

/* The interfaces with an address (Settings, Network): wlan0 while connected, wwan0 while
 * mobile data is (modem.c's) */
int __wrap_getifaddrs(struct ifaddrs **list)
{
	static const struct {
		const char *name, *addr;
	} all[] = { { "wlan0", "192.168.1.42" }, { "wwan0", "10.64.12.7" } };
	struct entry {
		struct ifaddrs ifa;
		struct sockaddr_in addr;
		char name[IFNAMSIZ];
	};
	*list = NULL;
	for (int i = (int)(sizeof(all) / sizeof(all[0])) - 1; i >= 0; i--) {
		if (i == 0 ? current < 0 || link_down : modem_data_state() != MODEM_DATA_CONNECTED)
			continue;
		struct entry *e = calloc(1, sizeof(*e));
		if (!e)
			break;
		snprintf(e->name, sizeof(e->name), "%s", all[i].name);
		e->addr.sin_family = AF_INET;
		inet_pton(AF_INET, all[i].addr, &e->addr.sin_addr);
		e->ifa = (struct ifaddrs){ .ifa_next = *list, .ifa_name = e->name, .ifa_flags = IFF_UP | IFF_RUNNING,
		                           .ifa_addr = (struct sockaddr *)&e->addr };
		*list = &e->ifa;
	}
	return 0;
}

void __wrap_freeifaddrs(struct ifaddrs *list)
{
	while (list) {
		struct ifaddrs *next = list->ifa_next;
		free(list); /* the entry, ifa first */
		list = next;
	}
}
