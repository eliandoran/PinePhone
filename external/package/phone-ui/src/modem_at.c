/*
 * Modem provider "at": standard AT commands (3GPP TS 27.007) on a serial port,
 * modem.conf's port=, or for a USB modem usb=<vendor>:<product> (hexadecimal) and
 * interface=<number>: the tty of that interface, found in sysfs each time, as its
 * number changes (there once the modem is powered on, gone when it is powered off or
 * restarts). Opened when it appears, read without blocking from a timer, opened again
 * after it went. One command at a time; the unsolicited lines come between them.
 *
 * Set up as it answers (echo off, numeric errors), then asked for the SIM's state:
 * AT+CPIN?, again after a while (busy just after power-up, a card put in or taken out).
 *
 * The network: registration reported as it changes (AT+CREG=1 circuit switched,
 * AT+CEREG=1 LTE: either registered counts, an LTE-only network leaves +CREG at 0),
 * asked once the SIM is ready; registered, the operator's name (AT+COPS=3,0 for it in
 * full, then AT+COPS?), asked again whenever the registration changes.
 *
 * Registered, the signal: AT+CESQ (3GPP's measure of each technology: RSRP for LTE,
 * RSCP for UMTS, RxLev for GSM; AT+CSQ's RSSI on a modem without it) in bars at
 * Android's default thresholds, and the technology +COPS gives with the operator
 * (<AcT>); both asked again every 10 s, as neither is reported when it changes.
 *
 * Mobile data (modem.c): the session on the modem's network interface started while it
 * is wanted (AT$QCRMCALL=1,1, Quectel's: standard AT has no way to tie one to the
 * interface), checked every 30 s (AT$QCRMCALL? lists it while up), ended when no longer
 * wanted. Refused, the board resets the interface (as after the modem's USB reset,
 * which leaves it refusing until its driver is bound again) and it is tried again.
 * The modem gone, restarted or not answering, the session is taken as ended.
 *
 * Its IMEI asked as it is set up (AT+CGSN), the SIM's own number once the network's
 * state is known (AT+CNUM: what the SIM holds, often nothing; asked again with the
 * signal while the SIM was busy).
 *
 * quirks= in modem.conf, for what is not standard:
 *	quectel  the card going or coming reported (AT+QSIMSTAT=1, +QSIMSTAT): a ready
 *	         SIM is not asked for again; "RDY" when the modem restarted (set up again);
 *	         mobile data (AT$QCRMCALL)
 */

#include <dirent.h>
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
#define SIGNAL_MS 10000    /* the signal and technology asked again, registered */
#define DATA_CHECK_MS 30000 /* the data session asked about */
#define DATA_RETRY_MS 5000  /* after a refused session */
#define QUEUE 16

/* Commands are compared by address: one copy of each */
static const char cpin[] = "AT+CPIN?";
static const char echo_off[] = "ATE0";
static const char errors[] = "AT+CMEE=1";
static const char simstat[] = "AT+QSIMSTAT=1";
static const char creg_on[] = "AT+CREG=1";
static const char cereg_on[] = "AT+CEREG=1";
static const char names[] = "AT+COPS=3,0";
static const char creg[] = "AT+CREG?";
static const char cereg[] = "AT+CEREG?";
static const char cops[] = "AT+COPS?";
static const char cesq[] = "AT+CESQ";
static const char csq[] = "AT+CSQ";
static const char data_on[] = "AT$QCRMCALL=1,1";
static const char data_off[] = "AT$QCRMCALL=0,1";
static const char data_query[] = "AT$QCRMCALL?";
static const char cgsn[] = "AT+CGSN";
static const char cnum[] = "AT+CNUM";

