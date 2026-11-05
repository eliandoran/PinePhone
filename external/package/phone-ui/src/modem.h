/* The modem (modem.c): whether there is one, the SIM's state */

#ifndef PHONE_UI_MODEM_H
#define PHONE_UI_MODEM_H

#include <stdbool.h>

enum modem_state {
	MODEM_NONE,      /* the device has none (no modem.conf), or its provider cannot run */
	MODEM_ABSENT,    /* not there: powered off, its switch off */
	MODEM_STARTING,  /* there, not answering yet */
	MODEM_NO_SIM,
	MODEM_SIM_PIN,   /* locked, its PIN wanted */
	MODEM_SIM_PUK,   /* blocked, its PUK wanted */
	MODEM_SIM_ERROR, /* there but unusable */
	MODEM_READY,     /* the SIM: then the network's state (enum modem_network) */
};

/* With a ready SIM */
enum modem_network {
	MODEM_NET_UNKNOWN,   /* not known yet */
	MODEM_NET_NONE,      /* not registered, not searching */
	MODEM_NET_SEARCHING,
	MODEM_NET_DENIED,    /* registration denied */
	MODEM_NET_HOME,
	MODEM_NET_ROAMING,
};

/* Reads the board's modem.conf and starts its provider (ui_init()) */
void modem_init(void);
enum modem_state modem_state(void);
enum modem_network modem_network(void);

/* The radio technology while registered */
enum modem_tech {
	MODEM_TECH_UNKNOWN,
	MODEM_TECH_2G, /* GSM, GPRS, EDGE */
	MODEM_TECH_3G, /* UMTS, HSPA */
	MODEM_TECH_4G, /* LTE */
	MODEM_TECH_5G, /* NR */
};
enum modem_tech modem_technology(void);
/* "4G"; "" not registered, or not known */
const char *modem_technology_text(void);
/* Signal bars while registered, 0 to 4; -1 not registered, or not known yet */
int modem_signal_bars(void);
/* The registered network's name ("" if it gave none) */
const char *modem_operator(void);
/* Registered on another network than the SIM's (the status bar's "R") */
bool modem_roaming(void);
/* The state for the status bar, a symbol after the signal bars for what is wrong:
 * LV_SYMBOL_WARNING no modem or a SIM error, UI_SYMBOL_SIM_CARD no SIM, UI_SYMBOL_LOCK
 * SIM locked or blocked, LV_SYMBOL_CLOSE no service; "" when registered, on the way
 * (starting, searching: the bars unlit say it) or without a modem. Alarming when
 * waiting will not help (no modem, SIM blocked): shown red */
const char *modem_status_icon(void);
bool modem_status_alarm(void);
/* The state in full for the shade and Settings ("No SIM card", the operator's name,
 * "Vodafone (roaming)"); valid until the state changes */
const char *modem_summary(void);
/* The modem's IMEI while it is there, the SIM's own number ("+40712345678") while it
 * is ready; "" not known (many SIMs do not hold their number) */
const char *modem_imei(void);
const char *modem_number(void);

/* Mobile data: on or off (remembered, mobile_data in the settings; off unless turned
 * on), connected while on and registered on the home network, roaming too with data
 * roaming on (data_roaming, off unless turned on: data abroad can cost). There when
 * the board set it up (modem.conf's data= and data_script=) */
enum modem_data {
	MODEM_DATA_OFF,
	MODEM_DATA_WAITING,    /* on: no SIM, no network, roaming without data roaming */
	MODEM_DATA_CONNECTING,
	MODEM_DATA_CONNECTED,
};
bool modem_data_available(void);
bool modem_data_enabled(void);
void modem_data_set_enabled(bool on);
bool modem_data_roaming(void);
void modem_data_set_roaming(bool on);
/* The access point name (apn in the settings): "" the network's own. Set, only what
 * an APN is made of is kept (letters, digits, dots, dashes; at most 63) */
const char *modem_data_apn(void);
void modem_data_set_apn(const char *apn);
#define MODEM_APN_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"
enum modem_data modem_data_state(void);

/* SMS. Why one cannot be sent now ("No SIM card", "No service"; static text), NULL if
 * it can */
const char *modem_sms_unavailable(void);
/* Sends a text: its id, its result given to conversation_sent() (data.h); 0 if it
 * cannot be sent (unavailable, too long, not a number). Received ones go to
 * conversation_received() */
unsigned modem_sms_send(const char *number, const char *text);

#endif
