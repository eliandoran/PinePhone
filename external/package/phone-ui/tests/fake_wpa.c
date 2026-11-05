/*
 * wpa_supplicant for the UI tests (ui_test.h, "Wi-Fi"): its control interface on
 * wlan0, answered from a thread (phone-ui waits for the replies), with the access
 * points and saved networks the test sets. It answers the commands Settings' Wi-Fi
 * page sends as wpa_supplicant 2.12 does, connects at once (address included) when
 * the password matches and refuses with WRONG_KEY otherwise; SAVE_CONFIG writes
 * /var/lib/wpa_supplicant/wpa_supplicant.conf, for the tests to read. Started by the
 * first ui_test_wifi_*() call: without it, there is no socket (not running).
 */

#include <ctype.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ui_test.h"

#define DIR "/tmp/wpa_supplicant" /* WPA_CTRL_DIR of wifi.o, the Makefile's */
#define CONF "/var/lib/wpa_supplicant/wpa_supplicant.conf"
#define MAX 16

struct ap {
	char ssid[33];
	int signal;
	int freq;          /* MHz */
	char flags[96];
	char password[64]; /* "" open */
};

struct network {
	bool used;
	char ssid[33];     /* decoded */
	char vars[8][2][80]; /* name, value as set */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;
static int sock = -1;
static struct ap aps[MAX];
static unsigned ap_count;
static struct network nets[MAX];
static int current = -1;
static bool link_down; /* wlan0 taken down (Wi-Fi off) */
static struct sockaddr_un monitors[4];
static unsigned monitor_count;

static const char *var(const struct network *n, const char *name)
{
	for (unsigned i = 0; i < 8; i++)
		if (strcmp(n->vars[i][0], name) == 0)
			return n->vars[i][1];
	return NULL;
}

static void var_set(struct network *n, const char *name, const char *value)
{
	unsigned i;
	for (i = 0; i < 8 && n->vars[i][0][0] && strcmp(n->vars[i][0], name) != 0; i++)
		;
	if (i == 8)
		return;
	snprintf(n->vars[i][0], sizeof(n->vars[i][0]), "%s", name);
	snprintf(n->vars[i][1], sizeof(n->vars[i][1]), "%s", value);
}

/* An SSID as wpa_supplicant writes it in its replies (printf_encode()) */
static void encode(char *out, size_t size, const char *s)
{
	size_t n = 0;
	for (; *s && n + 5 < size; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			n += (size_t)snprintf(out + n, size - n, "\\%c", c);
		else if (c < 32 || c >= 127)
			n += (size_t)snprintf(out + n, size - n, "\\x%02x", c);
		else
			out[n++] = (char)c;
	}
	out[n] = '\0';
}

/* A value of SET_NETWORK ssid/psk: "quoted" or hex */
static void unquote(char *out, size_t size, const char *v)
{
	size_t n = 0;
	if (v[0] == '"') {
		snprintf(out, size, "%.*s", (int)(strlen(v) >= 2 ? strlen(v) - 2 : 0), v + 1);
		return;
	}
	for (; isxdigit((unsigned char)v[0]) && isxdigit((unsigned char)v[1]) && n + 1 < size; v += 2) {
		char byte[3] = { v[0], v[1], 0 };
		out[n++] = (char)strtol(byte, NULL, 16);
	}
	out[n] = '\0';
}

static void event(const char *text)
{
	for (unsigned i = 0; i < monitor_count; i++)
		sendto(sock, text, strlen(text), 0, (struct sockaddr *)&monitors[i], sizeof(monitors[i]));
}

static const struct ap *ap_find(const char *ssid)
{
	for (unsigned i = 0; i < ap_count; i++)
		if (strcmp(aps[i].ssid, ssid) == 0)
			return &aps[i];
	return NULL;
}

/* Connects to network `id` if in range: with its password, or refused */
static void connect_to(int id)
{
	struct network *n = &nets[id];
	const struct ap *ap = ap_find(n->ssid);
	if (!ap || link_down)
		return;
	const char *psk = var(n, "psk"), *key_mgmt = var(n, "key_mgmt");
	char password[64] = "";
	if (psk)
		unquote(password, sizeof(password), psk);
	bool open = key_mgmt && strcmp(key_mgmt, "NONE") == 0;
	if (open ? ap->password[0] == '\0' : strcmp(password, ap->password) == 0) {
		current = id;
		event("<3>CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:01 completed [id=0 id_str=]");
	} else {
		char text[256], ssid[140];
		encode(ssid, sizeof(ssid), n->ssid);
		snprintf(text, sizeof(text),
		         "<3>CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 reason=WRONG_KEY",
		         id, ssid);
		if (current == id)
			current = -1;
		event(text);
	}
}

static void save_config(void)
{
	mkdir("/var/lib/wpa_supplicant", 0755);
	FILE *f = fopen(CONF, "w");
	if (!f)
		return;
	fputs("ctrl_interface=/run/wpa_supplicant\nupdate_config=1\n", f);
	for (int id = 0; id < MAX; id++) {
		if (!nets[id].used)
			continue;
		fputs("\nnetwork={\n", f);
		for (unsigned i = 0; i < 8 && nets[id].vars[i][0][0]; i++)
			fprintf(f, "\t%s=%s\n", nets[id].vars[i][0], nets[id].vars[i][1]);
		fputs("}\n", f);
	}
	fclose(f);
}

static int network_id(const char *arg)
{
	int id = atoi(arg);
	return isdigit((unsigned char)arg[0]) && id < MAX && nets[id].used ? id : -1;
}

/* The reply to a command */
static void handle(const char *cmd, const struct sockaddr_un *from, char *reply, size_t size)
{
	int id;
	snprintf(reply, size, "OK\n");
	if (strcmp(cmd, "PING") == 0) {
		snprintf(reply, size, "PONG\n");
	} else if (strcmp(cmd, "ATTACH") == 0) {
		if (monitor_count < 4)
			monitors[monitor_count++] = *from;
	} else if (strcmp(cmd, "DETACH") == 0) {
		for (unsigned i = 0; i < monitor_count; i++)
			if (strcmp(monitors[i].sun_path, from->sun_path) == 0)
				monitors[i--] = monitors[--monitor_count];
	} else if (strcmp(cmd, "SCAN") == 0 && link_down) {
		snprintf(reply, size, "FAIL\n");
	} else if (strcmp(cmd, "SCAN") == 0) {
		event("<2>CTRL-EVENT-SCAN-STARTED ");
		event("<2>CTRL-EVENT-SCAN-RESULTS ");
	} else if (strcmp(cmd, "SCAN_RESULTS") == 0) {
		size_t n = (size_t)snprintf(reply, size, "bssid / frequency / signal level / flags / ssid\n");
		for (unsigned i = 0; i < ap_count && !link_down && n < size; i++) {
			char ssid[140];
			encode(ssid, sizeof(ssid), aps[i].ssid);
			n += (size_t)snprintf(reply + n, size - n, "02:00:00:00:00:%02x\t%d\t%d\t%s\t%s\n", i + 1, aps[i].freq,
			                      aps[i].signal, aps[i].flags, ssid);
		}
	} else if (strcmp(cmd, "LIST_NETWORKS") == 0) {
		size_t n = (size_t)snprintf(reply, size, "network id / ssid / bssid / flags\n");
		for (int i = 0; i < MAX && n < size; i++) {
			char ssid[140];
			if (!nets[i].used)
				continue;
			encode(ssid, sizeof(ssid), nets[i].ssid);
			n += (size_t)snprintf(reply + n, size - n, "%d\t%s\tany\t%s\n", i, ssid,
			                      i == current ? "[CURRENT]" : "");
		}
	} else if (strcmp(cmd, "STATUS") == 0) {
		if (current >= 0) {
			char ssid[140];
			const struct ap *ap = ap_find(nets[current].ssid);
			const char *km = var(&nets[current], "key_mgmt");
			/* What it negotiated: SAE when both offer it */
			const char *key_mgmt = !km ? "WPA2-PSK" : strcmp(km, "NONE") == 0 ? "NONE" :
			                       strstr(km, "SAE") && ap && strstr(ap->flags, "SAE") ? "SAE" : "WPA2-PSK";
			encode(ssid, sizeof(ssid), nets[current].ssid);
			snprintf(reply, size,
			         "bssid=02:00:00:00:00:01\nfreq=%d\nssid=%s\nid=%d\nmode=station\n"
			         "pairwise_cipher=CCMP\nkey_mgmt=%s\nwpa_state=COMPLETED\n"
			         "ip_address=192.168.1.42\naddress=02:ba:7e:df:08:63\n",
			         ap ? ap->freq : 2437, ssid, current, key_mgmt);
		} else {
			snprintf(reply, size, "wpa_state=%s\naddress=02:ba:7e:df:08:63\n",
			         link_down ? "INTERFACE_DISABLED" : ap_count ? "SCANNING" : "DISCONNECTED");
		}
	} else if (strcmp(cmd, "SIGNAL_POLL") == 0) {
		const struct ap *ap = current >= 0 ? ap_find(nets[current].ssid) : NULL;
		if (ap)
			snprintf(reply, size, "RSSI=%d\nLINKSPEED=72\nNOISE=9999\nFREQUENCY=%d\n", ap->signal, ap->freq);
		else
			snprintf(reply, size, "FAIL\n");
	} else if (strcmp(cmd, "ADD_NETWORK") == 0) {
		for (id = 0; id < MAX && nets[id].used; id++)
			;
		if (id == MAX) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		memset(&nets[id], 0, sizeof(nets[id]));
		nets[id].used = true;
		snprintf(reply, size, "%d\n", id);
	} else if (strncmp(cmd, "SET_NETWORK ", 12) == 0) {
		char name[32];
		int skip = 0;
		id = network_id(cmd + 12);
		if (id < 0 || sscanf(cmd + 12, "%*d %31s %n", name, &skip) != 1 || !skip) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		var_set(&nets[id], name, cmd + 12 + skip);
		if (strcmp(name, "ssid") == 0)
			unquote(nets[id].ssid, sizeof(nets[id].ssid), cmd + 12 + skip);
	} else if (strncmp(cmd, "SELECT_NETWORK ", 15) == 0 || strncmp(cmd, "ENABLE_NETWORK ", 15) == 0) {
		if ((id = network_id(cmd + 15)) < 0) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		if (cmd[0] == 'S' || current < 0)
			connect_to(id);
	} else if (strncmp(cmd, "REMOVE_NETWORK ", 15) == 0) {
		if ((id = network_id(cmd + 15)) < 0) {
			snprintf(reply, size, "FAIL\n");
			return;
		}
		nets[id].used = false;
		if (current == id) {
			current = -1;
			event("<3>CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:00:01 reason=3 locally_generated=1");
		}
	} else if (strcmp(cmd, "SAVE_CONFIG") == 0) {
		save_config();
	} else {
		snprintf(reply, size, "UNKNOWN COMMAND\n");
	}
}

static void *serve(void *arg)
{
	(void)arg;
	for (;;) {
		char cmd[512], reply[4096];
		struct sockaddr_un from;
		socklen_t len = sizeof(from);
		ssize_t n = recvfrom(sock, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *)&from, &len);
		if (n < 0)
			continue;
		cmd[n] = '\0';
		pthread_mutex_lock(&lock);
		handle(cmd, &from, reply, sizeof(reply));
		pthread_mutex_unlock(&lock);
		sendto(sock, reply, strlen(reply), 0, (struct sockaddr *)&from, len);
	}
	return NULL;
}

