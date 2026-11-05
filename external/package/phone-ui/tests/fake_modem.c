/*
 * The modem for the UI tests (ui_test.h, "Modem"): the Quectel EG25-G's AT port as a
 * pty, its slave linked at /tmp/modem (the port of ui_test_reset()'s modem.conf), the
 * commands answered from a thread as the modem does: echo on until ATE0, numeric
 * +CME ERROR codes (it answers so with AT+CMEE=1 only, but phone-ui sends that first),
 * +QSIMSTAT URCs once AT+QSIMSTAT=1 asked for them. The network: registration (circuit
 * switched +CREG, LTE +CEREG, reported once AT+CREG=1/AT+CEREG=1 asked for it) and the
 * operator (+COPS, its name once AT+COPS=3,0 asked for it, numeric before), registered
 * only with a ready SIM. Its signal: AT+CESQ (the technology's own measure: RSRP for
 * LTE, RSCP for UMTS, RxLev for GSM), AT+CSQ, the technology +COPS's <AcT>.
 * Unplugging closes the master: the slave phone-ui holds reads
 * EIO, as a USB serial port whose device went.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
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
static enum ui_test_net net = UI_TEST_NET_HOME;
static char operator[64] = "Fake Mobile";
static bool lte_only, creg, cereg, names;
static int dbm = -80;                       /* the signal */
static enum ui_test_tech tech = UI_TEST_TECH_LTE;
static bool cesq = true;                    /* AT+CESQ answered */
static bool data;                           /* the data session (AT$QCRMCALL) */
static unsigned data_fails, data_starts;

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void say(const char *text)
{
	if (write(master, text, strlen(text)) < 0)
		perror("fake_modem: write");
}

