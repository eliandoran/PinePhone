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
 * The APN (modem.c's, "" the network's): data context 1's, the one AT$QCRMCALL=1,1
 * uses, read once registered (AT+CGDCONT?: the modem keeps it); another one set
 * (AT+CGDCONT=1,...) then attached again with it (AT+CGATT=0, =1: on LTE the session
 * is the bearer of the attach, made with the APN of then; the EG25-G answers =1 with
 * an error and attaches all the same), a session up ended first, started again after.
 *
 * Its IMEI asked as it is set up (AT+CGSN), the SIM's own number once the network's
 * state is known (AT+CNUM: what the SIM holds, often nothing; asked again with the
 * signal while the SIM was busy).
 *
 * SMS in PDU mode (3GPP TS 27.005, sms_pdu.c), once the SIM is ready: new ones stored
 * by the modem and reported (AT+CNMI=2,1: +CMTI), then all it stores listed
 * (AT+CMGL=4, also at the start: those that came while phone-ui was not running) and
 * each kept message deleted from it (AT+CMGD) once the conversations saved it. The
 * parts of a long one stay on the modem until all are there (or a day went by: what
 * came is kept). Sent with AT+CMGS=<length>, the PDU after its "> " prompt, one part
 * at a time; refused (+CMS ERROR), the message is not sent. Set up once the network's
 * state is known (after what the status bar shows); not ready yet (busy just after
 * the SIM): asked again a little later.
 *
 * USSD (27.007 7.15): a code or an answer sent with AT+CUSD=1 in UCS-2, AT+CSCS="UCS2"
 * on the same line (the reply comes in it too, whatever the network's alphabet), the
 * character set back to "GSM" once the reply came (the operator's name in +COPS follows
 * it). The reply comes later, +CUSD (in "GSM", its text over several lines: the modem
 * writes its line ends), or an error after the OK (+CME ERROR: 258 for a code Digi
 * refused): nothing else is sent while it is awaited, as that error would be taken as
 * another command's; up to USSD_MS, then the session is ended (AT+CUSD=2), as it is
 * when phone-ui leaves it (no +CUSD then). A +CUSD unasked is what the network sends.
 *
 * quirks= in modem.conf, for what is not standard:
 *	quectel  the card going or coming reported (AT+QSIMSTAT=1, +QSIMSTAT): a ready
 *	         SIM is not asked for again; "RDY" when the modem restarted (set up again);
 *	         mobile data (AT$QCRMCALL); the reports (+CMTI) on this port
 *	         (AT+QURCCFG), "+QIND: SMS DONE" when SMS is ready
 *
 * boot_wait=<seconds> in modem.conf: the board powers the modem at boot and it is on USB
 * by then (seconds since boot, CLOCK_BOOTTIME): until then its port missing is "Starting",
 * not "No modem" (the EG25-G takes about 12 s to show up).
 *
 * Settings the modem keeps, in modem.conf (Quectel's):
 *	sms_domain  where SMS go (its NV item sms_domain_pref, AT+QNVFR and AT+QNVFW):
 *	            "cs" circuit switched (on LTE through SGs), "ims" over IMS. A network may
 *	            refuse one of them (+CMS ERROR: 350): Digi's refuses IMS from modems it
 *	            did not approve
 *	ims         IMS (AT+QCFG="ims"), "on" or "off": off, calls and USSD go circuit
 *	            switched, falling back from LTE to 2G/3G (Digi connects this modem to
 *	            IMS, then releases its VoLTE calls at once)
 * Read as the modem is set up; another value is written and the modem restarted for
 * them to take (AT+CFUN=1,1, once for all), then read again: once a run, one that did
 * not take is logged, not written again. Unset, the modem's own is left. Restarting, it
 * goes on answering a few seconds, then leaves USB: nothing is sent meanwhile, nothing
 * it says counts but "RDY", "Starting" shown until it is back.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "modem_provider.h"
#include "sms_pdu.h"
#include "ui.h"

#define TICK_MS 200
#define OPEN_EVERY_MS 1000
#define ANSWER_MS 5000     /* then it is not answering: set up again */
#define SIM_AGAIN_MS 10000 /* while the SIM is not ready, or without reports */
#define BUSY_AGAIN_MS 1000 /* SIM busy, or not ready yet */
#define SIGNAL_MS 10000    /* the signal and technology asked again, registered */
#define DATA_CHECK_MS 30000 /* the data session asked about */
#define DATA_RETRY_MS 5000  /* after a refused session */
#define SMS_RETRY_MS 5000  /* SMS not ready yet (just after the SIM) */
#define RESTART_MS 30000   /* AT+CFUN=1,1: back by then ("RDY", or gone and back on USB) */
#define SMS_SEND_MS 60000  /* AT+CMGS: the network's answer */
#define SMS_LIST_MS 20000  /* AT+CMGL: all it stores */
#define SMS_PARTS_DAYS 1   /* the parts of a long one waited for */
#define SMS_DELETES 64
#define SMS_OUT 16         /* messages waiting to be sent */
#define QUEUE 24
#define USSD_MS 30000     /* a USSD reply awaited, then the session ended */
#define USSD_LINES 16     /* of a +CUSD text in "GSM" (the modem writes its line ends) */
#define CALL_LIST_MS 1000 /* AT+CLCC asked again while there is a call */

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
static const char sms_domain_read[] = "AT+QNVFR=\"/nv/item_files/modem/mmode/sms_domain_pref\"";
/* ,<value> added */
static const char sms_domain_write[] = "AT+QNVFW=\"/nv/item_files/modem/mmode/sms_domain_pref\"";
static const char ims_read[] = "AT+QCFG=\"ims\"";
static const char ims_write[] = "AT+QCFG=\"ims\""; /* ,<value> added */
static const char restart[] = "AT+CFUN=1,1";
static const char apn_query[] = "AT+CGDCONT?";
static const char apn_set[] = "AT+CGDCONT=1"; /* ,"IPV4V6","<apn>" added */
static const char detach[] = "AT+CGATT=0";
static const char attach[] = "AT+CGATT=1";
static const char urc_port[] = "AT+QURCCFG=\"urcport\",\"usbat\"";
static const char sms_mode[] = "AT+CMGF=0";
static const char sms_reports[] = "AT+CNMI=2,1,0,0,0";
static const char sms_list[] = "AT+CMGL=4";
/* With an argument when sent: the first index to delete, the part being sent's length */
static const char sms_delete[] = "AT+CMGD";
static const char sms_send[] = "AT+CMGS";
/* USSD: the character set for the reply on the same line; ,"<code>",15 added */
static const char cusd_send[] = "AT+CSCS=\"UCS2\";+CUSD=1";
static const char cusd_end[] = "AT+CUSD=2";
static const char cscs_gsm[] = "AT+CSCS=\"GSM\"";
/* Calls: ATD's number and ";" added, AT+CMUT's =<value>, AT+VTS's =<key> (one each:
 * the tone lasts AT+VTD's, 300 ms on the EG25-G) */
static const char call_dial[] = "ATD";
static const char call_answer[] = "ATA";
static const char call_hang_up[] = "AT+CHUP";
static const char call_mute[] = "AT+CMUT";
static const char call_tone[] = "AT+VTS";
static const char clcc[] = "AT+CLCC";
static const char clip_on[] = "AT+CLIP=1";

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
	char apn[64];           /* data context 1's, as read or set */
	bool apn_known;         /* read */
	bool apn_changing;      /* set, then attached again: no session meanwhile */
	char apn_new[64];       /* what is being set */
	uint32_t open_at;       /* last attempt, 0 none */
	long boot_wait;         /* modem.conf's, 0 none */
	const char *queue[QUEUE];
	unsigned queued;
	const char *sent;       /* waiting for its answer */
	uint32_t sent_at;
	bool ask;               /* AT+CPIN? at ask_at */
	uint32_t ask_at;
	bool restarting;        /* AT+CFUN=1,1 answered: until "RDY", gone, or RESTART_MS */
	uint32_t restart_at;
	char line[1200];       /* a USSD reply in hexadecimal UCS-2: 4 digits a character */
	size_t len;
} m = { .fd = -1, .cs = -1, .eps = -1, .bars = -1 };

static void signal_ask(void);
static void network_forget(void);
static void data_forget(void);
static void enqueue(const char *cmd);
static void enqueue_first(const char *cmd);
static void ussd_forget(void);
static void call_forget(void);

/* SMS */

struct listed {
	int index;
	bool text;               /* decoded: an SMS-DELIVER with text */
	bool done;
	struct sms_deliver d;
};

struct outgoing {
	unsigned id;
	unsigned part;           /* being sent */
	struct sms_submit s;
};

static struct {
	bool started;            /* set up for this SIM, or being */
	bool pdu;                /* AT+CMGF=0 answered OK: listed and sent only then (in
	                          * text mode, a listing would not decode and be deleted) */
	bool list_again;         /* +CMTI while listing */
	bool retry;              /* set up again at retry_at */
	uint32_t retry_at;
	int pdu_of;              /* +CMGL's index: its PDU is the next line; -1 none */
	struct listed *listed;
	unsigned listed_count, listed_size;
	int deleting[SMS_DELETES];
	unsigned deletes;
	struct outgoing *out[SMS_OUT];
	unsigned outs;
	bool prompted;           /* the PDU written after AT+CMGS's prompt */
	unsigned ref;            /* of the parts of the next long one */
} sms = { .pdu_of = -1 };

static void sms_set_up(void)
{
	if (m.quectel)
		enqueue(urc_port);
	enqueue(sms_mode);
	enqueue(sms_reports);
	enqueue(sms_list);
}

/* Once the network's state is known with a ready SIM */
static void sms_start(void)
{
	if (sms.started || modem_state() != MODEM_READY)
		return;
	sms.started = true;
	sms_set_up();
	/* After what the status bar and SMS need: USSD's character set, should an earlier
	 * phone-ui have left it in UCS-2 (before USSD can be sent: it queues after), and
	 * the caller's number with RING (AT+CLCC lists it too) */
	enqueue(cscs_gsm);
	enqueue(clip_on);
}

/* What the modem stores, listed again (+CMTI) */
static void sms_list_ask(void)
{
	if (m.sent == sms_list)
		sms.list_again = true;
	else
		enqueue(sms_list);
}

/* What is being sent fails, nothing is pending: the modem went, restarted, or its SIM */
static void sms_forget(void)
{
	for (unsigned i = 0; i < sms.outs; i++) {
		modem_sms_sent(sms.out[i]->id, false);
		free(sms.out[i]);
	}
	sms.outs = 0;
	sms.deletes = 0;
	sms.listed_count = 0;
	sms.pdu_of = -1;
	sms.started = sms.pdu = sms.list_again = sms.retry = sms.prompted = false;
}

static bool sms_deleting(int index)
{
	for (unsigned i = 0; i < sms.deletes; i++)
		if (sms.deleting[i] == index)
			return true;
	return false;
}

static void sms_delete_ask(int index)
{
	if (sms_deleting(index) || sms.deletes == SMS_DELETES)
		return;
	sms.deleting[sms.deletes++] = index;
	enqueue(sms_delete);
}

/* A PDU AT+CMGL listed: decoded, unless it is being deleted (kept already) */
static void sms_pdu_line(int index, const char *pdu)
{
	if (sms_deleting(index))
		return;
	if (sms.listed_count == sms.listed_size) {
		sms.listed_size = sms.listed_size ? sms.listed_size * 2 : 16;
		sms.listed = realloc(sms.listed, sms.listed_size * sizeof(*sms.listed));
	}
	struct listed *l = &sms.listed[sms.listed_count++];
	l->index = index;
	l->done = false;
	l->text = sms_deliver_decode(pdu, &l->d);
}

/* The parts of the message of l (with it) that were listed, in `parts` (NULL for a
 * part not there, an index by seq - 1) */
static void sms_parts_of(const struct listed *l, struct listed **parts)
{
	for (unsigned i = 0; i < l->d.total; i++)
		parts[i] = NULL;
	for (unsigned i = 0; i < sms.listed_count; i++) {
		struct listed *p = &sms.listed[i];
		if (!p->done && p->text && p->d.total == l->d.total && p->d.ref == l->d.ref &&
		    strcmp(p->d.sender, l->d.sender) == 0 && !parts[p->d.seq - 1])
			parts[p->d.seq - 1] = p;
	}
}

/* AT+CMGL answered: what came whole given to the conversations, deleted once kept */
static void sms_listed(void)
{
	time_t now = time(NULL);
	for (unsigned i = 0; i < sms.listed_count; i++) {
		struct listed *l = &sms.listed[i];
		if (l->done)
			continue;
		if (!l->text) {
			printf("phone-ui: modem: SMS %d is not a text, deleted\n", l->index);
			l->done = true;
			sms_delete_ask(l->index);
			continue;
		}

		struct listed *parts[256];
		sms_parts_of(l, parts);
		unsigned got = 0;
		size_t size = 1;
		time_t first = l->d.time, last = l->d.time;
		for (unsigned k = 0; k < l->d.total; k++) {
			if (!parts[k])
				continue;
			got++;
			size += strlen(parts[k]->d.text);
			first = parts[k]->d.time < first ? parts[k]->d.time : first;
			last = parts[k]->d.time > last ? parts[k]->d.time : last;
		}
		if (got < l->d.total && now - last < SMS_PARTS_DAYS * 86400)
			continue; /* waiting for the others, kept on the modem */

		char *text = malloc(size);
		text[0] = '\0';
		for (unsigned k = 0; k < l->d.total; k++)
			if (parts[k])
				strcat(text, parts[k]->d.text);
		bool kept = modem_sms_received(l->d.sender, first, text);
		free(text);
		/* Its parts, and for a long one others with the same reference (a part sent
		 * twice); a whole one is only itself (they all have reference 0) */
		for (unsigned k = 0; k < sms.listed_count; k++) {
			struct listed *p = &sms.listed[k];
			if (l->d.total == 1 ? p != l :
			    p->done || !p->text || p->d.total != l->d.total || p->d.ref != l->d.ref ||
			    strcmp(p->d.sender, l->d.sender) != 0)
				continue;
			p->done = true;
			if (kept)
				sms_delete_ask(p->index);
		}
	}
	sms.listed_count = 0;
}

/* Settings the modem keeps (sms_domain=, ims=): read, written if they differ, the modem
 * restarted once for them all, read again */
static struct setting {
	const char *key;           /* modem.conf's */
	const char *read, *write;
	const char *answer;        /* the read's line, its value after it */
	int base;                  /* of the value read */
	const char *format;        /* of the value written */
	const char *const *words;  /* modem.conf's, by value */
	int values;
	int wanted;                /* -1: the modem's left */
	int got;                   /* read, -1 not */
	enum { SETTING_READ, SETTING_CHECK, SETTING_DONE } step; /* CHECK: written, read again
	                            * after the restart; DONE: as wanted, or given up */
} settings[] = {
	{ "sms_domain", sms_domain_read, sms_domain_write, "+QNVFR: ", 16, "%02X",
	  (const char *const[]){ "cs", "ims" }, 2, -1, -1, SETTING_READ },
	{ "ims", ims_read, ims_write, "+QCFG: \"ims\",", 10, "%d",
	  (const char *const[]){ NULL, "on", "off" }, 3, -1, -1, SETTING_READ },
};
#define SETTINGS (sizeof(settings) / sizeof(settings[0]))

/* The answer to a setting's read: its value kept, true; false if it is none */
static bool setting_line(const char *l)
{
	for (unsigned i = 0; i < SETTINGS; i++) {
		struct setting *s = &settings[i];
		size_t n = strlen(s->answer);
		if (m.sent == s->read && strncmp(l, s->answer, n) == 0) {
			s->got = (int)strtol(l + n, NULL, s->base);
			return true;
		}
	}
	return false;
}

static void settings_answered(const char *cmd, bool ok)
{
	for (unsigned i = 0; i < SETTINGS; i++) {
		struct setting *s = &settings[i];
		const char *wanted = s->wanted >= 0 ? s->words[s->wanted] : "";
		if (cmd == s->read && ok && s->got >= 0) {
			if (s->got == s->wanted) {
				s->step = SETTING_DONE;
			} else if (s->step == SETTING_CHECK) {
				printf("phone-ui: modem: %s still not %s after a restart\n", s->key, wanted);
				s->step = SETTING_DONE; /* once a run */
			} else {
				s->step = SETTING_CHECK;
				enqueue(s->write);
			}
		} else if (cmd == s->read) {
			printf("phone-ui: modem: %s not read\n", s->key);
			s->step = SETTING_DONE;
		} else if (cmd == s->write && ok) {
			/* After the other writes queued: one restart for all */
			printf("phone-ui: modem: %s set to %s, restarting the modem\n", s->key, wanted);
			enqueue(restart);
		} else if (cmd == s->write) {
			printf("phone-ui: modem: %s not written\n", s->key);
			s->step = SETTING_DONE;
		}
		if (cmd == s->read)
			s->got = -1;
	}
	if (cmd == restart && ok) {
		m.restarting = true;
		m.restart_at = lv_tick_get();
		m.queued = 0;
		network_forget();
		data_forget();
		sms_forget();
		ussd_forget();
		call_forget();
		modem_set_state(MODEM_STARTING);
	}
}

/* The final answer to an SMS command: OK, or an error (+CMS ERROR's code, -1 none) */
static void sms_answered(const char *cmd, bool ok, int error)
{
	if (!ok && (cmd == sms_mode || cmd == sms_reports || cmd == sms_list || cmd == sms_delete ||
	            cmd == sms_send))
		printf("phone-ui: modem: %s refused (%d)\n", cmd, error); /* +CMS ERROR (27.005 3.2.5) */
	if (!ok && (cmd == sms_mode || cmd == sms_reports || cmd == sms_list) && !sms.retry &&
	    modem_state() == MODEM_READY) {
		sms.retry = true;
		sms.retry_at = lv_tick_get() + SMS_RETRY_MS;
	}
	if (cmd == sms_mode)
		sms.pdu = ok;
	if (cmd == sms_list) {
		if (ok)
			sms_listed();
		sms.listed_count = 0;
		sms.pdu_of = -1;
		if (sms.list_again) {
			sms.list_again = false;
			enqueue(sms_list);
		}
	} else if (cmd == sms_delete && sms.deletes) {
		memmove(sms.deleting, sms.deleting + 1, --sms.deletes * sizeof(sms.deleting[0]));
		if (sms.deletes)
			enqueue(sms_delete);
	} else if (cmd == sms_send && sms.outs) {
		struct outgoing *o = sms.out[0];
		sms.prompted = false;
		if (ok && ++o->part < o->s.count) {
			enqueue(sms_send);
			return;
		}
		modem_sms_sent(o->id, ok);
		free(o);
		memmove(sms.out, sms.out + 1, --sms.outs * sizeof(sms.out[0]));
		if (sms.outs)
			enqueue(sms_send);
	}
}

/* The provider's sms_send: queued, in parts */
static bool sms_send_text(unsigned id, const char *number, const char *text)
{
	if (m.fd < 0 || !sms.pdu || sms.outs == SMS_OUT)
		return false;
	struct outgoing *o = malloc(sizeof(*o));
	if (!sms_submit_encode(number, text, sms.ref++ & 0xff, &o->s)) {
		free(o);
		return false;
	}
	o->id = id;
	o->part = 0;
	sms.out[sms.outs++] = o;
	enqueue(sms_send);
	return true;
}

/* USSD */

static struct {
	char hex[260];           /* what to send, in UCS-2 */
	bool pending;            /* queued, or sent and not answered yet */
	bool cancelled;          /* while sent: ended once it is answered */
	bool waiting;            /* answered OK: the reply awaited, nothing else sent */
	uint32_t sent_at;
	bool open;               /* the network wants an answer */
	bool ucs2;               /* AT+CSCS="UCS2" (with AT+CUSD=1): "GSM" again first */
	bool ignore;             /* ended while awaited: its reply, if it comes, until ignore_at + USSD_MS */
	uint32_t ignore_at;
	bool joining;            /* a +CUSD's text over several lines: up to its closing quote */
	unsigned lines;
	char reply[1600];
} ussd;

/* Ended by phone-ui: the character set back first, the reply that may still come not shown */
static void ussd_end_session(void)
{
	ussd.waiting = ussd.open = false;
	ussd.ignore = true;
	ussd.ignore_at = lv_tick_get();
	if (ussd.ucs2)
		enqueue_first(cscs_gsm);
	enqueue_first(cusd_end);
}

/* A reply, or why there is none: the character set back, the session open if it wants
 * an answer */
static void ussd_result(enum modem_ussd state, const char *text)
{
	ussd.waiting = false;
	ussd.open = state == MODEM_USSD_MORE;
	if (ussd.ucs2)
		enqueue_first(cscs_gsm);
	modem_ussd_received(state, text);
}

/* The modem went, restarted, or its SIM: what was being sent fails */
static void ussd_forget(void)
{
	bool lost = ussd.pending || ussd.waiting;
	ussd.pending = ussd.cancelled = ussd.waiting = ussd.open = ussd.ucs2 = ussd.ignore = ussd.joining = false;
	if (lost)
		modem_ussd_received(MODEM_USSD_FAILED, "");
}

static void ussd_answered(const char *cmd, bool ok)
{
	if (cmd == cscs_gsm && ok) {
		ussd.ucs2 = false;
	} else if (cmd == cusd_send) {
		ussd.pending = false;
		ussd.ucs2 = true; /* even if AT+CUSD failed after it */
		if (ussd.cancelled) {
			ussd.cancelled = false;
			ussd_end_session();
		} else if (ok) {
			ussd.waiting = true;
			ussd.sent_at = lv_tick_get();
		} else {
			ussd_result(MODEM_USSD_REFUSED, ""); /* no service, a code it would not send */
		}
	}
}

/* +CUSD: <m>[,"<str>"[,<dcs>]], its text joined */
static void ussd_line(const char *l)
{
	int mode = atoi(l + 7), dcs = 15;
	const char *start = strchr(l, '"'), *end = start ? strrchr(l, '"') : NULL;
	char str[sizeof(ussd.reply)] = "", text[800];
	if (end > start) {
		snprintf(str, sizeof(str), "%.*s", (int)(end - start - 1), start + 1);
		if (end[1] == ',')
			dcs = atoi(end + 2);
	}
	if (!ussd.waiting && ussd.ignore && lv_tick_diff(lv_tick_get(), ussd.ignore_at) < USSD_MS) {
		printf("phone-ui: modem: USSD reply after its end, not shown\n");
		return;
	}
	ussd.ignore = false;
	ussd_decode(str, dcs, ussd.ucs2, text, sizeof(text));
	ussd_result(mode == 0 ? MODEM_USSD_DONE : mode == 1 ? MODEM_USSD_MORE : mode == 4 ? MODEM_USSD_UNSUPPORTED :
	            mode == 5 ? MODEM_USSD_TIMEOUT : MODEM_USSD_ENDED, text);
}

/* A line of a +CUSD: complete once its quotes are */
static void ussd_join(const char *l, bool first)
{
	size_t len = strlen(ussd.reply);
	if (first)
		len = ussd.lines = 0;
	snprintf(ussd.reply + len, sizeof(ussd.reply) - len, "%s%s", first ? "" : "\n", l);
	unsigned quotes = 0;
	for (const char *c = ussd.reply; *c; c++)
		quotes += *c == '"';
	ussd.joining = quotes % 2 && ++ussd.lines < USSD_LINES && strlen(ussd.reply) + 1 < sizeof(ussd.reply);
	if (!ussd.joining)
		ussd_line(ussd.reply);
}

/* The provider's ussd_send */
static bool ussd_send_text(const char *text)
{
	if (m.fd < 0 || ussd.pending || ussd.waiting || !ussd_encode(text, ussd.hex, sizeof(ussd.hex)))
		return false;
	ussd.pending = true;
	ussd.open = ussd.ignore = false;
	enqueue(cusd_send);
	return true;
}

/* The provider's ussd_cancel */
static void ussd_cancel(void)
{
	if (m.sent == cusd_send)
		ussd.cancelled = true;
	else if (ussd.pending)
		ussd.pending = false; /* still queued: not sent */
	else if (ussd.waiting || ussd.open)
		ussd_end_session();
}

/* Calls */

static struct {
	enum modem_call state;   /* as last listed, or set by what was sent */
	char dial[40];           /* the number to dial */
	bool dial_pending;       /* ATD queued, or sent and not answered yet */
	bool mute, mute_sent;    /* AT+CMUT's value to send, and the one sent last */
	char tones[32];          /* keys whose tones are still to send, in order */
	uint32_t list_at;        /* AT+CLCC last asked */
	bool listed;             /* a voice call in this AT+CLCC's answer */
	enum modem_call listed_state;
	char listed_number[32];
	enum modem_call_end end; /* what the modem said about how it ended (BUSY...) */
} calls;

/* The call's state, reported when it changes */
static void call_set(enum modem_call state, const char *number)
{
	if (state == MODEM_CALL_NONE) {
		calls.dial_pending = false;
		calls.tones[0] = '\0';
	}
	enum modem_call_end end = calls.end;
	if (state == MODEM_CALL_NONE)
		calls.end = MODEM_CALL_ENDED;
	calls.state = state;
	modem_set_call(state, number, end);
}

/* The list asked for again, first: a call does not wait behind the rest (SMS set-up) */
static void call_list_ask(void)
{
	enqueue_first(clcc);
	calls.list_at = lv_tick_get();
}

/* The modem went, restarted, or its SIM: the call is over */
static void call_forget(void)
{
	calls.dial_pending = false;
	if (calls.state != MODEM_CALL_NONE)
		call_set(MODEM_CALL_NONE, NULL);
}

/* +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,"<number>",<type>]: a voice call (mode 0;
 * Quectel's lists its data connections as calls too) */
static void call_list_line(const char *l)
{
	int id, dir, stat, mode;
	if (sscanf(l + 7, "%d,%d,%d,%d", &id, &dir, &stat, &mode) != 4 || mode != 0)
		return;
	static const enum modem_call states[] = { MODEM_CALL_ACTIVE, MODEM_CALL_ACTIVE, MODEM_CALL_DIALING,
	                                          MODEM_CALL_ALERTING, MODEM_CALL_INCOMING, MODEM_CALL_INCOMING };
	if (stat < 0 || stat > 5)
		return;
	calls.listed = true;
	calls.listed_state = states[stat];
	const char *start = strchr(l, '"'), *end = start ? strchr(start + 1, '"') : NULL;
	calls.listed_number[0] = '\0';
	if (end)
		snprintf(calls.listed_number, sizeof(calls.listed_number), "%.*s", (int)(end - start - 1), start + 1);
}

static void calls_answered(const char *cmd, bool ok)
{
	if (cmd == clcc && ok) {
		if (calls.listed)
			call_set(calls.listed_state, calls.listed_number[0] ? calls.listed_number : NULL);
		else if (calls.state != MODEM_CALL_NONE && !calls.dial_pending)
			call_set(MODEM_CALL_NONE, NULL);
	} else if (cmd == call_dial) {
		calls.dial_pending = false;
		if (ok) {
			call_list_ask();
		} else {
			calls.end = MODEM_CALL_FAILED; /* NO CARRIER: no network, refused */
			call_set(MODEM_CALL_NONE, NULL);
		}
	} else if (cmd == call_mute && calls.mute != calls.mute_sent) {
		enqueue_first(call_mute);
	} else if (cmd == call_tone && calls.tones[0]) {
		enqueue_first(call_tone); /* the next */
	} else if (cmd == call_answer || cmd == call_hang_up) {
		call_list_ask();
	}
	if (cmd == clcc)
		calls.listed = false;
}

/* What the modem says unasked about a call */
static bool call_line(const char *l)
{
	if (strcmp(l, "RING") == 0) {
		if (calls.state == MODEM_CALL_NONE)
			call_set(MODEM_CALL_INCOMING, "");
		call_list_ask();
	} else if (strncmp(l, "+CLIP: ", 7) == 0) {
		/* "<number>",<type>[,...]: before or after RING */
		const char *start = strchr(l, '"'), *end = start ? strchr(start + 1, '"') : NULL;
		char number[32];
		if (!end)
			return true;
		snprintf(number, sizeof(number), "%.*s", (int)(end - start - 1), start + 1);
		if (calls.state == MODEM_CALL_INCOMING)
			call_set(MODEM_CALL_INCOMING, number);
	} else if (strcmp(l, "BUSY") == 0 || strcmp(l, "NO ANSWER") == 0 || strcmp(l, "NO CARRIER") == 0) {
		calls.end = l[0] == 'B' ? MODEM_CALL_BUSY : l[3] == 'A' ? MODEM_CALL_NO_ANSWER : MODEM_CALL_ENDED;
		call_list_ask();
	} else {
		return false;
	}
	return true;
}

/* The provider's */
static bool call_dial_number(const char *number)
{
	size_t n = 0;
	for (const char *c = number; *c && n + 1 < sizeof(calls.dial); c++)
		if ((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#')
			calls.dial[n++] = *c;
	calls.dial[n] = '\0';
	if (m.fd < 0 || !n || calls.state != MODEM_CALL_NONE)
		return false;
	calls.dial_pending = true;
	calls.end = MODEM_CALL_ENDED;
	calls.state = MODEM_CALL_DIALING;
	enqueue_first(call_dial); /* before set-up still queued */
	return true;
}

static void call_answer_it(void)
{
	enqueue_first(call_answer);
}

static void call_hang_up_it(void)
{
	if (calls.dial_pending && m.sent != call_dial) {
		calls.dial_pending = false; /* not dialed yet */
		call_set(MODEM_CALL_NONE, NULL);
		return;
	}
	enqueue_first(call_hang_up);
}

static void call_mute_it(bool on)
{
	calls.mute = on;
	if (m.sent != call_mute)
		enqueue_first(call_mute); /* sent: again once answered, if it changed */
}

static void call_tone_it(char key)
{
	size_t n = strlen(calls.tones);
	if (n + 1 >= sizeof(calls.tones))
		return;
	calls.tones[n] = key;
	calls.tones[n + 1] = '\0';
	if (m.sent != call_tone)
		enqueue_first(call_tone); /* sent: the next once answered */
}

/* The command to write for one of the queue's, its argument added; false if it has
 * nothing to do any more */
static bool command_text(const char *cmd, char *out, size_t size)
{
	if ((cmd == sms_list || cmd == sms_send || cmd == sms_delete) && !sms.pdu)
		return false; /* not in PDU mode (yet) */
	if (cmd == sms_delete) {
		if (!sms.deletes)
			return false;
		snprintf(out, size, "%s=%d\r", cmd, sms.deleting[0]);
	} else if (cmd == sms_send) {
		if (!sms.outs)
			return false;
		snprintf(out, size, "%s=%u\r", cmd, sms.out[0]->s.length[sms.out[0]->part]);
		sms.prompted = false;
	} else if (cmd == call_dial) {
		if (!calls.dial_pending)
			return false; /* hung up before */
		snprintf(out, size, "%s%s;\r", cmd, calls.dial);
	} else if (cmd == call_mute) {
		calls.mute_sent = calls.mute;
		snprintf(out, size, "%s=%d\r", cmd, calls.mute);
	} else if (cmd == call_tone) {
		if (!calls.tones[0])
			return false; /* the call ended */
		snprintf(out, size, "%s=%c\r", cmd, calls.tones[0]);
		memmove(calls.tones, calls.tones + 1, strlen(calls.tones));
	} else if (cmd == cusd_send) {
		if (!ussd.pending)
			return false; /* cancelled */
		snprintf(out, size, "%s,\"%s\",15\r", cmd, ussd.hex);
	} else if (cmd == apn_set) {
		snprintf(out, size, "%s,\"IPV4V6\",\"%s\"\r", cmd, m.apn_new);
	} else {
		size_t n = (size_t)snprintf(out, size, "%s", cmd);
		for (unsigned i = 0; i < SETTINGS; i++)
			if (cmd == settings[i].write && n < size) {
				out[n++] = ',';
				n += (size_t)snprintf(out + n, size - n, settings[i].format, settings[i].wanted);
			}
		snprintf(out + n, size - n, "\r");
	}
	return true;
}

/* The data session taken as ended (the modem gone, restarted, not answering) */
static void data_forget(void)
{
	m.data = m.data_refused = false;
	if (modem_data_state() != MODEM_DATA_OFF)
		modem_set_data(MODEM_DATA_WAITING);
}

/* Another APN than context 1's: a session up ended, the APN set, attached again */
static void apn_update(void)
{
	if (!m.quectel || !m.apn_known || m.apn_changing || !modem_data_available() ||
	    strcmp(modem_data_apn(), m.apn) == 0)
		return;
	snprintf(m.apn_new, sizeof(m.apn_new), "%s", modem_data_apn());
	printf("phone-ui: modem: APN %s, attaching again\n", m.apn_new[0] ? m.apn_new : "the network's");
	m.apn_changing = true;
	if (m.data) {
		m.data = false;
		enqueue(data_off);
	}
	enqueue(apn_set);
}

/* What the APN commands' answers mean, once final */
static void apn_answered(const char *cmd, bool ok)
{
	if (cmd == apn_query) {
		m.apn_known = true; /* not read (an error): "", the network's, taken */
	} else if (cmd == apn_set) {
		if (!ok)
			printf("phone-ui: modem: APN refused\n");
		snprintf(m.apn, sizeof(m.apn), "%s", m.apn_new); /* not tried again until changed */
		/* Attached again even when not known to be registered yet (at the start): it
		 * may have attached with the last one */
		if (ok) {
			enqueue(detach);
			enqueue(attach);
		} else {
			m.apn_changing = false;
		}
	} else if (cmd == attach) {
		m.apn_changing = false; /* an error even when attached (the EG25-G) */
		/* Registered again a few seconds later: the session after DATA_RETRY_MS */
		m.data_refused = true;
		m.data_at = lv_tick_get();
	}
}

/* +CGDCONT: <cid>,"<type>","<apn>",...: context 1's APN */
static void apn_line(const char *l)
{
	if (atoi(l + 10) != 1)
		return;
	const char *start = strchr(l, ',');
	start = start ? strchr(start + 1, ',') : NULL;
	const char *end = start && start[1] == '"' ? strchr(start + 2, '"') : NULL;
	if (end)
		snprintf(m.apn, sizeof(m.apn), "%.*s", (int)(end - start - 2), start + 2);
}

/* The data session started, ended or checked as needed (each tick) */
static void data_update(uint32_t now)
{
	apn_update();
	bool want = m.quectel && modem_data_wanted() && m.apn_known && !m.apn_changing;
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

/* Before what is queued (once) */
static void enqueue_first(const char *cmd)
{
	if (m.sent == cmd)
		return;
	unsigned i = 0;
	while (i < m.queued && m.queue[i] != cmd)
		i++;
	if (i == m.queued && m.queued == QUEUE)
		i--; /* full: the last one dropped */
	else if (i == m.queued)
		m.queued++;
	memmove(m.queue + 1, m.queue, i * sizeof(m.queue[0]));
	m.queue[0] = cmd;
}

/* From the start: echo may be on again, nothing asked is answered */
static void set_up(void)
{
	m.restarting = false;
	m.queued = 0;
	m.sent = NULL;
	m.ask = false;
	m.len = 0;
	network_forget();
	data_forget();
	m.number_known = false;
	m.apn_known = m.apn_changing = false;
	m.apn[0] = '\0';
	sms_forget();
	ussd_forget();
	call_forget();
	enqueue(echo_off);
	enqueue(errors);
	enqueue(cgsn);
	for (unsigned i = 0; i < SETTINGS; i++)
		if (settings[i].wanted >= 0 && settings[i].step != SETTING_DONE)
			enqueue(settings[i].read); /* first: they may restart the modem */
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

/* Within boot_wait=: a modem not on USB yet is still starting */
static bool booting(void)
{
	struct timespec t;
	return clock_gettime(CLOCK_BOOTTIME, &t) == 0 && t.tv_sec < m.boot_wait;
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
	m.restarting = false;
	data_forget();
	sms_forget();
	ussd_forget();
	call_forget();
	modem_set_state(booting() ? MODEM_STARTING : MODEM_ABSENT);
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
		sms_forget();
		ussd_forget();
		call_forget();
	} else if (!was) {
		enqueue(creg);
		enqueue(cereg);
	}
}

/* The SIM's number and context 1's APN, after what the network's state needs (its
 * operator) */
static void number_ask(void)
{
	if (!m.number_known && modem_state() == MODEM_READY)
		enqueue(cnum);
	/* Data context 1's APN, for a data session (registered) */
	if (!m.apn_known && m.quectel && modem_data_available())
		enqueue(apn_query);
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
		sms_start();
		return;
	}
	if (m.cs < 0 || m.eps < 0)
		return;
	number_ask();
	sms_start();
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
	apn_answered(cmd, ok);
	settings_answered(cmd, ok);
	sms_answered(cmd, ok, error);
	ussd_answered(cmd, ok);
	calls_answered(cmd, ok);
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
	if (ussd.joining) {
		ussd_join(l, false); /* any text: "At..." is not an echo there */
		return;
	}
	if (strncasecmp(l, "AT", 2) == 0)
		return; /* the echo of a command */
	if (m.restarting && strcmp(l, "RDY") != 0)
		return; /* what it says before it goes */
	if (strncmp(l, "+CUSD: ", 7) == 0) {
		ussd_join(l, true);
		return;
	}
	/* An error while a USSD reply is awaited (nothing else sent): the code refused */
	if (ussd.waiting && !m.sent && (strcmp(l, "ERROR") == 0 || strncmp(l, "+CME ERROR: ", 12) == 0)) {
		ussd_result(MODEM_USSD_REFUSED, "");
		return;
	}
	/* ATD's and ATA's other final answers; unasked, how a call ended */
	bool ended = strcmp(l, "NO CARRIER") == 0 || strcmp(l, "BUSY") == 0 || strcmp(l, "NO ANSWER") == 0;
	if (ended && (m.sent == call_dial || m.sent == call_answer)) {
		answered(false, -1);
		return;
	}
	if (call_line(l))
		return;
	if (m.sent == sms_list && sms.pdu_of >= 0 && strcmp(l, "OK") != 0 && strncmp(l, "+CM", 3) != 0) {
		sms_pdu_line(sms.pdu_of, l); /* after its +CMGL: line */
		sms.pdu_of = -1;
		return;
	}
	if (strcmp(l, "OK") == 0) {
		answered(true, 0);
	} else if (strcmp(l, "ERROR") == 0) {
		answered(false, -1);
	} else if (strncmp(l, "+CME ERROR: ", 12) == 0) {
		answered(false, error_code(l + 12));
	} else if (strncmp(l, "+CMS ERROR: ", 12) == 0) {
		answered(false, atoi(l + 12));
	} else if (strncmp(l, "+CMTI: ", 7) == 0) {
		sms_list_ask(); /* a new one stored */
	} else if (strncmp(l, "+CMGL: ", 7) == 0) {
		/* <index>,<stat>,[<alpha>],<length>, its PDU on the next line */
		if (m.sent == sms_list)
			sms.pdu_of = atoi(l + 7);
	} else if (m.quectel && strcmp(l, "+QIND: SMS DONE") == 0) {
		if (sms.started)
			sms_set_up();
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
	} else if (m.sent == clcc && strncmp(l, "+CLCC: ", 7) == 0) {
		call_list_line(l);
	} else if (strncmp(l, "+CNUM: ", 7) == 0) {
		number_line(l);
	} else if (m.sent == cgsn && imei_line(l)) {
		/* the IMEI, kept */
	} else if (m.sent == apn_query && strncmp(l, "+CGDCONT: ", 10) == 0) {
		apn_line(l);
	} else if (strncmp(l, "$QCRMCALL: ", 11) == 0) {
		m.data_listed = true;
	} else if (strncmp(l, "+CSQ: ", 6) == 0) {
		csq_line(l);
	} else if (setting_line(l)) {
		/* a setting's value, kept */
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
		if (!port_open()) {
			if (modem_state() == MODEM_STARTING && !booting())
				modem_set_state(MODEM_ABSENT);
			return;
		}
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

	/* AT+CMGS's prompt ("> ", no line end): the part's PDU, ended by Ctrl-Z */
	if (m.sent == sms_send && !sms.prompted && m.len && m.line[0] == '>' && sms.outs) {
		const struct outgoing *o = sms.out[0];
		char pdu[SMS_PDU_HEX + 1];
		int len = snprintf(pdu, sizeof(pdu), "%s\x1a", o->s.pdu[o->part]);
		sms.prompted = true;
		m.len = 0;
		if (write(m.fd, pdu, (size_t)len) != len)
			printf("phone-ui: modem: cannot send the SMS\n"); /* not answered: set up again */
	}

	uint32_t limit = m.sent == sms_send ? SMS_SEND_MS : m.sent == sms_list ? SMS_LIST_MS : ANSWER_MS;
	if (m.sent && lv_tick_diff(now, m.sent_at) >= limit) {
		printf("phone-ui: modem: no answer to %s\n", m.sent);
		modem_set_state(MODEM_STARTING);
		set_up();
	}
	if (calls.state != MODEM_CALL_NONE && (int32_t)(now - calls.list_at) >= CALL_LIST_MS)
		call_list_ask();
	if (ussd.waiting && (int32_t)(now - ussd.sent_at) >= USSD_MS) { /* sent_at may be after now */
		ussd_end_session();
		modem_ussd_received(MODEM_USSD_TIMEOUT, "");
	}
	if (m.restarting) {
		if (lv_tick_diff(now, m.restart_at) < RESTART_MS)
			return;
		printf("phone-ui: modem: not back after its restart\n");
		set_up();
	}
	if (m.ask && (int32_t)(now - m.ask_at) >= 0) {
		m.ask = false;
		enqueue(cpin);
	}
	data_update(now);
	if (m.registered && modem_state() == MODEM_READY && lv_tick_diff(now, m.signal_at) >= SIGNAL_MS)
		signal_ask();
	if (sms.retry && (int32_t)(now - sms.retry_at) >= 0) {
		sms.retry = false;
		if (modem_state() == MODEM_READY)
			sms_set_up();
	}
	while (!m.sent && m.queued && !ussd.waiting) { /* a USSD reply awaited: nothing else */
		char cmd[384];
		const char *next = m.queue[0];
		memmove(m.queue, m.queue + 1, --m.queued * sizeof(m.queue[0]));
		if (!command_text(next, cmd, sizeof(cmd)))
			continue; /* nothing left to do */
		m.sent = next;
		m.sent_at = now;
		m.data_listed = false;
		size_t len = strlen(cmd);
		if (write(m.fd, cmd, len) != (ssize_t)len)
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
	const char *boot_wait = modem_config("boot_wait");
	m.boot_wait = boot_wait ? strtol(boot_wait, NULL, 10) : 0;
	if (booting())
		modem_set_state(MODEM_STARTING);
	for (unsigned i = 0; i < SETTINGS; i++) {
		struct setting *s = &settings[i];
		const char *value = modem_config(s->key);
		for (int v = 0; value && m.quectel && v < s->values; v++)
			if (s->words[v] && strcmp(value, s->words[v]) == 0)
				s->wanted = v;
		if (value && s->wanted < 0)
			printf("phone-ui: modem: %s=%s not applied (Quectel's, a value it takes)\n", s->key, value);
	}
	lv_timer_create(tick_cb, TICK_MS, NULL);
	return true;
}

const struct modem_provider modem_at = { .name = "at", .start = start, .sms_send = sms_send_text,
                                         .ussd_send = ussd_send_text, .ussd_cancel = ussd_cancel,
                                         .call_dial = call_dial_number, .call_answer = call_answer_it,
                                         .call_hang_up = call_hang_up_it, .call_mute = call_mute_it,
                                         .call_tone = call_tone_it };
