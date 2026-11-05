/*
 * The modem for the UI tests (ui_test.h, "Modem"): the Quectel EG25-G's AT port as a
 * pty, its slave linked at /tmp/modem (the port of ui_test_reset()'s modem.conf), the
 * commands answered from a thread as the modem does: echo on until ATE0, numeric
 * +CME ERROR codes (it answers so with AT+CMEE=1 only, but phone-ui sends that first),
 * +QSIMSTAT URCs once AT+QSIMSTAT=1 asked for them. The network: registration (circuit
 * switched +CREG, LTE +CEREG, reported once AT+CREG=1/AT+CEREG=1 asked for it) and the
 * operator (+COPS, its name once AT+COPS=3,0 asked for it, numeric before), registered
 * only with a ready SIM. Its signal: AT+CESQ (the technology's own measure: RSRP for
 * LTE, RSCP for UMTS, RxLev for GSM), AT+CSQ, the technology +COPS's <AcT>. Its
 * IMEI (AT+CGSN, a line of digits) and the SIM's own number (AT+CNUM, the EG25-G's
 * format: no name, the number, its type), with a ready SIM. Its data context 1's APN
 * (AT+CGDCONT, kept across restarts) is the network's at the attach only, as on LTE:
 * at power-up and on AT+CGATT=1 after AT+CGATT=0 (answered +CME ERROR: 30 as the
 * EG25-G does, attached all the same); empty, the network's own ("internet").
 * SMS in PDU mode: a store of its own (+CMTI once AT+CNMI asked for it, AT+CMGL=4,
 * AT+CMGD), AT+CMGS's "> " prompt then the PDU up to Ctrl-Z; its PDUs made and read
 * here, not by phone-ui's sms_pdu.c (GSM 7-bit for the characters it shares with
 * ASCII, UCS-2).
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
#include <time.h>
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
static char number[32] = UI_TEST_NUMBER;     /* the SIM's own, "" none stored */
static char context_apn[64];                 /* data context 1's, "" the network's */
static char attach_apn[64];                  /* the network's at the last attach */
static bool detached;                        /* AT+CGATT=0 */
static unsigned attaches;                    /* AT+CGATT=1 */

/* Attached: the APN the data sessions get */
static void attach(void)
{
	snprintf(attach_apn, sizeof(attach_apn), "%s", context_apn[0] ? context_apn : "internet");
	detached = false;
}

/* SMS */
#define STORE 32
static struct {
	bool used, read;
	char pdu[400];
} store[STORE];
static bool cnmi;                           /* +CMTI asked for */
static bool prompt;                         /* after AT+CMGS's "> ": the PDU, up to Ctrl-Z */
static unsigned sms_fails, sms_busy, sms_mr;
#define NV_SMS_DOMAIN "\"/nv/item_files/modem/mmode/sms_domain_pref\""
static unsigned sms_domain = 1;             /* NV: 0 circuit switched, 1 IMS */
static bool ims_refused, domain_fixed;
static unsigned restarts;                   /* by AT+CFUN=1,1 */
#define RESTART_MS 2000                     /* away that long, of the tests' time */
static uint64_t restarting;                 /* "RDY" then (CLOCK_MONOTONIC, ms), 0 not restarting */
static unsigned restart_commands;           /* received meanwhile: not answered */
#define SENT 32
static struct {
	char number[32];
	char text[2048];
	unsigned ref, total, got; /* parts: got so far */
} sent[SENT];
static unsigned sent_count, sent_done;      /* sent_done: whole messages, first ones */

/* The tests' time (CLOCK_MONOTONIC is wrapped: ui_test_wait()'s), in ms */
static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void say(const char *text);
static void fresh(void);
static void sayf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Characters GSM 7-bit has at ASCII's codes */
static bool gsm_ascii(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       (c && strchr(" !\"#%&'()*+,-./:;<=>?\n\r", c));
}

static void hex_put(char **at, const unsigned char *d, size_t n)
{
	for (size_t i = 0; i < n; i++)
		*at += sprintf(*at, "%02X", d[i]);
}

