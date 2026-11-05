/*
 * Modem provider "at": standard AT commands (3GPP TS 27.007) on a serial port,
 * modem.conf's port= (a USB modem's: there once it is powered on, gone when it is
 * powered off or restarts). Opened when it appears, read without blocking from a
 * timer, opened again after it went. One command at a time; the unsolicited lines
 * come between them.
 *
 * Set up as it answers (echo off, numeric errors), then asked for the SIM's state:
 * AT+CPIN?, again after a while (busy just after power-up, a card put in or taken out).
 *
 * quirks= in modem.conf, for what is not standard:
 *	quectel  the card going or coming reported (AT+QSIMSTAT=1, +QSIMSTAT): a ready
 *	         SIM is not asked for again; "RDY" when the modem restarted (set up again)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#include "modem_provider.h"
#include "ui.h"

#define TICK_MS 200
#define OPEN_EVERY_MS 1000
#define ANSWER_MS 5000     /* then it is not answering: set up again */
#define SIM_AGAIN_MS 10000 /* while the SIM is not ready, or without reports */
#define BUSY_AGAIN_MS 1000 /* SIM busy, or not ready yet */
#define QUEUE 8

/* Commands are compared by address: one copy of each */
static const char cpin[] = "AT+CPIN?";
static const char echo_off[] = "ATE0";
static const char errors[] = "AT+CMEE=1";
static const char simstat[] = "AT+QSIMSTAT=1";

static struct {
	const char *port;
	bool quectel;
	int fd;
	uint32_t open_at;       /* last attempt, 0 none */
	const char *queue[QUEUE];
	unsigned queued;
	const char *sent;       /* waiting for its answer */
	uint32_t sent_at;
	bool ask;               /* AT+CPIN? at ask_at */
	uint32_t ask_at;
	char line[512];
	size_t len;
} m = { .fd = -1 };

static void enqueue(const char *cmd)
{
	if (m.sent == cmd)
		return;
	for (unsigned i = 0; i < m.queued; i++)
		if (m.queue[i] == cmd)
			return;
	if (m.queued < QUEUE)
		m.queue[m.queued++] = cmd;
}

/* From the start: echo may be on again, nothing asked is answered */
static void set_up(void)
{
	m.queued = 0;
	m.sent = NULL;
	m.ask = false;
	m.len = 0;
	enqueue(echo_off);
	enqueue(errors);
	if (m.quectel)
		enqueue(simstat);
	enqueue(cpin);
}

static void ask_sim_in(uint32_t ms)
{
	m.ask = true;
	m.ask_at = lv_tick_get() + ms;
}

static void port_close(void)
{
	if (m.fd >= 0) {
		close(m.fd);
		printf("phone-ui: modem: %s gone\n", m.port);
	}
	m.fd = -1;
	m.queued = 0;
	m.sent = NULL;
	m.ask = false;
	modem_set_state(MODEM_ABSENT);
}

static bool port_open(void)
{
	int fd = open(m.port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(fd, TCSANOW, &t);
	}
	tcflush(fd, TCIOFLUSH);
	m.fd = fd;
	printf("phone-ui: modem: %s opened\n", m.port);
	modem_set_state(MODEM_STARTING);
	set_up();
	return true;
}

/* +CPIN: <code>, answered or unsolicited */
static void sim_code(const char *code)
{
	if (strcmp(code, "READY") == 0)
		modem_set_state(MODEM_READY);
	else if (strcmp(code, "NOT INSERTED") == 0)
		modem_set_state(MODEM_NO_SIM);
	else if (strstr(code, "PUK"))
		modem_set_state(MODEM_SIM_PUK);
	else if (strstr(code, "PIN"))
		modem_set_state(MODEM_SIM_PIN);
	else
		ask_sim_in(BUSY_AGAIN_MS); /* NOT READY: starting, or the card just went */
}