static struct {
	const char *port;       /* port=, or NULL: usb= and interface= */
	long vendor, product, interface;
	char path[280];         /* the port found */
	bool quectel;
	int fd;
	int cs, eps;            /* +CREG's, +CEREG's <stat>, -1 not known */
	bool registered;        /* by them, at the last check */
	int bars;               /* -1 not known */
	enum modem_tech tech;
	bool no_cesq;           /* answered ERROR: AT+CSQ instead */
	uint32_t signal_at;     /* last asked */
	bool data;              /* the data session is up */
	bool data_listed;       /* $QCRMCALL: seen since the last command */
	uint32_t data_at;       /* last checked, or refused */
	bool data_refused;      /* the last start was: DATA_RETRY_MS from data_at */
	bool number_known;      /* AT+CNUM answered for this SIM */
	uint32_t open_at;       /* last attempt, 0 none */
	const char *queue[QUEUE];
	unsigned queued;
	const char *sent;       /* waiting for its answer */
	uint32_t sent_at;
	bool ask;               /* AT+CPIN? at ask_at */
	uint32_t ask_at;
	char line[512];
	size_t len;
} m = { .fd = -1, .cs = -1, .eps = -1, .bars = -1 };

static void signal_ask(void);
static void enqueue(const char *cmd);

/* The data session taken as ended (the modem gone, restarted, not answering) */
static void data_forget(void)
{
	m.data = m.data_refused = false;
	if (modem_data_state() != MODEM_DATA_OFF)
		modem_set_data(MODEM_DATA_WAITING);
}

/* The data session started, ended or checked as needed (each tick) */
static void data_update(uint32_t now)
{
	bool want = m.quectel && modem_data_wanted();
	if (want && !m.data) {
		if (!m.data_refused || lv_tick_diff(now, m.data_at) >= DATA_RETRY_MS) {
			m.data_refused = false;
			if (m.sent != data_on)
				enqueue(data_on);
			modem_set_data(MODEM_DATA_CONNECTING);
		}
	} else if (!want && m.data) {
		m.data = false;
		enqueue(data_off);
	} else if (want && lv_tick_diff(now, m.data_at) >= DATA_CHECK_MS) {
		m.data_at = now;
		enqueue(data_query);
	}
	if (!want)
		modem_set_data(modem_data_enabled() ? MODEM_DATA_WAITING : MODEM_DATA_OFF);
}

/* What the data commands' answers mean, once final */
static void data_answered(const char *cmd, bool ok)
{
	if (cmd == data_on && ok && m.data_listed) {
		m.data = true;
		m.data_at = lv_tick_get();
		modem_set_data(MODEM_DATA_CONNECTED);
	} else if (cmd == data_on) {
		printf("phone-ui: modem: data session refused\n");
		m.data_refused = true;
		m.data_at = lv_tick_get();
		modem_data_script("reset");
	} else if (cmd == data_query && ok && !m.data_listed && m.data) {
		printf("phone-ui: modem: data session ended\n");
		m.data = false;
		modem_set_data(MODEM_DATA_WAITING);
	}
}

static void network_forget(void)
{
	m.cs = m.eps = -1;
	m.registered = false;
	m.bars = -1;
	m.tech = MODEM_TECH_UNKNOWN;
}

/* Bars from dBm at thresholds for 1, 2, 3 and 4 bars */
static int bars_of(int dbm, const int thresholds[4])
{
	int bars = 0;
	while (bars < 4 && dbm >= thresholds[bars])
		bars++;
	return bars;
}

/* Android's defaults (CellSignalStrength*): LTE's RSRP, UMTS's RSCP; GSM's RSSI
 * (ASU 3, 5, 8, 12), also for AT+CSQ's */
static const int rsrp_levels[4] = { -115, -105, -95, -85 };
static const int rscp_levels[4] = { -115, -105, -95, -85 };
static const int rssi_levels[4] = { -107, -103, -97, -89 };

/* +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>: the serving technology's set, the
 * others "not known" (99, 255) */
static void cesq_line(const char *l)
{
	int rxlev, ber, rscp, ecno, rsrq, rsrp;
	if (sscanf(l, "+CESQ: %d,%d,%d,%d,%d,%d", &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) != 6)
		return;
	(void)ber, (void)ecno, (void)rsrq;
	/* Each step a dB from the lowest: RSRP 1 is -140 dBm, RSCP 1 -120, RxLev 1 -110 */
	m.bars = rsrp >= 0 && rsrp <= 97 ? bars_of(rsrp - 141, rsrp_levels) :
	         rscp >= 0 && rscp <= 96 ? bars_of(rscp - 121, rscp_levels) :
	         rxlev >= 0 && rxlev <= 63 ? bars_of(rxlev - 111, rssi_levels) : -1;
	modem_set_signal(m.bars, m.tech);
}