/* An SMS-DELIVER, with an empty service centre's address first (as AT+CMGL lists) */
static void deliver(char *pdu, const char *sender, const char *text, unsigned ref, unsigned total, unsigned seq)
{
	char *at = pdu;
	unsigned char head[] = { 0x00, total > 1 ? 0x44 : 0x04 };
	hex_put(&at, head, sizeof(head));

	/* The sender: digits, or a name in GSM 7-bit */
	unsigned char addr[24] = { 0 };
	size_t addr_size;
	if (sender[0] == '+' || (sender[0] >= '0' && sender[0] <= '9')) {
		unsigned digits = 0;
		for (const char *s = sender; *s; s++)
			if (*s >= '0' && *s <= '9') {
				addr[2 + digits / 2] |= digits % 2 ? (*s - '0') << 4 : *s - '0';
				digits++;
			}
		if (digits % 2)
			addr[2 + digits / 2] |= 0xf0;
		addr[0] = digits;
		addr[1] = sender[0] == '+' ? 0x91 : 0x81;
		addr_size = 2 + (digits + 1) / 2;
	} else {
		size_t n = strlen(sender);
		for (size_t i = 0, bit = 0; i < n; i++, bit += 7) {
			addr[2 + bit / 8] |= sender[i] << (bit % 8);
			if (bit % 8 > 1)
				addr[2 + bit / 8 + 1] |= sender[i] >> (8 - bit % 8);
		}
		addr[0] = (n * 7 + 3) / 4;
		addr[1] = 0xd0;
		addr_size = 2 + (n * 7 + 7) / 8;
	}
	hex_put(&at, addr, addr_size);

	bool ucs2 = false;
	for (const char *s = text; *s; s++)
		ucs2 |= !gsm_ascii(*s);
	/* The time stamp: now, in UTC (semi-octets swapped) */
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	int v[] = { tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0 };
	unsigned char tail[2 + 7];
	tail[0] = 0x00;
	tail[1] = ucs2 ? 0x08 : 0x00;
	for (int i = 0; i < 7; i++)
		tail[2 + i] = (v[i] % 10) << 4 | v[i] / 10;
	hex_put(&at, tail, sizeof(tail));

	unsigned char ud[160] = { 0 };
	size_t used = 0;
	if (total > 1) {
		unsigned char udh[] = { 5, 0, 3, ref, total, seq };
		memcpy(ud, udh, sizeof(udh));
		used = sizeof(udh);
	}
	unsigned length;
	if (ucs2) {
		/* UTF-8 to UTF-16 */
		for (const unsigned char *s = (const unsigned char *)text; *s && used + 4 <= sizeof(ud);) {
			unsigned c = *s, n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
			c &= n ? 0x3f >> n : 0x7f;
			for (unsigned k = 1; k <= n; k++)
				c = c << 6 | (s[k] & 0x3f);
			s += n + 1;
			if (c >= 0x10000) {
				unsigned hi = 0xd800 + ((c - 0x10000) >> 10), lo = 0xdc00 + ((c - 0x10000) & 0x3ff);
				ud[used++] = hi >> 8, ud[used++] = hi & 0xff;
				c = lo;
			}
			ud[used++] = c >> 8, ud[used++] = c & 0xff;
		}
		length = used;
	} else {
		size_t first = (used * 8 + 6) / 7, bit = first * 7, n = strlen(text);
		for (size_t i = 0; i < n; i++, bit += 7) {
			ud[bit / 8] |= text[i] << (bit % 8);
			if (bit % 8 > 1)
				ud[bit / 8 + 1] |= text[i] >> (8 - bit % 8);
		}
		length = first + n;
		used = (bit + 7) / 8;
	}
	unsigned char udl = length;
	hex_put(&at, &udl, 1);
	hex_put(&at, ud, used);
}

static int unhex(const char *hex, unsigned char *out, size_t max)
{
	size_t n = strlen(hex) / 2;
	if (n > max)
		return -1;
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			return -1;
		out[i] = v;
	}
	return n;
}