static void start(void)
{
	if (started)
		return;
	started = true;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	mkdir(DIR, 0755);
	snprintf(addr.sun_path, sizeof(addr.sun_path), DIR "/wlan0");
	unlink(addr.sun_path);
	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	pthread_t thread;
	if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    pthread_create(&thread, NULL, serve, NULL) != 0) {
		perror("fake_wpa");
		exit(EXIT_FAILURE);
	}
	pthread_detach(thread);
}

void ui_test_wifi_ap(const char *ssid, int signal, const char *flags, const char *password)
{
	pthread_mutex_lock(&lock);
	start();
	if (ap_count < MAX) {
		struct ap *ap = &aps[ap_count++];
		snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
		ap->signal = signal;
		ap->freq = 2437;
		snprintf(ap->flags, sizeof(ap->flags), "%s", flags);
		snprintf(ap->password, sizeof(ap->password), "%s", password ? password : "");
	}
	pthread_mutex_unlock(&lock);
}

void ui_test_wifi_saved(const char *ssid, const char *password)
{
	pthread_mutex_lock(&lock);
	start();
	int id;
	for (id = 0; id < MAX && nets[id].used; id++)
		;
	if (id < MAX) {
		char value[80];
		nets[id].used = true;
		snprintf(nets[id].ssid, sizeof(nets[id].ssid), "%s", ssid);
		snprintf(value, sizeof(value), "\"%s\"", ssid);
		var_set(&nets[id], "ssid", value);
		if (password) {
			snprintf(value, sizeof(value), "\"%s\"", password);
			var_set(&nets[id], "psk", value);
		} else {
			var_set(&nets[id], "key_mgmt", "NONE");
		}
		save_config();
		/* As at startup: the first saved network in range */
		if (current < 0)
			connect_to(id);
	}
	pthread_mutex_unlock(&lock);
}