/* +CSQ: <rssi>,<ber>: 0 -113 dBm, 2 dB a step, 99 not known */
static void csq_line(const char *l)
{
	int rssi;
	if (sscanf(l, "+CSQ: %d", &rssi) != 1)
		return;
	m.bars = rssi >= 0 && rssi <= 31 ? bars_of(rssi * 2 - 113, rssi_levels) : -1;
	modem_set_signal(m.bars, m.tech);
}

/* +COPS's <AcT> (27.007): 0 GSM, 1 GSM compact, 2 UTRAN, 3 EGPRS, 4-6 HSPA, 7 E-UTRAN,
 * 8-9 E-UTRAN (IoT), 10 E-UTRAN on a 5G core, 11-12 NR, 13 LTE and NR (non-standalone) */
static enum modem_tech tech_of(int act)
{
	return act < 0 ? MODEM_TECH_UNKNOWN :
	       act == 0 || act == 1 || act == 3 ? MODEM_TECH_2G :
	       act == 2 || (act >= 4 && act <= 6) ? MODEM_TECH_3G :
	       act >= 7 && act <= 10 ? MODEM_TECH_4G :
	       act <= 13 ? MODEM_TECH_5G : MODEM_TECH_UNKNOWN;
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
	network_forget();
	data_forget();
	m.number_known = false;
	enqueue(echo_off);
	enqueue(errors);
	enqueue(cgsn);
	if (m.quectel) {
		enqueue(simstat);
		/* A session the modem kept (an earlier phone-ui's): started again if wanted */
		enqueue(data_off);
	}
	enqueue(creg_on);
	enqueue(cereg_on);
	enqueue(names);
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
		printf("phone-ui: modem: %s gone\n", m.path);
	}
	m.fd = -1;
	m.queued = 0;
	m.sent = NULL;
	m.ask = false;
	data_forget();
	modem_set_state(MODEM_ABSENT);
}

/* A hexadecimal sysfs attribute of the tty `name`'s device (relative to it), -1 if none */
static long tty_attr(const char *name, const char *attr)
{
	char path[256], value[16];
	snprintf(path, sizeof(path), "/sys/class/tty/%s/device/%s", name, attr);
	return ui_read_line(path, value, sizeof(value)) ? strtol(value, NULL, 16) : -1;
}

/* The port in m.path: port=, or the tty of interface= of the USB device usb= (its
 * number changes: a port still open when the modem reset keeps its name taken) */
static bool port_find(void)
{
	if (m.port) {
		snprintf(m.path, sizeof(m.path), "%s", m.port);
		return true;
	}
	DIR *dir = opendir("/sys/class/tty");
	struct dirent *e;
	bool found = false;
	while (dir && !found && (e = readdir(dir))) {
		if (e->d_name[0] == '.')
			continue;
		/* device: the USB serial port, in its interface, in the USB device */
		found = tty_attr(e->d_name, "../bInterfaceNumber") == m.interface &&
		        tty_attr(e->d_name, "../../idVendor") == m.vendor &&
		        tty_attr(e->d_name, "../../idProduct") == m.product;
		if (found)
			snprintf(m.path, sizeof(m.path), "/dev/%s", e->d_name);
	}
	if (dir)
		closedir(dir);
	return found;
}