/* An SMS-SUBMIT phone-ui sent: kept as a part of its message, joined in order */
static bool submit(const char *pdu)
{
	unsigned char d[200];
	int n = unhex(pdu, d, sizeof(d));
	if (n < 8)
		return false;
	size_t i = 1 + d[0];
	unsigned char first = d[i++];
	i++; /* its reference */
	unsigned digits = d[i++];
	char to[32];
	size_t t = 0;
	if (d[i] == 0x91)
		to[t++] = '+';
	for (unsigned k = 0; k < digits; k++)
		to[t++] = '0' + (k % 2 ? d[i + 1 + k / 2] >> 4 : d[i + 1 + k / 2] & 0x0f);
	to[t] = '\0';
	i += 1 + (digits + 1) / 2;
	i++; /* protocol */
	bool ucs2 = d[i++] == 0x08;
	if ((first & 0x18) == 0x10)
		i++; /* relative validity */
	unsigned length = d[i++];
	const unsigned char *ud = &d[i];

	unsigned ref = 0, total = 1, seq = 1;
	size_t header = 0;
	if (first & 0x40) {
		header = ud[0] + 1;
		ref = ud[3], total = ud[4], seq = ud[5];
	}
	char text[1024];
	size_t at = 0;
	if (ucs2) {
		for (size_t k = header; k + 1 < length && at + 5 < sizeof(text); k += 2) {
			unsigned c = ud[k] << 8 | ud[k + 1];
			if (c >= 0xd800 && c < 0xdc00 && k + 3 < length) {
				c = 0x10000 + ((c - 0xd800) << 10) + ((ud[k + 2] << 8 | ud[k + 3]) - 0xdc00);
				k += 2;
			}
			if (c < 0x80) {
				text[at++] = c;
			} else if (c < 0x800) {
				text[at++] = 0xc0 | c >> 6, text[at++] = 0x80 | (c & 0x3f);
			} else if (c < 0x10000) {
				text[at++] = 0xe0 | c >> 12, text[at++] = 0x80 | (c >> 6 & 0x3f), text[at++] = 0x80 | (c & 0x3f);
			} else {
				text[at++] = 0xf0 | c >> 18, text[at++] = 0x80 | (c >> 12 & 0x3f);
				text[at++] = 0x80 | (c >> 6 & 0x3f), text[at++] = 0x80 | (c & 0x3f);
			}
		}
	} else {
		for (size_t k = (header * 8 + 6) / 7; k < length && at + 1 < sizeof(text); k++) {
			size_t bit = k * 7;
			unsigned s = (ud[bit / 8] >> (bit % 8) | (bit % 8 > 1 ? ud[bit / 8 + 1] << (8 - bit % 8) : 0)) & 0x7f;
			text[at++] = gsm_ascii(s) ? s : s == 0 ? '@' : '?';
		}
	}
	text[at] = '\0';

	/* The message it belongs to: a part after the first goes to one waiting for it */
	unsigned m = sent_count;
	if (seq > 1)
		for (m = sent_done; m < sent_count; m++)
			if (sent[m].ref == ref && sent[m].total == total && sent[m].got == seq - 1)
				break;
	if (m == sent_count) {
		if (seq != 1 || sent_count == SENT)
			return false;
		sent_count++;
		snprintf(sent[m].number, sizeof(sent[m].number), "%s", to);
		sent[m].text[0] = '\0';
		sent[m].ref = ref;
		sent[m].total = total;
		sent[m].got = 0;
	}
	strncat(sent[m].text, text, sizeof(sent[m].text) - strlen(sent[m].text) - 1);
	sent[m].got++;
	while (sent_done < sent_count && sent[sent_done].got == sent[sent_done].total)
		sent_done++;
	return true;
}

static void pdu_answer(const char *pdu)
{
	bool lte = tech == UI_TEST_TECH_LTE || tech == UI_TEST_TECH_NR_NSA;
	if (sms_fails) {
		sms_fails--;
		say("\r\n+CMS ERROR: 500\r\n");
	} else if (lte && sms_domain == 1 && ims_refused) {
		say("\r\n+CMS ERROR: 350\r\n"); /* the network's IMS refused it */
	} else if (!submit(pdu)) {
		say("\r\n+CMS ERROR: 304\r\n"); /* invalid PDU */
	} else {
		sayf("\r\n+CMGS: %u\r\n\r\nOK\r\n", sms_mr++ % 256);
	}
}