static void sayf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sayf(const char *fmt, ...)
{
	char text[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	say(text);
}

static bool registered(enum ui_test_net n)
{
	return n == UI_TEST_NET_HOME || n == UI_TEST_NET_ROAMING;
}

/* The registration state of a domain: circuit switched (+CREG) or LTE (+CEREG) */
static enum ui_test_net state(bool cs)
{
	if (sim != UI_TEST_SIM_READY)
		return UI_TEST_NET_NONE;
	return cs && lte_only && registered(net) ? UI_TEST_NET_NONE : net;
}

/* The URCs of what changed since `cs`, `eps` */
static void report(enum ui_test_net cs, enum ui_test_net eps)
{
	if (!plugged)
		return;
	if (creg && state(true) != cs)
		sayf("\r\n+CREG: %d\r\n", state(true));
	if (cereg && state(false) != eps)
		sayf("\r\n+CEREG: %d\r\n", state(false));
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
	} else if (strcasecmp(cmd, "AT+CREG=1") == 0) {
		creg = true;
	} else if (strcasecmp(cmd, "AT+CEREG=1") == 0) {
		cereg = true;
	} else if (strcasecmp(cmd, "AT+CREG?") == 0) {
		sayf("\r\n+CREG: %d,%d\r\n", creg, state(true));
	} else if (strcasecmp(cmd, "AT+CEREG?") == 0) {
		sayf("\r\n+CEREG: %d,%d\r\n", cereg, state(false));
	} else if (strcasecmp(cmd, "AT+COPS=3,0") == 0) {
		names = true;
	} else if (strcasecmp(cmd, "AT+COPS?") == 0) {
		if (!registered(state(true)) && !registered(state(false)))
			say("\r\n+COPS: 0\r\n");
		else if (names)
			sayf("\r\n+COPS: 0,0,\"%s\",%d\r\n", operator, tech);
		else
			sayf("\r\n+COPS: 0,2,\"20801\",%d\r\n", tech);
	} else if (strcasecmp(cmd, "AT+CESQ") == 0) {
		if (!cesq) {
			say("\r\nERROR\r\n");
			return;
		}
		/* <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>: the technology's own, the others
		 * unknown (99, 255) */
		bool on = registered(state(true)) || registered(state(false));
		int rxlev = 99, rscp = 255, ecno = 255, rsrq = 255, rsrp = 255;
		if (on && (tech == UI_TEST_TECH_LTE || tech == UI_TEST_TECH_NR_NSA))
			rsrp = clamp(dbm + 141, 0, 97), rsrq = 20;
		else if (on && (tech == UI_TEST_TECH_UMTS || tech == UI_TEST_TECH_HSPA))
			rscp = clamp(dbm + 121, 0, 96), ecno = 30;
		else if (on)
			rxlev = clamp(dbm + 111, 0, 63);
		sayf("\r\n+CESQ: %d,99,%d,%d,%d,%d\r\n", rxlev, rscp, ecno, rsrq, rsrp);
	} else if (strcasecmp(cmd, "AT+CSQ") == 0) {
		bool on = registered(state(true)) || registered(state(false));
		sayf("\r\n+CSQ: %d,99\r\n", on ? clamp((dbm + 113) / 2, 0, 31) : 99);
	} else if (strcasecmp(cmd, "AT$QCRMCALL=1,1") == 0) {
		/* The data session on the network interface: registered, and not failing */
		if (data_fails || (!registered(state(true)) && !registered(state(false)))) {
			if (data_fails)
				data_fails--;
			say("\r\nERROR\r\n");
			return;
		}
		data = true;
		data_starts++;
		say("\r\n$QCRMCALL: 1,V4\r\n");
	} else if (strcasecmp(cmd, "AT$QCRMCALL=0,1") == 0) {
		data = false;
	} else if (strcasecmp(cmd, "AT$QCRMCALL?") == 0) {
		if (data)
			say("\r\n$QCRMCALL: 1,V4\r\n");
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

/* As after power-up: nothing asked for yet */
static void fresh(void)
{
	echo = true;
	simstat = creg = cereg = names = data = false;
}

/* As a USB serial port (ui_test_modem_usb()): its tty in sysfs, interface 2 of the
 * EG25-G (2c7c:0125), and /dev/<tty> to the pty */
#define USB_DEV "/sys/devices/platform/soc/1c1b000.usb/usb1/1-1"
#define USB_IF USB_DEV "/1-1:1.2"
static char usb_tty[32];

static void usb_add(void)
{
	char path[256], target[256];
	if (!usb_tty[0])
		return;
	ui_test_write(USB_DEV "/idVendor", "2c7c\n");
	ui_test_write(USB_DEV "/idProduct", "0125\n");
	ui_test_write(USB_IF "/bInterfaceNumber", "02\n");
	snprintf(path, sizeof(path), USB_IF "/%s/port_number", usb_tty);
	ui_test_write(path, "0\n");
	snprintf(path, sizeof(path), "/sys/class/tty/%s/dev", usb_tty);
	ui_test_write(path, "188:2\n");
	snprintf(path, sizeof(path), "/sys/class/tty/%s/device", usb_tty);
	snprintf(target, sizeof(target), USB_IF "/%s", usb_tty);
	if (symlink(target, path) != 0)
		perror("fake_modem: sysfs");
	snprintf(path, sizeof(path), "/dev/%s", usb_tty);
	unlink(path);
	if (symlink(PORT, path) != 0)
		perror("fake_modem: /dev");
}

static void usb_remove(void)
{
	char path[256];
	if (!usb_tty[0])
		return;
	snprintf(path, sizeof(path), "/dev/%s", usb_tty);
	unlink(path);
	snprintf(path, sizeof(path), "/sys/class/tty/%s/device", usb_tty);
	unlink(path);
	snprintf(path, sizeof(path), "/sys/class/tty/%s/dev", usb_tty);
	unlink(path);
	snprintf(path, sizeof(path), "/sys/class/tty/%s", usb_tty);
	rmdir(path);
}

void ui_test_modem_usb(const char *tty)
{
	pthread_mutex_lock(&lock);
	if (plugged)
		usb_remove();
	snprintf(usb_tty, sizeof(usb_tty), "%s", tty);
	if (plugged)
		usb_add();
	pthread_mutex_unlock(&lock);
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
	usb_add();
	fresh();
	stop = false;
	plugged = true;
	pthread_create(&thread, NULL, run, NULL);
}

void ui_test_modem(enum ui_test_sim s)
{
	pthread_mutex_lock(&lock);
	enum ui_test_sim was = sim;
	enum ui_test_net cs = state(true), eps = state(false);
	sim = s;
	if (plugged && simstat && (was == UI_TEST_SIM_ABSENT) != (s == UI_TEST_SIM_ABSENT))
		say(s == UI_TEST_SIM_ABSENT ? "\r\n+QSIMSTAT: 1,0\r\n" : "\r\n+QSIMSTAT: 1,1\r\n");
	report(cs, eps);
	bool start = !plugged;
	pthread_mutex_unlock(&lock);
	if (start)
		plug();
}

void ui_test_modem_network(enum ui_test_net n, const char *name)
{
	pthread_mutex_lock(&lock);
	enum ui_test_net cs = state(true), eps = state(false);
	net = n;
	snprintf(operator, sizeof(operator), "%s", name ? name : "");
	report(cs, eps);
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_lte_only(bool only)
{
	pthread_mutex_lock(&lock);
	enum ui_test_net cs = state(true), eps = state(false);
	lte_only = only;
	report(cs, eps);
	pthread_mutex_unlock(&lock);
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
	usb_remove();
	close(slave);
	close(master);
	slave = master = -1;
}

void ui_test_modem_restart(enum ui_test_sim s)
{
	pthread_mutex_lock(&lock);
	sim = s;
	fresh();
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

void ui_test_modem_signal(int signal_dbm, enum ui_test_tech technology)
{
	pthread_mutex_lock(&lock);
	dbm = signal_dbm;
	tech = technology;
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_no_cesq(void)
{
	pthread_mutex_lock(&lock);
	cesq = false;
	pthread_mutex_unlock(&lock);
}

bool ui_test_modem_data(unsigned *starts)
{
	pthread_mutex_lock(&lock);
	bool on = data;
	if (starts)
		*starts = data_starts;
	pthread_mutex_unlock(&lock);
	return on;
}

void ui_test_modem_data_fails(unsigned count)
{
	pthread_mutex_lock(&lock);
	data_fails = count;
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_data_drop(void)
{
	pthread_mutex_lock(&lock);
	data = false;
	pthread_mutex_unlock(&lock);
}
