/*
 * The modem in the browser, instead of ../src/modem/modem_at.c: the provider main.c's
 * modem.conf names ("provider=at"), played here without AT commands. A SIM registered
 * at home on "Demo Network", 4G, three bars, a second after starting. Mobile data
 * connects a second after it is wanted (the board's script cannot run: the browser has
 * the internet). Texts are sent and answered by their recipient; USSD codes get a
 * balance; calls ring, are answered and stay connected (without sound) until hung up.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modem/modem_provider.h"
#include "ui.h"

#define REPLY "Hello! This phone runs in your browser: your message went nowhere, but it was sent."

/* fn(arg) in `ms`, once */
static void later(uint32_t ms, lv_timer_cb_t fn, void *arg)
{
	lv_timer_t *t = lv_timer_create(fn, ms, arg);
	lv_timer_set_repeat_count(t, 1);
}

static void registered_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	modem_set_imei("356938035643809");
	modem_set_state(MODEM_READY);
	modem_set_number("+44 7700 900123");
	modem_set_network(MODEM_NET_HOME, "Demo Network");
	modem_set_signal(3, MODEM_TECH_4G);
}

/* Mobile data, each second: connecting, then connected, while wanted */
static void data_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	if (!modem_data_wanted())
		modem_set_data(modem_data_enabled() ? MODEM_DATA_WAITING : MODEM_DATA_OFF);
	else if (modem_data_state() == MODEM_DATA_CONNECTING)
		modem_set_data(MODEM_DATA_CONNECTED);
	else if (modem_data_state() != MODEM_DATA_CONNECTED)
		modem_set_data(MODEM_DATA_CONNECTING);
}

static bool start(void)
{
	modem_set_state(MODEM_STARTING);
	later(1000, registered_cb, NULL);
	lv_timer_create(data_cb, 1000, NULL);
	return true;
}

/* SMS: sent a second later, answered three after that */

struct sms {
	unsigned id;
	char number[32];
};

static void sms_reply_cb(lv_timer_t *t)
{
	struct sms *s = lv_timer_get_user_data(t);
	modem_sms_received(s->number, time(NULL), REPLY);
	free(s);
}

static void sms_sent_cb(lv_timer_t *t)
{
	struct sms *s = lv_timer_get_user_data(t);
	modem_sms_sent(s->id, true);
	later(3000, sms_reply_cb, s);
}

static bool sms_send(unsigned id, const char *number, const char *text)
{
	LV_UNUSED(text);
	struct sms *s = malloc(sizeof(*s));
	if (!s)
		return false;
	s->id = id;
	snprintf(s->number, sizeof(s->number), "%s", number);
	later(1000, sms_sent_cb, s);
	return true;
}

/* USSD: any code, the same reply */

static void ussd_reply_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	modem_ussd_received(MODEM_USSD_DONE, "Your balance is 12.50. This is a demo network.");
}

static bool ussd_send(const char *text)
{
	LV_UNUSED(text);
	later(1500, ussd_reply_cb, NULL);
	return true;
}

static void ussd_cancel(void)
{
}

/* Calls: ringing there after a second, answered three later. One timer at a time,
 * deleted when hung up */

static lv_timer_t *call_timer;

static void answered_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	call_timer = NULL;
	modem_set_call(MODEM_CALL_ACTIVE, NULL, MODEM_CALL_ENDED);
}

static void alerting_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	modem_set_call(MODEM_CALL_ALERTING, NULL, MODEM_CALL_ENDED);
	call_timer = lv_timer_create(answered_cb, 3000, NULL);
	lv_timer_set_repeat_count(call_timer, 1);
}

static bool call_dial(const char *number)
{
	LV_UNUSED(number);
	call_timer = lv_timer_create(alerting_cb, 1000, NULL);
	lv_timer_set_repeat_count(call_timer, 1);
	return true;
}

static void call_answer(void)
{
	modem_set_call(MODEM_CALL_ACTIVE, NULL, MODEM_CALL_ENDED);
}

static void call_hang_up(void)
{
	if (call_timer)
		lv_timer_delete(call_timer);
	call_timer = NULL;
	modem_set_call(MODEM_CALL_NONE, NULL, MODEM_CALL_ENDED);
}

static void call_mute(bool on)
{
	LV_UNUSED(on);
}

const struct modem_provider modem_at = {
	.name = "at",
	.start = start,
	.sms_send = sms_send,
	.ussd_send = ussd_send,
	.ussd_cancel = ussd_cancel,
	.call_dial = call_dial,
	.call_answer = call_answer,
	.call_hang_up = call_hang_up,
	.call_mute = call_mute,
};