void ui_test_wifi_running(void)
{
	pthread_mutex_lock(&lock);
	start();
	pthread_mutex_unlock(&lock);
}

void ui_test_wifi_frequency(const char *ssid, int mhz)
{
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < ap_count; i++)
		if (strcmp(aps[i].ssid, ssid) == 0)
			aps[i].freq = mhz;
	pthread_mutex_unlock(&lock);
}

/* wlan0 up or down, as SIOCSIFFLAGS: down, wpa_supplicant disconnects and waits
 * (INTERFACE_DISABLED); up, it connects again to a saved network in range */
static void link_set(bool up)
{
	if (up == !link_down)
		return;
	link_down = !up;
	if (!up && current >= 0) {
		current = -1;
		event("<3>CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:00:01 reason=3 locally_generated=1");
	}
	for (int id = 0; up && current < 0 && id < MAX; id++)
		if (nets[id].used)
			connect_to(id);
}

/* phone-ui's ioctl() (UI tests link with --wrap=ioctl): wlan0's flags are this fake's,
 * the cameras' nodes fake_camera.c's, the rest goes to the real one */
int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	struct ifreq *ifr = arg;
	if ((request == SIOCGIFFLAGS || request == SIOCSIFFLAGS) && strcmp(ifr->ifr_name, "wlan0") == 0) {
		pthread_mutex_lock(&lock);
		if (request == SIOCGIFFLAGS)
			ifr->ifr_flags = (short)(IFF_BROADCAST | IFF_MULTICAST | (link_down ? 0 : IFF_UP));
		else
			link_set(ifr->ifr_flags & IFF_UP);
		pthread_mutex_unlock(&lock);
		return 0;
	}
	int ret;
	if (fake_camera_ioctl(fd, request, arg, &ret))
		return ret;
	return __real_ioctl(fd, request, arg);
}

bool ui_test_wifi_up(void)
{
	pthread_mutex_lock(&lock);
	bool up = !link_down;
	pthread_mutex_unlock(&lock);
	return up;
}
