/*
 * Pretend sockets (sockets.c): what phone-ui talks to over sockets on the phone, answered
 * in the page. Their descriptors are /dev/null's, so that they are real ones.
 */

#ifndef WASM_SOCKETS_H
#define WASM_SOCKETS_H

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>

enum fake_kind {
	FAKE_WPA,    /* a datagram socket to wpa_supplicant's control interface (wifi.c) */
	FAKE_IFACE,  /* an AF_INET one for the interfaces' flags (wifi.c) */
	FAKE_PLAYER, /* music.c's end of its socket pair to `mpg123 --remote` (player.c) */
};

#define FAKE_QUEUE 16

struct fake_socket {
	int fd; /* 0: free */
	enum fake_kind kind;
	bool connected, attached; /* FAKE_WPA: to wlan0's interface, and for its events */
	char *queue[FAKE_QUEUE];  /* what phone-ui reads next: datagrams, or a stream's pieces */
	unsigned count;
	char line[512];           /* FAKE_PLAYER: a command not ended yet */
	size_t line_len;
};

/* Something for phone-ui to read on a socket, and on every one of a kind (attached ones
 * for FAKE_WPA) */
void fake_push(struct fake_socket *s, const char *text);
void fake_push_all(enum fake_kind kind, const char *text);

/* wifi.c: whether a control interface is there, a command's reply, wlan0's flags
 * (SIOCGIFFLAGS, SIOCSIFFLAGS) */
bool wifi_connect(const char *path);
void wifi_command(struct fake_socket *s, const char *cmd, char *reply, size_t size);
bool wifi_ioctl(int request, struct ifreq *ifr); /* false: not wlan0 */

/* player.c: a command line of mpg123's remote interface, without its "\n" */
void player_command(struct fake_socket *s, const char *line);

#endif