static bool port_open(void)
{
	if (!port_find())
		return false;
	int fd = open(m.path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct termios t;
	if (tcgetattr(fd, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(fd, TCSANOW, &t);
	}
	tcflush(fd, TCIOFLUSH);
	m.fd = fd;
	printf("phone-ui: modem: %s opened\n", m.path);
	modem_set_state(MODEM_STARTING);
	set_up();
	return true;
}

/* The SIM's state: ready, the network asked for; not, forgotten */
static void sim_set(enum modem_state state)
{
	bool was = modem_state() == MODEM_READY;
	modem_set_state(state);
	if (state != MODEM_READY) {
		network_forget();
		m.number_known = false;
	} else if (!was) {
		enqueue(creg);
		enqueue(cereg);
	}
}

/* The SIM's number, after what the network's state needs (its operator) */
static void number_ask(void)
{
	if (!m.number_known && modem_state() == MODEM_READY)
		enqueue(cnum);
}

/* AT+CGSN's answer: the IMEI, a line of digits ("+CGSN: " first on newer modems) */
static bool imei_line(const char *l)
{
	if (strncmp(l, "+CGSN: ", 7) == 0)
		l += 7;
	l += *l == '"';
	size_t n = strspn(l, "0123456789");
	if (n < 14 || n > 17 || (l[n] && l[n] != '"'))
		return false;
	char imei[18];
	snprintf(imei, sizeof(imei), "%.*s", (int)n, l);
	modem_set_imei(imei);
	return true;
}

/* +CNUM: [<alpha>],<number>,<type>[,...]: the SIM's own numbers, the first kept;
 * type 145 is international, its "+" sometimes left out */
static void number_line(const char *l)
{
	if (m.number_known)
		return;
	const char *p = l + 7;
	if (*p == '"') {
		p = strchr(p + 1, '"');
		p = p ? p + 1 : "";
	}
	const char *comma = strchr(p, ',');
	if (!comma || comma[1] != '"')
		return;
	const char *start = comma + 2, *end = strchr(start, '"');
	if (!end || end == start)
		return;
	int type = end[1] == ',' ? atoi(end + 2) : 0;
	char number[32];
	snprintf(number, sizeof(number), "%s%.*s", type == 145 && *start != '+' ? "+" : "", (int)(end - start),
	         start);
	m.number_known = true;
	modem_set_number(number);
}

/* The network's state from +CREG's and +CEREG's: registered, its operator asked for
 * (the name with it); not, once both are known */
static void registration(void)
{
	bool cs = m.cs == 1 || m.cs == 5, eps = m.eps == 1 || m.eps == 5;
	m.registered = cs || eps;
	if (m.registered) {
		signal_ask();
		return;
	}
	if (m.cs < 0 || m.eps < 0)
		return;
	number_ask();
	modem_set_network(m.cs == 2 || m.eps == 2 ? MODEM_NET_SEARCHING :
	                  m.cs == 3 || m.eps == 3 ? MODEM_NET_DENIED : MODEM_NET_NONE, NULL);
}

/* +CREG: / +CEREG: <stat> unsolicited, <n>,<stat>[,...] answering `query` */
static void registration_line(const char *l, int *stat, const char *query)
{
	const char *value = strchr(l, ':') + 1;
	const char *comma = strchr(value, ',');
	if (m.sent == query && comma)
		value = comma + 1;
	*stat = atoi(value);
	if (modem_state() == MODEM_READY)
		registration();
}

/* +COPS: <mode>[,<format>,"<operator>"[,<AcT>]] */
static void operator_line(const char *l)
{
	char name[64] = "";
	const char *start = strchr(l, '"'), *end = start ? strchr(start + 1, '"') : NULL;
	int act = -1;
	if (end) {
		snprintf(name, sizeof(name), "%.*s", (int)(end - start - 1), start + 1);
		if (end[1] == ',')
			act = atoi(end + 2);
	}
	if (!m.registered)
		return;
	modem_set_network(m.cs == 1 || m.eps == 1 ? MODEM_NET_HOME : MODEM_NET_ROAMING, name);
	m.tech = tech_of(act);
	modem_set_signal(m.bars, m.tech);
}

/* The signal, and with the operator its technology */
static void signal_ask(void)
{
	m.signal_at = lv_tick_get();
	enqueue(m.no_cesq ? csq : cesq);
	enqueue(cops);
	number_ask();
}

/* +CPIN: <code>, answered or unsolicited */
static void sim_code(const char *code)
{
	if (strcmp(code, "READY") == 0)
		sim_set(MODEM_READY);
	else if (strcmp(code, "NOT INSERTED") == 0)
		sim_set(MODEM_NO_SIM);
	else if (strstr(code, "PUK"))
		sim_set(MODEM_SIM_PUK);
	else if (strstr(code, "PIN"))
		sim_set(MODEM_SIM_PIN);
	else
		ask_sim_in(BUSY_AGAIN_MS); /* NOT READY: starting, or the card just went */
}

/* The final answer to what was sent: OK, or an error (+CME ERROR's code, -1 none) */
static void answered(bool ok, int error)
{
	const char *cmd = m.sent;
	m.sent = NULL;
	/* Not answered: a modem without LTE (+CEREG), or one with LTE only (+CREG) */
	if (!ok && (cmd == creg || cmd == cereg)) {
		*(cmd == creg ? &m.cs : &m.eps) = 0;
		if (modem_state() == MODEM_READY)
			registration();
	}
	if (!ok && cmd == cesq) {
		printf("phone-ui: modem: no AT+CESQ, AT+CSQ instead\n");
		m.no_cesq = true;
		enqueue(csq);
	}
	/* No +CNUM: the SIM does not hold its number. An error (busy): asked again with
	 * the signal */
	if (ok && cmd == cnum && !m.number_known) {
		m.number_known = true;
		modem_set_number(NULL);
	}
	data_answered(cmd, ok);
	if (cmd != cpin)
		return;
	if (!ok) {
		if (error == 10)
			sim_set(MODEM_NO_SIM);
		else if (error == 14 || error < 0)
			ask_sim_in(BUSY_AGAIN_MS); /* busy */
		else
			sim_set(MODEM_SIM_ERROR); /* 13 failure, 15 wrong... */
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
	} else if (strncmp(l, "+CREG: ", 7) == 0) {
		registration_line(l, &m.cs, creg);
	} else if (strncmp(l, "+CEREG: ", 8) == 0) {
		registration_line(l, &m.eps, cereg);
	} else if (strncmp(l, "+COPS: ", 7) == 0) {
		operator_line(l);
	} else if (strncmp(l, "+CESQ: ", 7) == 0) {
		cesq_line(l);
	} else if (strncmp(l, "+CNUM: ", 7) == 0) {
		number_line(l);
	} else if (m.sent == cgsn && imei_line(l)) {
		/* the IMEI, kept */
	} else if (strncmp(l, "$QCRMCALL: ", 11) == 0) {
		m.data_listed = true;
	} else if (strncmp(l, "+CSQ: ", 6) == 0) {
		csq_line(l);
	} else if (m.quectel && strncmp(l, "+QSIMSTAT: ", 11) == 0) {
		/* <enabled>,<inserted> */
		const char *inserted = strchr(l, ',');
		if (inserted && inserted[1] == '0') {
			sim_set(MODEM_NO_SIM);
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
	data_update(now);
	if (m.registered && modem_state() == MODEM_READY && lv_tick_diff(now, m.signal_at) >= SIGNAL_MS)
		signal_ask();
	if (!m.sent && m.queued) {
		char cmd[64];
		m.sent = m.queue[0];
		m.sent_at = now;
		m.data_listed = false;
		memmove(m.queue, m.queue + 1, --m.queued * sizeof(m.queue[0]));
		int len = snprintf(cmd, sizeof(cmd), "%s\r", m.sent);
		if (write(m.fd, cmd, (size_t)len) != len)
			printf("phone-ui: modem: cannot send %s\n", m.sent); /* not answered: set up again */
	}
}

static bool start(void)
{
	m.port = modem_config("port");
	const char *usb = modem_config("usb"), *interface = modem_config("interface");
	if (!m.port && (!usb || !interface || sscanf(usb, "%lx:%lx", &m.vendor, &m.product) != 2)) {
		printf("phone-ui: modem: the AT provider needs port=, or usb=<vendor>:<product> and interface=\n");
		return false;
	}
	m.interface = interface ? strtol(interface, NULL, 10) : -1;
	m.quectel = modem_config_has("quirks", "quectel");
	lv_timer_create(tick_cb, TICK_MS, NULL);
	return true;
}

const struct modem_provider modem_at = { .name = "at", .start = start };
