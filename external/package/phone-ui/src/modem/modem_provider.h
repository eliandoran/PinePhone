/*
 * Modem providers: what drives a kind of modem for modem.c, chosen by the board's
 * modem.conf ("provider=at"). A provider reads its settings from the same file and
 * reports what it finds with modem_set_state(); the apps only see modem.h.
 */

#ifndef PHONE_UI_MODEM_PROVIDER_H
#define PHONE_UI_MODEM_PROVIDER_H

#include <stdbool.h>
#include <time.h>

#include "modem.h"

struct modem_provider {
	const char *name; /* modem.conf's provider= */
	/* Starts watching the modem, from timers of its own; false if it cannot (a
	 * setting missing), logged */
	bool (*start)(void);
	/* Sends a text (registered), its result given with modem_sms_sent(id); false if it
	 * cannot (too long, not a number). NULL: no SMS */
	bool (*sms_send)(unsigned id, const char *number, const char *text);
	/* Sends a USSD code or an answer (registered), its reply given with
	 * modem_ussd_received(); false if it cannot (a reply awaited). Ends the session.
	 * NULL: no USSD */
	bool (*ussd_send)(const char *text);
	void (*ussd_cancel)(void);
	/* Voice calls (registered), their state given with modem_set_call(); dial false if
	 * it cannot. NULL: no calls */
	bool (*call_dial)(const char *number);
	void (*call_answer)(void);
	void (*call_hang_up)(void);
	void (*call_mute)(bool on);
	/* A key's tone (0-9, *, #) to the other end, connected; NULL none */
	void (*call_tone)(char key);
	/* The phone woke up from sleep, perhaps for a call whose first ring the modem
	 * reported while it slept, lost: asks for its calls. NULL: nothing */
	void (*woke)(void);
	/* What woke() asked is not answered yet. NULL: nothing awaited */
	bool (*woke_pending)(void);
};

/* Standard AT commands (3GPP TS 27.007) on a serial port (modem_at.c) */
extern const struct modem_provider modem_at;

/* A value of modem.conf, NULL if it is not set */
const char *modem_config(const char *key);
/* The `n`th value of a key set on several lines (from 0), NULL past the last */
const char *modem_config_nth(const char *key, unsigned n);
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
/* What an earlier phone-ui left (it does not end the session as it exits), once the
 * provider knows: a session up, taken as connected without the script while its interface
 * is still up (true: kept); none, or its interface down since, the interface taken down
 * (false: the provider ends the session) */
bool modem_data_left(bool up);
/* SMS: one received whole (its parts joined), from a number or a name, at the service
 * centre's time; true once it is kept (the provider may delete it from the modem).
 * The result of a send */
bool modem_sms_received(const char *sender, time_t time, const char *text);
void modem_sms_sent(unsigned id, bool ok);
/* USSD: a reply (text in UTF-8, "" none) or why there is none, asked for or not */
void modem_ussd_received(enum modem_ussd state, const char *text);
/* The call's state, with the other end's number (NULL: not known yet, kept; "" hidden)
 * and once NONE how it ended */
void modem_set_call(enum modem_call state, const char *number, enum modem_call_end end);

#endif