/* Stored; +CMTI if asked for */
static void receive(const char *sender, const char *text, unsigned ref, unsigned total, unsigned seq)
{
	for (int i = 0; i < STORE; i++) {
		if (store[i].used)
			continue;
		store[i].used = true;
		store[i].read = false;
		deliver(store[i].pdu, sender, text, ref, total, seq);
		if (plugged && cnmi)
			sayf("\r\n+CMTI: \"ME\",%d\r\n", i);
		return;
	}
	fprintf(stderr, "fake_modem: SMS store full\n");
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
	if (restarting) {
		restart_commands++;
		return;
	}
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
	} else if (strcasecmp(cmd, "AT+CGSN") == 0) {
		say("\r\n" UI_TEST_IMEI "\r\n");
	} else if (strcasecmp(cmd, "AT+CNUM") == 0) {
		if (sim != UI_TEST_SIM_READY) {
			say(sim == UI_TEST_SIM_ABSENT ? "\r\n+CME ERROR: 10\r\n" : "\r\n+CME ERROR: 11\r\n");
			return;
		}
		if (number[0])
			sayf("\r\n+CNUM: ,\"%s\",%d\r\n", number, number[0] == '+' ? 145 : 129);
	} else if (strcasecmp(cmd, "AT+CGDCONT?") == 0) {
		sayf("\r\n+CGDCONT: 1,\"IPV4V6\",\"%s\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0,0\r\n",
		     context_apn);
		say("\r\n+CGDCONT: 2,\"IPV4V6\",\"ims\",\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0,0\r\n");
	} else if (strncasecmp(cmd, "AT+CGDCONT=1,", 13) == 0) {
		/* AT+CGDCONT=1,"<type>","<apn>" */
		const char *apn = strchr(cmd + 13, ',');
		const char *end = apn && apn[1] == '"' ? strchr(apn + 2, '"') : NULL;
		if (!end) {
			say("\r\nERROR\r\n");
			return;
		}
		snprintf(context_apn, sizeof(context_apn), "%.*s", (int)(end - apn - 2), apn + 2);
	} else if (strcasecmp(cmd, "AT+CGATT=0") == 0) {
		detached = true;
		data = false;
	} else if (strcasecmp(cmd, "AT+CGATT=1") == 0) {
		attach();
		attaches++;
		say("\r\n+CME ERROR: 30\r\n"); /* the EG25-G's, attached all the same */
		return;
	} else if (strcasecmp(cmd, "AT$QCRMCALL=1,1") == 0) {
		/* The data session on the network interface: registered, attached, and not failing */
		if (data_fails || detached || (!registered(state(true)) && !registered(state(false)))) {
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
	} else if (strcasecmp(cmd, "AT+QNVFR=" NV_SMS_DOMAIN) == 0) {
		sayf("\r\n+QNVFR: %02u\r\n", sms_domain);
	} else if (strncasecmp(cmd, "AT+QNVFW=" NV_SMS_DOMAIN ",", strlen("AT+QNVFW=" NV_SMS_DOMAIN ",")) == 0) {
		unsigned v = strtoul(cmd + strlen("AT+QNVFW=" NV_SMS_DOMAIN ","), NULL, 16);
		if (!domain_fixed)
			sms_domain = v;
	} else if (strcasecmp(cmd, "AT+CFUN=1,1") == 0) {
		/* Restarts: OK, then nothing answered for a while (a real one goes on a few
		 * seconds, then leaves USB), then as after power-up: "RDY" */
		say("\r\nOK\r\n");
		fresh();
		restarts++;
		restarting = now_ms() + RESTART_MS;
		return;
	} else if (strcasecmp(cmd, "AT+CMGF=0") == 0) {
		/* PDU mode, once SMS is ready */
		if (sms_busy) {
			sms_busy--;
			say("\r\n+CMS ERROR: 314\r\n"); /* SIM busy */
			return;
		}
	} else if (strncasecmp(cmd, "AT+QURCCFG=", 11) == 0) {
		/* the URCs' port: this one */
	} else if (strncasecmp(cmd, "AT+CNMI=2,1", 11) == 0) {
		cnmi = true;
	} else if (strcasecmp(cmd, "AT+CMGL=4") == 0) {
		if (sim != UI_TEST_SIM_READY) {
			say("\r\n+CMS ERROR: 310\r\n");
			return;
		}
		for (int i = 0; i < STORE; i++) {
			if (!store[i].used)
				continue;
			sayf("\r\n+CMGL: %d,%d,,%zu\r\n", i, store[i].read, strlen(store[i].pdu) / 2 - 1);
			say(store[i].pdu);
			store[i].read = true;
		}
		say("\r\n");
	} else if (strncasecmp(cmd, "AT+CMGD=", 8) == 0) {
		int i = atoi(cmd + 8);
		if (i < 0 || i >= STORE || !store[i].used) {
			say("\r\n+CMS ERROR: 321\r\n"); /* invalid index */
			return;
		}
		store[i].used = false;
	} else if (strncasecmp(cmd, "AT+CMGS=", 8) == 0) {
		if (sim != UI_TEST_SIM_READY) {
			say("\r\n+CMS ERROR: 310\r\n");
			return;
		}
		if (!registered(state(true)) && !registered(state(false))) {
			say("\r\n+CMS ERROR: 331\r\n"); /* no network service */
			return;
		}
		prompt = true;
		say("\r\n> ");
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
	char line[512];
	size_t len = 0;
	for (;;) {
		struct pollfd p = { .fd = master, .events = POLLIN };
		int ready = poll(&p, 1, 10);
		pthread_mutex_lock(&lock);
		if (stop) {
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		if (restarting && now_ms() >= restarting) {
			restarting = 0;
			say("\r\nRDY\r\n");
		}
		char buf[256];
		ssize_t n = ready > 0 ? read(master, buf, sizeof(buf)) : 0;
		for (ssize_t i = 0; i < n; i++) {
			if (prompt && buf[i] == 0x1a) { /* Ctrl-Z: the PDU is complete */
				line[len] = '\0';
				prompt = false;
				pdu_answer(line);
				len = 0;
			} else if (prompt) {
				if (buf[i] != '\r' && buf[i] != '\n' && len + 1 < sizeof(line))
					line[len++] = buf[i];
			} else if (buf[i] == '\r') {
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
	simstat = creg = cereg = names = data = cnmi = prompt = false;
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
	attach(); /* powered up: with context 1's APN */
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
	attach();
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

void ui_test_modem_number(const char *n)
{
	pthread_mutex_lock(&lock);
	snprintf(number, sizeof(number), "%s", n ? n : "");
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_sms(const char *sender, const char *text)
{
	ui_test_modem_sms_part(sender, text, 0, 1, 1);
}

void ui_test_modem_sms_part(const char *sender, const char *text, unsigned ref, unsigned total, unsigned seq)
{
	pthread_mutex_lock(&lock);
	receive(sender, text, ref, total, seq);
	pthread_mutex_unlock(&lock);
}

unsigned ui_test_modem_sms_stored(void)
{
	unsigned n = 0;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < STORE; i++)
		n += store[i].used;
	pthread_mutex_unlock(&lock);
	return n;
}

const char *ui_test_modem_sms_sent(unsigned i, const char **to)
{
	pthread_mutex_lock(&lock);
	const char *text = i < sent_done ? sent[i].text : NULL;
	if (text && to)
		*to = sent[i].number;
	pthread_mutex_unlock(&lock);
	return text;
}

void ui_test_modem_sms_fails(unsigned count)
{
	pthread_mutex_lock(&lock);
	sms_fails = count;
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_sms_busy(unsigned count)
{
	pthread_mutex_lock(&lock);
	sms_busy = count;
	pthread_mutex_unlock(&lock);
}

void ui_test_modem_context_apn(const char *apn)
{
	pthread_mutex_lock(&lock);
	snprintf(context_apn, sizeof(context_apn), "%s", apn ? apn : "");
	pthread_mutex_unlock(&lock);
}

const char *ui_test_modem_apn(unsigned *reattached)
{
	static char apn[64];
	pthread_mutex_lock(&lock);
	snprintf(apn, sizeof(apn), "%s", detached ? "" : attach_apn);
	if (reattached)
		*reattached = attaches;
	pthread_mutex_unlock(&lock);
	return apn;
}

void ui_test_modem_sms_domain(unsigned domain, bool refused, bool fixed)
{
	pthread_mutex_lock(&lock);
	sms_domain = domain;
	ims_refused = refused;
	domain_fixed = fixed;
	pthread_mutex_unlock(&lock);
}

unsigned ui_test_modem_sms_domain_now(unsigned *count)
{
	pthread_mutex_lock(&lock);
	unsigned v = sms_domain;
	if (count)
		*count = restarts;
	pthread_mutex_unlock(&lock);
	return v;
}

unsigned ui_test_modem_restart_commands(void)
{
	pthread_mutex_lock(&lock);
	unsigned n = restart_commands;
	pthread_mutex_unlock(&lock);
	return n;
}
