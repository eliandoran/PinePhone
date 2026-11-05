/*
 * The modem, a Quectel EG25-G on the internal USB: its AT port (ttyUSB2, the option
 * driver's third) is there once the modem is powered on, and goes when it is powered off
 * or restarts. Opened when it appears, read without blocking from a timer, opened again
 * after it went. One command at a time; the unsolicited lines come between them.
 *
 * Set up as it answers (echo off, numeric errors, SIM insertion reports), then asked for
 * the SIM's state: AT+CPIN?, again after a while until it is ready (busy just after
 * power-up, a card put in on a modem that does not report it). +QSIMSTAT says when the
 * card goes or comes, "RDY" that the modem restarted (set up again).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#include "modem.h"
#include "ui.h"

#ifndef MODEM_PORT
#define MODEM_PORT "/dev/ttyUSB2"
#endif
#define TICK_MS 200
#define OPEN_EVERY_MS 1000
#define ANSWER_MS 5000    /* then it is not answering: set up again */
#define SIM_AGAIN_MS 10000 /* while the SIM is not ready */
#define BUSY_AGAIN_MS 1000 /* SIM busy, or not ready yet */
#define QUEUE 8

#define CPIN "AT+CPIN?"
static const char *const setup[] = { "ATE0", "AT+CMEE=1", "AT+QSIMSTAT=1", CPIN };

static struct {
	int fd;
	enum modem_state state;
	uint32_t open_at;       /* last attempt */
	const char *queue[QUEUE];
	unsigned queued;
	const char *sent;       /* waiting for its answer */
	uint32_t sent_at;
	bool ask;               /* AT+CPIN? at ask_at */
	uint32_t ask_at;
	char line[512];
	size_t len;
} m = { .fd = -1 };

static const char *const texts[][2] = {
	[MODEM_ABSENT] = { "No modem", "No modem" },
	[MODEM_STARTING] = { "Modem...", "Starting" },
	[MODEM_NO_SIM] = { "No SIM", "No SIM card" },
	[MODEM_SIM_PIN] = { "SIM locked", "SIM locked (PIN)" },
	[MODEM_SIM_PUK] = { "SIM blocked", "SIM blocked (PUK)" },
	[MODEM_SIM_ERROR] = { "SIM error", "SIM error" },
	[MODEM_READY] = { "SIM ready", "SIM ready" },
};

enum modem_state modem_state(void)
{
	return m.state;
}

const char *modem_status_text(void)
{
	return texts[m.state][0];
}

const char *modem_summary(void)
{
	return texts[m.state][1];
}

static void state_set(enum modem_state state)
{
	if (state != m.state)
		printf("phone-ui: modem: %s\n", texts[state][1]);
	m.state = state;
}

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
	for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]); i++)
		enqueue(setup[i]);
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
		printf("phone-ui: modem: " MODEM_PORT " gone\n");
	}
	m.fd = -1;
	m.queued = 0;
	m.sent = NULL;
	m.ask = false;
	state_set(MODEM_ABSENT);
}

static bool port_open(void)
{
	int fd = open(MODEM_PORT, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(fd, TCSANOW, &t);
	}
	tcflush(fd, TCIOFLUSH);
	m.fd = fd;
	printf("phone-ui: modem: " MODEM_PORT " opened\n");
	state_set(MODEM_STARTING);
	set_up();
	return true;
}

/* +CPIN: <code>, answered or unsolicited */
static void sim_code(const char *code)
{
	if (strcmp(code, "READY") == 0)
		state_set(MODEM_READY);
	else if (strcmp(code, "NOT INSERTED") == 0)
		state_set(MODEM_NO_SIM);
	else if (strstr(code, "PUK"))
		state_set(MODEM_SIM_PUK);
	else if (strstr(code, "PIN"))
		state_set(MODEM_SIM_PIN);
	else
		ask_sim_in(BUSY_AGAIN_MS); /* NOT READY: starting, or the card just went */
}

/* The final answer to what was sent: OK, or an error (+CME ERROR's code, -1 none) */
static void answered(bool ok, int error)
{
	const char *cmd = m.sent;
	m.sent = NULL;
	if (cmd != CPIN)
		return;
	if (!ok) {
		if (error == 10)
			state_set(MODEM_NO_SIM);
		else if (error == 14 || error < 0)
			ask_sim_in(BUSY_AGAIN_MS); /* busy */
		else
			state_set(MODEM_SIM_ERROR); /* 13 failure, 15 wrong... */
	} else if (m.state == MODEM_STARTING) {
		ask_sim_in(BUSY_AGAIN_MS); /* OK without +CPIN */
	}
	if (m.state != MODEM_READY && !m.ask)
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
	if (strcmp(l, "OK") == 0)
		answered(true, 0);
	else if (strcmp(l, "ERROR") == 0)
		answered(false, -1);
	else if (strncmp(l, "+CME ERROR: ", 12) == 0)
		answered(false, error_code(l + 12));
	else if (strncmp(l, "+CPIN: ", 7) == 0)
		sim_code(l + 7);
	else if (strncmp(l, "+QSIMSTAT: ", 11) == 0) {
		/* <enabled>,<inserted> */
		const char *inserted = strchr(l, ',');
		if (inserted && inserted[1] == '0') {
			state_set(MODEM_NO_SIM);
			ask_sim_in(SIM_AGAIN_MS);
		} else if (inserted && inserted[1] == '1')
			enqueue(CPIN);
	} else if (strcmp(l, "RDY") == 0) {
		printf("phone-ui: modem: restarted\n");
		state_set(MODEM_STARTING);
		set_up();
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	uint32_t now = lv_tick_get();
	if (m.fd < 0) {
		if (lv_tick_diff(now, m.open_at) < OPEN_EVERY_MS && m.open_at)
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
		state_set(MODEM_STARTING);
		set_up();
	}
	if (m.ask && (int32_t)(now - m.ask_at) >= 0) {
		m.ask = false;
		enqueue(CPIN);
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

void modem_init(void)
{
	lv_timer_create(tick_cb, TICK_MS, NULL);
}