/* The final answer to what was sent: OK, or an error (+CME ERROR's code, -1 none) */
static void answered(bool ok, int error)
{
	const char *cmd = m.sent;
	m.sent = NULL;
	if (cmd != cpin)
		return;
	if (!ok) {
		if (error == 10)
			modem_set_state(MODEM_NO_SIM);
		else if (error == 14 || error < 0)
			ask_sim_in(BUSY_AGAIN_MS); /* busy */
		else
			modem_set_state(MODEM_SIM_ERROR); /* 13 failure, 15 wrong... */
	} else if (modem_state() == MODEM_STARTING) {
		ask_sim_in(BUSY_AGAIN_MS); /* OK without +CPIN */
	}
	/* A card going is reported by Quectel's modems, asked for again otherwise */
	if (!m.ask && (modem_state() != MODEM_READY || !m.quectel))
		ask_sim_in(SIM_AGAIN_MS);
}

/* +CME ERROR: a number (AT+CMEE=1), or its text before that took (AT+CMEE=2) */
static int error_code(const char *s)
{
	if (*s >= '0' && *s <= '9')
		return atoi(s);
	return strcasestr(s, "not inserted") ? 10 : strcasestr(s, "busy") ? 14 : -1;
}

static void line(const char *l)
{
	if (strncasecmp(l, "AT", 2) == 0)
		return; /* the echo of a command */
	if (strcmp(l, "OK") == 0) {
		answered(true, 0);
	} else if (strcmp(l, "ERROR") == 0) {
		answered(false, -1);
	} else if (strncmp(l, "+CME ERROR: ", 12) == 0) {
		answered(false, error_code(l + 12));
	} else if (strncmp(l, "+CPIN: ", 7) == 0) {
		sim_code(l + 7);
	} else if (m.quectel && strncmp(l, "+QSIMSTAT: ", 11) == 0) {
		/* <enabled>,<inserted> */
		const char *inserted = strchr(l, ',');
		if (inserted && inserted[1] == '0') {
			modem_set_state(MODEM_NO_SIM);
			ask_sim_in(SIM_AGAIN_MS);
		} else if (inserted && inserted[1] == '1') {
			enqueue(cpin);
		}
	} else if (m.quectel && strcmp(l, "RDY") == 0) {
		printf("phone-ui: modem: restarted\n");
		modem_set_state(MODEM_STARTING);
		set_up();
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	uint32_t now = lv_tick_get();
	if (m.fd < 0) {
		if (m.open_at && lv_tick_diff(now, m.open_at) < OPEN_EVERY_MS)
			return;
		m.open_at = now ? now : 1;
		if (!port_open())
			return;
	}

	char buf[512];
	ssize_t n;
	while ((n = read(m.fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\r' || buf[i] == '\n') {
				m.line[m.len] = '\0';
				if (m.len)
					line(m.line);
				m.len = 0;
			} else if (m.len + 1 < sizeof(m.line)) {
				m.line[m.len++] = buf[i];
			}
		}
	}
	if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
		port_close(); /* the device went (EIO) */
		return;
	}

	if (m.sent && lv_tick_diff(now, m.sent_at) >= ANSWER_MS) {
		printf("phone-ui: modem: no answer to %s\n", m.sent);
		modem_set_state(MODEM_STARTING);
		set_up();
	}
	if (m.ask && (int32_t)(now - m.ask_at) >= 0) {
		m.ask = false;
		enqueue(cpin);
	}
	if (!m.sent && m.queued) {
		char cmd[64];
		m.sent = m.queue[0];
		m.sent_at = now;
		memmove(m.queue, m.queue + 1, --m.queued * sizeof(m.queue[0]));
		int len = snprintf(cmd, sizeof(cmd), "%s\r", m.sent);
		if (write(m.fd, cmd, (size_t)len) != len)
			printf("phone-ui: modem: cannot send %s\n", m.sent); /* not answered: set up again */
	}
}

static bool start(void)
{
	m.port = modem_config("port");
	if (!m.port) {
		printf("phone-ui: modem: no port= for the AT provider\n");
		return false;
	}
	m.quectel = modem_config_has("quirks", "quectel");
	lv_timer_create(tick_cb, TICK_MS, NULL);
	return true;
}

const struct modem_provider modem_at = { .name = "at", .start = start };
