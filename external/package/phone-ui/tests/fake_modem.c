/*
 * The modem for the UI tests (ui_test.h, "Modem"): the Quectel EG25-G's AT port as a
 * pty, its slave linked at /tmp/modem (MODEM_PORT of modem.o, the Makefile's), the
 * commands answered from a thread as the modem does: echo on until ATE0, numeric
 * +CME ERROR codes (it answers so with AT+CMEE=1 only, but phone-ui sends that first),
 * +QSIMSTAT URCs once AT+QSIMSTAT=1 asked for them. Unplugging closes the master: the
 * slave phone-ui holds reads EIO, as a USB serial port whose device went.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui_test.h"

#define PORT "/tmp/modem"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool plugged, stop;
static int master = -1, slave = -1; /* slave: kept open, or the master reads EIO until phone-ui opens it */
static enum ui_test_sim sim;
static bool echo = true, simstat, silent;
static unsigned busy;

static void say(const char *text)
{
	if (write(master, text, strlen(text)) < 0)
		perror("fake_modem: write");
}

static void answer(const char *cmd)
{
	if (silent)
		return;
	if (echo) {
		say(cmd);
		say("\r");
	}
	if (strcasecmp(cmd, "ATE0") == 0) {
		echo = false;
	} else if (strcasecmp(cmd, "ATE1") == 0) {
		echo = true;
	} else if (strcasecmp(cmd, "AT+QSIMSTAT=1") == 0) {
		simstat = true;
	} else if (strcasecmp(cmd, "AT+CPIN?") == 0) {
		if (busy) {
			busy--;
			say("\r\n+CME ERROR: 14\r\n");
			return;
		}
		static const char *const pin[] = {
			[UI_TEST_SIM_ABSENT] = "\r\n+CME ERROR: 10\r\n",
			[UI_TEST_SIM_READY] = "\r\n+CPIN: READY\r\n",
			[UI_TEST_SIM_PIN] = "\r\n+CPIN: SIM PIN\r\n",
			[UI_TEST_SIM_PUK] = "\r\n+CPIN: SIM PUK\r\n",
		};
		say(pin[sim]);
		if (sim == UI_TEST_SIM_ABSENT)
			return;
	} else if (strcasecmp(cmd, "AT") != 0 && strcasecmp(cmd, "AT+CMEE=1") != 0) {
		say("\r\nERROR\r\n");
		return;
	}
	say("\r\nOK\r\n");
}

static void *run(void *arg)
{
	(void)arg;
	char line[256];
	size_t len = 0;
	for (;;) {
		struct pollfd p = { .fd = master, .events = POLLIN };
		int ready = poll(&p, 1, 10);
		pthread_mutex_lock(&lock);
		if (stop) {
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		char buf[256];
		ssize_t n = ready > 0 ? read(master, buf, sizeof(buf)) : 0;
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\r') {
				line[len] = '\0';
				if (len)
					answer(line);
				len = 0;
			} else if (buf[i] != '\n' && len + 1 < sizeof(line)) {
				line[len++] = buf[i];
			}
		}
		pthread_mutex_unlock(&lock);
	}
}

static void plug(void)
{
	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
		perror("fake_modem: pty");
		return;
	}
	const char *name = ptsname(master);
	slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	unlink(PORT);
	if (slave < 0 || symlink(name, PORT) != 0)
		perror("fake_modem: " PORT);
	echo = true;
	simstat = false;
	stop = false;
	plugged = true;
	pthread_create(&thread, NULL, run, NULL);
}

void ui_test_modem(enum ui_test_sim s)
{
	pthread_mutex_lock(&lock);
	enum ui_test_sim was = sim;
	sim = s;
	if (plugged && simstat && (was == UI_TEST_SIM_ABSENT) != (s == UI_TEST_SIM_ABSENT))
		say(s == UI_TEST_SIM_ABSENT ? "\r\n+QSIMSTAT: 1,0\r\n" : "\r\n+QSIMSTAT: 1,1\r\n");
	bool start = !plugged;
	pthread_mutex_unlock(&lock);
	if (start)
		plug();
}

void ui_test_modem_unplug(void)
{
	pthread_mutex_lock(&lock);
	bool was = plugged;
	stop = true;
	plugged = false;
	pthread_mutex_unlock(&lock);
	if (!was)
		return;
	pthread_join(thread, NULL);
	unlink(PORT);
	close(slave);
	close(master);
	slave = master = -1;
}

void ui_test_modem_restart(enum ui_test_sim s)
{
	pthread_mutex_lock(&lock);
	sim = s;
	echo = true;
	simstat = false;
	if (plugged)
		say("\r\nRDY\r\n");
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_silent(bool s)
{
	pthread_mutex_lock(&lock);
	silent = s;
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_sim_busy(unsigned count)
{
	pthread_mutex_lock(&lock);
	busy = count;
	pthread_mutex_unlock(&lock);
}
