/* The modem (modem.c): whether there is one, the SIM's state */

#ifndef PHONE_UI_MODEM_H
#define PHONE_UI_MODEM_H

enum modem_state {
	MODEM_ABSENT,    /* no AT port: powered off, its switch off */
	MODEM_STARTING,  /* the port is there, no answer yet */
	MODEM_NO_SIM,
	MODEM_SIM_PIN,   /* locked, its PIN wanted */
	MODEM_SIM_PUK,   /* blocked, its PUK wanted */
	MODEM_SIM_ERROR, /* there but unusable */
	MODEM_READY,
};

/* Watches the AT port from a timer of its own (ui_init()) */
void modem_init(void);
enum modem_state modem_state(void);
/* The state for the status bar ("No SIM") and in full for Settings ("No SIM card") */
const char *modem_status_text(void);
const char *modem_summary(void);

#endif
