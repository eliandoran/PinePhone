/*
 * Modem providers: what drives a kind of modem for modem.c, chosen by the board's
 * modem.conf ("provider=at"). A provider reads its settings from the same file and
 * reports what it finds with modem_set_state(); the apps only see modem.h.
 */

#ifndef PHONE_UI_MODEM_PROVIDER_H
#define PHONE_UI_MODEM_PROVIDER_H

#include <stdbool.h>

#include "modem.h"

struct modem_provider {
	const char *name; /* modem.conf's provider= */
	/* Starts watching the modem, from timers of its own; false if it cannot (a
	 * setting missing), logged */
	bool (*start)(void);
};

/* Standard AT commands (3GPP TS 27.007) on a serial port (modem_at.c) */
extern const struct modem_provider modem_at;

/* A value of modem.conf, NULL if it is not set */
const char *modem_config(const char *key);
/* Whether a comma-separated value has that word: ("quirks", "quectel") */
bool modem_config_has(const char *key, const char *word);
/* What the provider found, logged when it changes. The network's state counts with a
 * ready SIM only (forgotten when it is not); its operator's name (NULL, "": none) while
 * registered */
void modem_set_state(enum modem_state state);
void modem_set_network(enum modem_network network, const char *operator_name);
/* While registered: the signal bars (0 to 4, -1 not known) and the technology,
 * forgotten when it is not */
void modem_set_signal(int bars, enum modem_tech tech);
/* The modem's IMEI, the SIM's own number (NULL, "": none), forgotten with the modem,
 * the SIM */
void modem_set_imei(const char *imei);
void modem_set_number(const char *number);
/* Mobile data: whether the session should be up now (on, registered at home), and its
 * state. Connected, the board's script brings the interface up; no longer, down */
bool modem_data_wanted(void);
void modem_set_data(enum modem_data state);
/* The board's script for the data interface, with an action of its own ("reset":
 * the interface refused a session); run one at a time, in order, not waited for */
void modem_data_script(const char *action);

#endif
