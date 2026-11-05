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
/* What the provider found, logged when it changes */
void modem_set_state(enum modem_state state);

#endif
