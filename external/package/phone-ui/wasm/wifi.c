/*
 * Wi-Fi in the browser: ../src/settings/wifi.c talks to wpa_supplicant over a Unix
 * datagram socket, which a browser has none of. The socket calls are wrapped
 * (-Wl,--wrap, the Makefile's): a socket connected to wlan0's control interface is
 * answered here at once, as wpa_supplicant 2.12 would (../tests/fake_wpa.c, the tests'
 * own, answers from a thread). Its descriptor is /dev/null's, so that it is a real one;
 * every other goes to Emscripten's calls. wlan0's flags (SIOCGIFFLAGS, SIOCSIFFLAGS)
 * are here too: down, it disconnects; and the interfaces' addresses (getifaddrs()).
 *
 * Four networks around, "Home" saved and connected; any password of 8 characters or
 * more connects. Nothing is kept.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "modem.h"

#define FDS 8
#define QUEUE 8
#define NETS 8
#define REPLY_SIZE 4096

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

/* The sockets: control interface clients, attached ones get events; or AF_INET ones
 * for wlan0's flags */
static struct client {
	int fd; /* 0: free */
	bool wpa, connected, attached;
	char *queue[QUEUE];
	unsigned count;
} clients[FDS];

static struct client *client(int fd)
{
	for (unsigned i = 0; i < FDS; i++)
		if (fd > 0 && clients[i].fd == fd)
			return &clients[i];
	return NULL;
}

static void push(struct client *c, const char *msg)
{
	if (c->count < QUEUE && (c->queue[c->count] = strdup(msg)))
		c->count++;
}

static void event(const char *msg)
{
	for (unsigned i = 0; i < FDS; i++)
		if (clients[i].fd && clients[i].attached)
			push(&clients[i], msg);
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

static void handle(struct client *c, const char *cmd, char *reply, size_t size)
{
	int id;
	snprintf(reply, size, "OK\n");
	if (strcmp(cmd, "PING") == 0) {
		snprintf(reply, size, "PONG\n");
	} else if (strcmp(cmd, "ATTACH") == 0) {
		c->attached = true;
	} else if (strcmp(cmd, "DETACH") == 0) {
		c->attached = false;
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

/* The wrapped calls */

int __real_socket(int domain, int type, int protocol);
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
ssize_t __real_recv(int fd, void *buf, size_t len, int flags);
int __real_poll(struct pollfd *fds, nfds_t n, int timeout);
int __real_close(int fd);
int __real_ioctl(int fd, int request, ...);

int __wrap_socket(int domain, int type, int protocol)
{
	bool wpa = domain == AF_UNIX && (type & 0xf) == SOCK_DGRAM;
	if (!wpa && !(domain == AF_INET && (type & 0xf) == SOCK_DGRAM))
		return __real_socket(domain, type, protocol);
	for (unsigned i = 0; i < FDS; i++) {
		if (clients[i].fd)
			continue;
		int fd = open("/dev/null", O_RDWR);
		if (fd < 0)
			return -1;
		clients[i] = (struct client){ .fd = fd, .wpa = wpa };
		return fd;
	}
	errno = EMFILE;
	return -1;
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	return client(fd) ? 0 : __real_bind(fd, addr, len);
}

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	struct client *c = client(fd);
	if (!c)
		return __real_connect(fd, addr, len);
	const char *path = ((const struct sockaddr_un *)addr)->sun_path;
	const char *slash = strrchr(path, '/');
	if (!c->wpa || !slash || strcmp(slash, "/wlan0") != 0) {
		errno = ENOENT;
		return -1;
	}
	c->connected = true;
	return 0;
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
	struct client *c = client(fd);
	if (!c)
		return __real_send(fd, buf, len, flags);
	if (!c->connected) {
		errno = ENOTCONN;
		return -1;
	}
	char cmd[512], reply[REPLY_SIZE];
	snprintf(cmd, sizeof(cmd), "%.*s", (int)len, (const char *)buf);
	handle(c, cmd, reply, sizeof(reply));
	push(c, reply);
	return (ssize_t)len;
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
	struct client *c = client(fd);
	if (!c)
		return __real_recv(fd, buf, len, flags);
	if (!c->count) {
		errno = EAGAIN;
		return -1;
	}
	char *msg = c->queue[0];
	size_t n = strlen(msg) < len ? strlen(msg) : len;
	memcpy(buf, msg, n);
	free(msg);
	memmove(c->queue, c->queue + 1, --c->count * sizeof(c->queue[0]));
	return (ssize_t)n;
}

/* One of ours alone: ready when something is queued (a reply always is) */
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	struct client *c = n == 1 ? client(fds[0].fd) : NULL;
	if (!c)
		return __real_poll(fds, n, timeout);
	fds[0].revents = c->count ? POLLIN : 0;
	return c->count ? 1 : 0;
}

int __wrap_close(int fd)
{
	struct client *c = client(fd);
	if (c) {
		while (c->count)
			free(c->queue[--c->count]);
		c->fd = 0;
	}
	return __real_close(fd);
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

int __wrap_ioctl(int fd, int request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	struct ifreq *ifr = arg;
	if (client(fd) && (request == SIOCGIFFLAGS || request == SIOCSIFFLAGS) && strcmp(ifr->ifr_name, "wlan0") == 0) {
		if (request == SIOCGIFFLAGS)
			ifr->ifr_flags = (short)(IFF_BROADCAST | IFF_MULTICAST | (link_down ? 0 : IFF_UP));
		else
			link_set(ifr->ifr_flags & IFF_UP);
		return 0;
	}
	return __real_ioctl(fd, request, arg);
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
