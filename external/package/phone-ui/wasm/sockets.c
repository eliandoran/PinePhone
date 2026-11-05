/*
 * Pretend sockets: the socket calls are wrapped (-Wl,--wrap, the Makefile's SOCKET_CALLS).
 * The ones phone-ui opens to talk to wpa_supplicant (an AF_UNIX datagram socket, and an
 * AF_INET one for wlan0's flags) and to `mpg123 --remote` (a socket pair, then fork() and
 * exec, which the browser cannot) are answered by wifi.c and player.c at once; every
 * other descriptor goes to Emscripten's calls.
 *
 * music.c's fork() is the one after its socketpair(): it gets a pretend process, which
 * its waitpid() reaps; any other fork() fails as Emscripten's does.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sockets.h"

#define SOCKETS 8
#define REPLY_SIZE 4096
#define PLAYER_PID 4242

static struct fake_socket sockets[SOCKETS];
static bool player_forked; /* a socket pair made, its fork() not called yet */

static struct fake_socket *find(int fd)
{
	for (unsigned i = 0; i < SOCKETS; i++)
		if (fd > 0 && sockets[i].fd == fd)
			return &sockets[i];
	return NULL;
}

static struct fake_socket *create(enum fake_kind kind)
{
	for (unsigned i = 0; i < SOCKETS; i++) {
		if (sockets[i].fd)
			continue;
		int fd = open("/dev/null", O_RDWR);
		if (fd < 0)
			return NULL;
		sockets[i] = (struct fake_socket){ .fd = fd, .kind = kind };
		return &sockets[i];
	}
	errno = EMFILE;
	return NULL;
}

void fake_push(struct fake_socket *s, const char *text)
{
	if (s->count < FAKE_QUEUE && (s->queue[s->count] = strdup(text)))
		s->count++;
}

void fake_push_all(enum fake_kind kind, const char *text)
{
	for (unsigned i = 0; i < SOCKETS; i++)
		if (sockets[i].fd && sockets[i].kind == kind && (kind != FAKE_WPA || sockets[i].attached))
			fake_push(&sockets[i], text);
}

/* What phone-ui reads: a datagram whole (cut to len), or as much of a stream as fits */
static ssize_t take(struct fake_socket *s, void *buf, size_t len)
{
	if (!s->count) {
		errno = EAGAIN;
		return -1;
	}
	char *msg = s->queue[0];
	size_t n = strlen(msg) < len ? strlen(msg) : len;
	memcpy(buf, msg, n);
	if (s->kind == FAKE_PLAYER && n < strlen(msg)) {
		memmove(msg, msg + n, strlen(msg + n) + 1);
		return (ssize_t)n;
	}
	free(msg);
	memmove(s->queue, s->queue + 1, --s->count * sizeof(s->queue[0]));
	return (ssize_t)n;
}

/* What phone-ui sends: a wpa_supplicant command, or the player's lines */
static void give(struct fake_socket *s, const char *data, size_t len)
{
	if (s->kind == FAKE_WPA) {
		char cmd[512], reply[REPLY_SIZE];
		snprintf(cmd, sizeof(cmd), "%.*s", (int)len, data);
		wifi_command(s, cmd, reply, sizeof(reply));
		fake_push(s, reply);
		return;
	}
	for (size_t i = 0; i < len; i++) {
		if (data[i] != '\n') {
			if (s->line_len < sizeof(s->line) - 1)
				s->line[s->line_len++] = data[i];
			continue;
		}
		s->line[s->line_len] = '\0';
		s->line_len = 0;
		player_command(s, s->line);
	}
}

/* The wrapped calls */

int __real_socket(int domain, int type, int protocol);
int __real_socketpair(int domain, int type, int protocol, int sv[2]);
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
ssize_t __real_recv(int fd, void *buf, size_t len, int flags);
ssize_t __real_read(int fd, void *buf, size_t len);
int __real_poll(struct pollfd *fds, nfds_t n, int timeout);
int __real_close(int fd);
int __real_ioctl(int fd, int request, ...);
pid_t __real_fork(void);
pid_t __real_waitpid(pid_t pid, int *status, int options);

int __wrap_socket(int domain, int type, int protocol)
{
	struct fake_socket *s = NULL;
	if (domain == AF_UNIX && (type & 0xf) == SOCK_DGRAM)
		s = create(FAKE_WPA);
	else if (domain == AF_INET && (type & 0xf) == SOCK_DGRAM)
		s = create(FAKE_IFACE);
	else
		return __real_socket(domain, type, protocol);
	return s ? s->fd : -1;
}

/* music.c's to mpg123: its end, and the one the child would have (closed at once) */
int __wrap_socketpair(int domain, int type, int protocol, int sv[2])
{
	if (domain != AF_UNIX || (type & 0xf) != SOCK_STREAM)
		return __real_socketpair(domain, type, protocol, sv);
	struct fake_socket *ours = create(FAKE_PLAYER), *theirs = create(FAKE_PLAYER);
	if (!ours || !theirs) {
		if (ours)
			close(ours->fd);
		return -1;
	}
	sv[0] = ours->fd;
	sv[1] = theirs->fd;
	player_forked = true;
	return 0;
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	return find(fd) ? 0 : __real_bind(fd, addr, len);
}

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	struct fake_socket *s = find(fd);
	if (!s)
		return __real_connect(fd, addr, len);
	if (s->kind != FAKE_WPA || !wifi_connect(((const struct sockaddr_un *)addr)->sun_path)) {
		errno = ENOENT;
		return -1;
	}
	s->connected = true;
	return 0;
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
	struct fake_socket *s = find(fd);
	if (!s)
		return __real_send(fd, buf, len, flags);
	if (s->kind == FAKE_WPA && !s->connected) {
		errno = ENOTCONN;
		return -1;
	}
	give(s, buf, len);
	return (ssize_t)len;
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
	struct fake_socket *s = find(fd);
	return s ? take(s, buf, len) : __real_recv(fd, buf, len, flags);
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
	struct fake_socket *s = find(fd);
	return s ? take(s, buf, len) : __real_read(fd, buf, len);
}

/* One of ours alone: ready when something is queued (a reply always is) */
int __wrap_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	struct fake_socket *s = n == 1 ? find(fds[0].fd) : NULL;
	if (!s)
		return __real_poll(fds, n, timeout);
	fds[0].revents = s->count ? POLLIN : 0;
	return s->count ? 1 : 0;
}

int __wrap_close(int fd)
{
	struct fake_socket *s = find(fd);
	if (s) {
		while (s->count)
			free(s->queue[--s->count]);
		s->fd = 0;
	}
	return __real_close(fd);
}

int __wrap_ioctl(int fd, int request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	struct fake_socket *s = find(fd);
	if (s && s->kind == FAKE_IFACE && (request == SIOCGIFFLAGS || request == SIOCSIFFLAGS) &&
	    wifi_ioctl(request, arg))
		return 0;
	return __real_ioctl(fd, request, arg);
}

pid_t __wrap_fork(void)
{
	if (!player_forked)
		return __real_fork();
	player_forked = false;
	return PLAYER_PID;
}

pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
	if (pid != PLAYER_PID)
		return __real_waitpid(pid, status, options);
	if (status)
		*status = 0;
	return pid;
}
