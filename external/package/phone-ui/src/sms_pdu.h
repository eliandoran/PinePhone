/*
 * SMS in PDU mode (3GPP TS 23.040, 23.038): the SMS-DELIVER a modem lists, decoded,
 * and the SMS-SUBMIT parts it sends, encoded. Texts are UTF-8: GSM 7-bit (with its
 * extension table) when they fit in it, UCS-2 otherwise (UTF-16: emoji take two
 * units); a long text goes in parts tied by a concatenation header.
 */

#ifndef PHONE_UI_SMS_PDU_H
#define PHONE_UI_SMS_PDU_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* A received message, or a part of one */
struct sms_deliver {
	char sender[32];   /* "+40744555210" (international), "7726", or a name ("Vodafone") */
	time_t time;       /* the service centre's, UTC */
	char text[704];    /* a part's: 160 GSM characters, up to 3 bytes each in UTF-8 (€) */
	unsigned ref;      /* concatenated: the parts' reference, */
	unsigned total;    /* how many (1: a whole message) */
	unsigned seq;      /* and which one, from 1 */
};

/* A PDU with the service centre's address first, as AT+CMGR and AT+CMGL give it (in
 * hexadecimal); false if it is not an SMS-DELIVER with text (a status report, 8-bit
 * data, a broken one) */
bool sms_deliver_decode(const char *hex, struct sms_deliver *out);

#define SMS_PARTS_MAX 10
#define SMS_PDU_HEX 384

/* A message to send: its parts, for AT+CMGS=<length> then the PDU (the service
 * centre's address left to the SIM: "00" first, not counted in the length) */
struct sms_submit {
	unsigned count;
	unsigned length[SMS_PARTS_MAX];
	char pdu[SMS_PARTS_MAX][SMS_PDU_HEX];
};

/* `ref` ties the parts of a long text (each message a new one, 0-255). False if the
 * number has no digits, or the text is empty or needs more than SMS_PARTS_MAX parts */
bool sms_submit_encode(const char *number, const char *text, unsigned ref, struct sms_submit *out);

/* USSD strings (AT+CUSD, 3GPP TS 27.007 7.15). `text` (UTF-8) in hexadecimal UCS-2,
 * for AT+CSCS="UCS2"; false if that does not fit in `size` */
bool ussd_encode(const char *text, char *hex, size_t size);
/* A +CUSD string as UTF-8, its line ends "\n": hexadecimal UCS-2 when `ucs2`
 * (AT+CSCS="UCS2"), or when its dcs (3GPP TS 23.038's cell broadcast scheme) is UCS-2
 * and it is hexadecimal (as modems give those in "GSM" too); text otherwise */
void ussd_decode(const char *str, int dcs, bool ucs2, char *out, size_t size);

#endif
