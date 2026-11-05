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
	MODEM_READY,
};

/* Reads the board's modem.conf and starts its provider (ui_init()) */
void modem_init(void);
enum modem_state modem_state(void);
/* The state for the status bar ("No SIM", "" without a modem) and in full for
 * Settings ("No SIM card") */
const char *modem_status_text(void);
const char *modem_summary(void);

#endif
