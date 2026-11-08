/*
 * Internet over the USB cable: the NCM link is up (usb0's carrier) and the host
 * forwards our traffic (`just share-internet`). That is probed with a TCP connect
 * to the DNS server, non-blocking: started every PROBE_EVERY s, its result read by
 * the following updates (the status bar's, each second), given up after PROBE_TIMEOUT s.
 */

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ui/ui_internal.h"

#define USB_NET "/sys/class/net/usb0"
#define PROBE_EVERY 10
#define PROBE_TIMEOUT 5

static int usb_probe_fd = -1;
static time_t usb_probe_time;
static bool usb_online;
static int usb_state; /* usb_net_state() at the last update */
static bool usb_lit;  /* the computer had its address then */

int ui_usb_net(void)
{
	return usb_state;
}

bool usb_net_lit(void)
{
	return usb_lit;
}

static void usb_probe_end(bool online)
{
	close(usb_probe_fd);
	usb_probe_fd = -1;
	usb_online = online;
}

static void usb_probe_start(time_t now)
{
	usb_probe_time = now;
	usb_probe_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (usb_probe_fd < 0)
		return;
	/* Through the host only: the default route may be a Wi-Fi network's */
	setsockopt(usb_probe_fd, SOL_SOCKET, SO_BINDTODEVICE, "usb0", sizeof("usb0"));
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(53),
		.sin_addr.s_addr = htonl(0x09090909), /* 9.9.9.9, the first nameserver (S05usbgadget) */
	};
	if (connect(usb_probe_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		usb_probe_end(true);
	else if (errno != EINPROGRESS)
		usb_probe_end(false);
}

#ifndef PROC_NET_ARP
#define PROC_NET_ARP "/proc/net/arp"
#endif

/* The computer has its address on the USB network: a neighbour resolved on usb0 (the probe
 * above, through it as the default route, asks for it every PROBE_EVERY seconds) */
static bool usb_host_addressed(void)
{
	FILE *f = fopen(PROC_NET_ARP, "r");
	char line[160], dev[16];
	unsigned flags;
	bool found = false;
	if (!f)
		return false;
	/* IP address, HW type, Flags (0x2: complete), HW address, Mask, Device */
	while (!found && fgets(line, sizeof(line), f))
		found = sscanf(line, "%*s %*s %x %*s %*s %15s", &flags, dev) == 2 && (flags & 0x2) &&
		        strcmp(dev, "usb0") == 0;
	fclose(f);
	return found;
}

/* 0: no USB network, 1: USB network without internet, 2: internet */
static int usb_net_state(time_t now)
{
	long carrier;
	if (!ui_read_long(USB_NET "/carrier", &carrier) || !carrier) {
		if (usb_probe_fd >= 0)
			usb_probe_end(false);
		usb_online = false;
		usb_probe_time = 0; /* probe as soon as it comes back */
		return 0;
	}

	if (usb_probe_fd >= 0) {
		struct pollfd pfd = { .fd = usb_probe_fd, .events = POLLOUT };
		if (poll(&pfd, 1, 0) > 0) {
			int err = 1;
			socklen_t len = sizeof(err);
			getsockopt(usb_probe_fd, SOL_SOCKET, SO_ERROR, &err, &len);
			usb_probe_end(err == 0);
		} else if (now - usb_probe_time >= PROBE_TIMEOUT) {
			usb_probe_end(false);
		}
	} else if (now - usb_probe_time >= PROBE_EVERY || now < usb_probe_time) {
		usb_probe_start(now); /* also when the clock went back (NTP) */
	}
	return usb_online ? 2 : 1;
}

void usb_net_update(time_t now)
{
	usb_state = usb_net_state(now);
	usb_lit = usb_state == 2 || (usb_state == 1 && usb_host_addressed());
}
