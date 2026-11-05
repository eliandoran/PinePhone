/* The modem (modem.c): whether there is one, the SIM's state */

#ifndef PHONE_UI_MODEM_H
#define PHONE_UI_MODEM_H

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
/* The registered network's name ("" if it gave none) */
const char *modem_operator(void);
/* The state for the status bar, what is wrong only ("No SIM", "No service"; "" when
 * registered or without a modem), and in full for the shade and Settings ("No SIM
 * card", the operator's name, "Vodafone (roaming)"); valid until the state changes */
const char *modem_status_text(void);
const char *modem_summary(void);

#endif
