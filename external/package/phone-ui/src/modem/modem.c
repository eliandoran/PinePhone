/*
 * The modem, whatever drives it: the board says in /etc/phone-ui/modem.conf which
 * provider (modem_provider.h) and its settings, one "key=value" a line, "#" comments:
 *
 *	provider=at
 *	port=/dev/ttyUSB2
 *	quirks=quectel
 *
 * Without the file the device has no modem: nothing is probed, the status bar shows
 * nothing about it. The state is the provider's, the texts the apps show are here.
 *
 * Mobile data, with data=<interface> and data_script=<path>: the provider starts the
 * modem's session, the board's script sets up the interface, run as
 * `<script> up|down|reset <interface>` (address, routes, DNS; a driver reset when the
 * interface refused a session). Run one at a time, in order, by a timer that reaps
 * them: the UI does not wait. Their file descriptors are closed but stdio, as a DHCP
 * client they leave running would otherwise keep phone-ui's (DRM master among them).
 *
 * Calls, with audio_script=<path>: the board routes their sound (the codec between the
 * modem, the earpiece or loudspeaker and the microphone), run as `<script> earpiece
 * <level>`, `speaker <level>`, `bluetooth <level>` (a headset's, bluez-alsa's Hands-Free
 * PCMs; the call's volume, 1 to UI_VOLUME_LEVELS: volume.c) or `off` in the same queue. Routed as a call is dialed (the network's ring tone comes
 * through it) or answered, again when its volume changes, off once it ends. Logged as it ends: outgoing
 * with how long it was connected, incoming answered or refused here, missed.
 */

#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net/bluetooth.h"
#include "data/data.h"
#include "i18n.h"
#include "log.h"
#include "modem_provider.h"
#include "ui.h"
#include "ui_symbols.h"

#define SCRIPT_TICK_MS 200
#define SCRIPT_QUEUE 8

#ifndef MODEM_CONF
#define MODEM_CONF "/etc/phone-ui/modem.conf"
#endif
#define MAX_KEYS 16

static const struct modem_provider *const providers[] = { &modem_at };
static const struct modem_provider *provider; /* modem.conf's, started */

static enum modem_state state;
static enum modem_network network;
static char operator_name[64];
static char imei[32], own_number[32];
static int bars = -1;           /* while registered */
static enum modem_tech tech;
static const char *status_icon = "";
static char summary[96];
static char keys[MAX_KEYS][2][128];
static unsigned key_count;
static struct {
	const char *iface, *script; /* NULL: no mobile data */
	enum modem_data state;      /* the provider's, while on */
} data;

/* The board's scripts (mobile data, call audio), run one at a time, in order */
static struct {
	struct {
		const char *script, *action;
		char arg[32];           /* "" none */
		void (*done)(void);     /* once it ran, NULL nothing */
	} queue[SCRIPT_QUEUE];
	unsigned queued;
	pid_t pid;                  /* the one running, 0 none */
	void (*done)(void);         /* its */
} scripts;

/* Calls: the provider's state, and what the UI and the call log need of it */
static struct {
	const char *script;         /* audio_script=, NULL none */
	enum modem_call state;
	char number[32];
	bool incoming, refused;     /* refused: hung up while it rang here */
	time_t start;               /* when it was dialed or rang */
	uint32_t connected;         /* lv_tick_get() when it was answered, 0 not */
	enum modem_call_output output;
	const char *route;          /* the audio script's last: "earpiece", "speaker", "bluetooth", "off" */
	int volume;                 /* and the level it was given */
} call;

static void call_route(const char *route);

/* Each state: its status bar symbol ("" none: the bars unlit say it), its text (English: logged,
 * kept with a message not sent; translated where shown) */
static const char *const texts[][2] = {
	[MODEM_NONE] = { "", N_("Not available") },
	[MODEM_ABSENT] = { LV_SYMBOL_WARNING, N_("No modem") },
	[MODEM_STARTING] = { "", N_("Starting") },
	[MODEM_NO_SIM] = { UI_SYMBOL_SIM_CARD, N_("No SIM card") },
	[MODEM_SIM_PIN] = { UI_SYMBOL_LOCK, N_("SIM locked (PIN)") },
	[MODEM_SIM_PUK] = { UI_SYMBOL_LOCK, N_("SIM blocked (PUK)") },
	[MODEM_SIM_ERROR] = { LV_SYMBOL_WARNING, N_("SIM error") },
	[MODEM_READY] = { "", N_("SIM ready") }, /* the network's state not known yet */
};

static const char *const network_texts[][2] = {
	[MODEM_NET_NONE] = { LV_SYMBOL_CLOSE, N_("No service") },
	[MODEM_NET_SEARCHING] = { "", N_("Searching") },
	[MODEM_NET_DENIED] = { LV_SYMBOL_CLOSE, N_("No service (denied)") },
};

enum modem_state modem_state(void)
{
	return state;
}

enum modem_network modem_network(void)
{
	return network;
}

const char *modem_operator(void)
{
	return operator_name;
}

static bool registered(void)
{
	return state == MODEM_READY && (network == MODEM_NET_HOME || network == MODEM_NET_ROAMING);
}

bool modem_roaming(void)
{
	return registered() && network == MODEM_NET_ROAMING;
}

enum modem_tech modem_technology(void)
{
	return registered() ? tech : MODEM_TECH_UNKNOWN;
}

const char *modem_technology_text(void)
{
	static const char *const names[] = {
		[MODEM_TECH_UNKNOWN] = "", [MODEM_TECH_2G] = "2G", [MODEM_TECH_3G] = "3G",
		[MODEM_TECH_4G] = "4G", [MODEM_TECH_5G] = "5G",
	};
	return names[modem_technology()];
}

int modem_signal_bars(void)
{
	return registered() ? bars : -1;
}

void modem_set_signal(int b, enum modem_tech t)
{
	if (b < -1 || b > 4)
		b = -1;
	bool changed = b != bars || t != tech;
	bars = b;
	tech = t;
	if (changed && registered())
		printf("phone-ui: modem: signal: %d bars, %s\n", b, t ? modem_technology_text() : "?");
}

/* The state now, in status_icon and summary (in the current language) */
static void texts_update(void)
{
	const char *name = operator_name[0] ? operator_name : _("Registered");
	if (state != MODEM_READY || network == MODEM_NET_UNKNOWN) {
		status_icon = texts[state][0];
		snprintf(summary, sizeof(summary), "%s", _(texts[state][1]));
	} else if (network == MODEM_NET_HOME || network == MODEM_NET_ROAMING) {
		status_icon = ""; /* all is well: the bars, the name in the shade */
		snprintf(summary, sizeof(summary), network == MODEM_NET_ROAMING ? _("%s (roaming)") : "%s", name);
	} else {
		status_icon = network_texts[network][0];
		snprintf(summary, sizeof(summary), "%s", _(network_texts[network][1]));
	}
}

const char *modem_status_icon(void)
{
	return status_icon;
}

bool modem_status_alarm(void)
{
	return state == MODEM_ABSENT || state == MODEM_SIM_PUK;
}

const char *modem_summary(void)
{
	texts_update(); /* the language may have changed */
	return summary;
}

void modem_set_state(enum modem_state s)
{
	if (s != state)
		printf("phone-ui: modem: %s\n", texts[s][1]);
	state = s;
	if (s == MODEM_NONE || s == MODEM_ABSENT)
		imei[0] = '\0';
	if (s != MODEM_READY) {
		network = MODEM_NET_UNKNOWN;
		operator_name[0] = '\0';
		own_number[0] = '\0';
		bars = -1;
		tech = MODEM_TECH_UNKNOWN;
		if (data.state != MODEM_DATA_OFF)
			modem_set_data(MODEM_DATA_WAITING);
	}
	texts_update();
}

const char *modem_imei(void)
{
	return imei;
}

const char *modem_number(void)
{
	return own_number;
}

void modem_set_imei(const char *s)
{
	if (state == MODEM_NONE || state == MODEM_ABSENT)
		return;
	snprintf(imei, sizeof(imei), "%s", s ? s : "");
}

void modem_set_number(const char *s)
{
	if (state != MODEM_READY)
		return;
	s = s ? s : "";
	if (strcmp(s, own_number) != 0) /* not the number itself: logs get shared */
		printf("phone-ui: modem: the SIM's number %s\n", s[0] ? "known" : "not on it");
	snprintf(own_number, sizeof(own_number), "%s", s);
}

/* Mobile data */

bool modem_data_available(void)
{
	return data.iface && data.script && state != MODEM_NONE;
}

bool modem_data_enabled(void)
{
	return modem_data_available() && ui_setting_get_int("mobile_data", 0) != 0;
}

void modem_data_set_enabled(bool on)
{
	ui_setting_set_int("mobile_data", on);
	printf("phone-ui: modem: mobile data %s\n", on ? "on" : "off");
}

bool modem_data_roaming(void)
{
	return ui_setting_get_int("data_roaming", 0) != 0;
}

void modem_data_set_roaming(bool on)
{
	ui_setting_set_int("data_roaming", on);
	printf("phone-ui: modem: data roaming %s\n", on ? "on" : "off");
}

/* Calls over 4G */

bool modem_volte_available(void)
{
	const char *ims = modem_config("ims");
	return ims && (strcmp(ims, "on") == 0 || strcmp(ims, "off") == 0) && state != MODEM_NONE;
}

bool modem_volte_enabled(void)
{
	const char *ims = modem_config("ims");
	return ui_setting_get_int("volte", ims && strcmp(ims, "on") == 0) != 0;
}

void modem_volte_set_enabled(bool on)
{
	ui_setting_set_int("volte", on);
	printf("phone-ui: modem: calls over 4G %s\n", on ? "on" : "off");
}

const char *modem_data_apn(void)
{
	return ui_setting_get("apn", "");
}

void modem_data_set_apn(const char *apn)
{
	char kept[64];
	size_t n = 0;
	for (; apn && *apn && n + 1 < sizeof(kept); apn++)
		if (strchr(MODEM_APN_CHARS, *apn))
			kept[n++] = *apn;
	kept[n] = '\0';
	if (strcmp(kept, modem_data_apn()) == 0)
		return;
	ui_setting_set("apn", kept);
	printf("phone-ui: modem: APN %s\n", kept[0] ? kept : "automatic");
}

enum modem_data modem_data_state(void)
{
	return modem_data_enabled() ? LV_MAX(data.state, MODEM_DATA_WAITING) : MODEM_DATA_OFF;
}

bool modem_data_wanted(void)
{
	return modem_data_enabled() && state == MODEM_READY &&
	       (network == MODEM_NET_HOME || (network == MODEM_NET_ROAMING && modem_data_roaming()));
}

/* A board script to run with `action` (and `arg`, NULL none), then `done`; twice in a
 * row: once, with the last one's `arg` */
static void script_queue(const char *script, const char *action, const char *arg, void (*done)(void))
{
	typeof(scripts.queue[0]) *last = scripts.queued ? &scripts.queue[scripts.queued - 1] : NULL;
	if (!last || last->script != script || last->action != action) {
		if (scripts.queued == SCRIPT_QUEUE)
			return;
		last = &scripts.queue[scripts.queued++];
	}
	last->script = script;
	last->action = action;
	last->done = done;
	snprintf(last->arg, sizeof(last->arg), "%s", arg ? arg : "");
}

void modem_data_script(const char *action)
{
	if (data.script)
		script_queue(data.script, action, data.iface, NULL);
}

/* The data interface up (IFF_UP in its sysfs flags), as the board's script leaves it */
static bool iface_up(void)
{
	char path[64 + IF_NAMESIZE], flags[32] = "";
	snprintf(path, sizeof(path), "/sys/class/net/%s/flags", data.iface ? data.iface : "");
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(flags, sizeof(flags), f))
			flags[0] = '\0';
		fclose(f);
	}
	return strtoul(flags, NULL, 16) & IFF_UP;
}

bool modem_data_left(bool up)
{
	if (up && !iface_up()) {
		printf("phone-ui: modem: mobile data: the session an earlier run left has its interface down\n");
		up = false;
	}
	if (!up) {
		modem_data_script("down");
		return false;
	}
	printf("phone-ui: modem: mobile data: connected (the session an earlier run left)\n");
	data.state = MODEM_DATA_CONNECTED;
	return true;
}

void modem_set_data(enum modem_data s)
{
	static const char *const names[] = { "off", "waiting", "connecting", "connected" };
	if (s == data.state)
		return;
	printf("phone-ui: modem: mobile data: %s\n", names[s]);
	if (s == MODEM_DATA_CONNECTED)
		modem_data_script("up");
	else if (data.state == MODEM_DATA_CONNECTED)
		modem_data_script("down");
	data.state = s;
}

/* The scripts: the one running reaped, the next started */
/* The scripts: the one running reaped, the next started */
static void script_tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	int status;
	if (scripts.pid > 0) {
		if (waitpid(scripts.pid, &status, WNOHANG) == 0)
			return;
		scripts.pid = 0;
		if (scripts.done)
			scripts.done();
	}
	if (!scripts.queued)
		return;
	const char *script = scripts.queue[0].script, *action = scripts.queue[0].action;
	char arg[sizeof(scripts.queue[0].arg)];
	memcpy(arg, scripts.queue[0].arg, sizeof(arg));
	scripts.done = scripts.queue[0].done;
	memmove(scripts.queue, scripts.queue + 1, --scripts.queued * sizeof(scripts.queue[0]));
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		execl(script, script, action, arg[0] ? arg : NULL, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		printf("phone-ui: modem: cannot run %s\n", script);
		if (scripts.done)
			scripts.done();
	} else {
		scripts.pid = pid;
	}
	printf("phone-ui: modem: %s %s%s%s\n", script, action, arg[0] ? " " : "", arg);
}

void modem_set_network(enum modem_network n, const char *name)
{
	if (state != MODEM_READY)
		return;
	bool on = n == MODEM_NET_HOME || n == MODEM_NET_ROAMING;
	if (!on) {
		name = "";
		bars = -1;
		tech = MODEM_TECH_UNKNOWN;
	} else if (!name) {
		name = "";
	}
	if (n == network && strcmp(name, operator_name) == 0)
		return;
	network = n;
	snprintf(operator_name, sizeof(operator_name), "%s", name);
	texts_update();
	printf("phone-ui: modem: network: %s\n", summary);
}

const char *modem_config(const char *key)
{
	for (unsigned i = 0; i < key_count; i++)
		if (strcmp(keys[i][0], key) == 0)
			return keys[i][1];
	return NULL;
}

bool modem_config_has(const char *key, const char *word)
{
	const char *v = modem_config(key);
	size_t n = strlen(word);
	while (v && *v) {
		v += strspn(v, ", ");
		size_t len = strcspn(v, ", ");
		if (len == n && strncmp(v, word, n) == 0)
			return true;
		v += len;
	}
	return false;
}

/* "key=value" lines, spaces around both trimmed */
static bool config_load(void)
{
	FILE *f = fopen(MODEM_CONF, "r");
	if (!f)
		return false;
	char line[256];
	while (fgets(line, sizeof(line), f) && key_count < MAX_KEYS) {
		char *eq = strchr(line, '=');
		char *key = line + strspn(line, " \t");
		if (*key == '#' || !eq)
			continue;
		char *value = eq + 1;
		value += strspn(value, " \t");
		value[strcspn(value, "\r\n")] = '\0';
		for (char *e = value + strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t');)
			*--e = '\0';
		*eq = '\0';
		for (char *e = eq; e > key && (e[-1] == ' ' || e[-1] == '\t');)
			*--e = '\0';
		snprintf(keys[key_count][0], sizeof(keys[0][0]), "%s", key);
		snprintf(keys[key_count][1], sizeof(keys[0][1]), "%s", value);
		key_count++;
	}
	fclose(f);
	return true;
}

void modem_init(void)
{
	texts_update();
	if (!config_load()) {
		printf("phone-ui: modem: none (no " MODEM_CONF ")\n");
		return;
	}
	const char *name = modem_config("provider");
	data.iface = modem_config("data");
	data.script = modem_config("data_script");
	if (!data.iface)
		data.script = NULL;
	call.script = modem_config("audio_script");
	if (data.script || call.script)
		lv_timer_create(script_tick_cb, SCRIPT_TICK_MS, NULL);
	/* What an earlier run left up (phone-ui does not end them as it exits): the call
	 * audio; the data session, once the provider knows (modem_data_left()) */
	call_route("off");
	for (unsigned i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
		if (name && strcmp(providers[i]->name, name) == 0) {
			state = MODEM_ABSENT;
			provider = providers[i];
			if (!provider->start()) {
				state = MODEM_NONE;
				provider = NULL;
			}
			texts_update();
			return;
		}
	}
	printf("phone-ui: modem: no provider \"%s\" (" MODEM_CONF ")\n", name ? name : "");
}

/* SMS: through the provider, to and from the conversations (data.c) */

/* Why the network cannot be reached for SMS or USSD (the provider has it, `able`) */
static const char *unavailable(bool able)
{
	if (state == MODEM_NONE || !provider || !able)
		return N_("No modem");
	if (state != MODEM_READY)
		return texts[state][1];
	return registered() ? NULL : N_("No service");
}

const char *modem_sms_unavailable(void)
{
	return unavailable(provider && provider->sms_send);
}

unsigned modem_sms_send(const char *number, const char *text)
{
	static unsigned last;
	if (modem_sms_unavailable())
		return 0;
	unsigned id = ++last ? last : ++last;
	return provider->sms_send(id, number, text) ? id : 0;
}

bool modem_sms_received(const char *sender, time_t time, const char *text)
{
	unsigned version = data_version;
	bool kept = conversation_received(sender, time, text);
	if (data_version != version) { /* new, not listed again */
		printf("phone-ui: modem: SMS received\n"); /* not who from: logs get shared */
		ui_vibrate(400);
		notification_banner(sender);
	}
	return kept;
}

void modem_sms_sent(unsigned id, bool ok)
{
	printf("phone-ui: modem: SMS %s\n", ok ? "sent" : "not sent");
	conversation_sent(id, ok);
}

/* USSD: through the provider, its replies to the screen (ussd.c) */

const char *modem_ussd_unavailable(void)
{
	return unavailable(provider && provider->ussd_send);
}

bool modem_ussd_send(const char *text)
{
	return !modem_ussd_unavailable() && provider->ussd_send(text);
}

void modem_ussd_cancel(void)
{
	if (provider && provider->ussd_cancel)
		provider->ussd_cancel();
}

void modem_ussd_received(enum modem_ussd s, const char *text)
{
	/* NON-NLS-BEGIN: for the log */
	static const char *const names[] = { "reply", "reply, an answer wanted", "ended by the network",
	                                     "not supported", "no reply", "refused", "not sent" };
	/* NON-NLS-END */
	printf("phone-ui: modem: USSD %s\n", names[s]); /* not its text: logs get shared */
	ussd_received(s, text);
}

/* Calls: through the provider, logged, their sound routed by the board */

/* The audio script's route, when it changes */
/* The music's volume again once the call's sound is off: volume.c kept it for after */
static void call_route_off(void)
{
	ui_volume_call(false);
}

static void call_route(const char *route)
{
	if (!call.script)
		return;
	if (strcmp(route, "off") == 0) {
		if (!call.route || strcmp(call.route, route) != 0)
			script_queue(call.script, route, NULL, call_route_off);
		call.route = route;
		return;
	}
	ui_volume_call(true);
	int volume = ui_volume_call_level();
	if (call.route && strcmp(call.route, route) == 0 && call.volume == volume)
		return;
	char level[8];
	snprintf(level, sizeof(level), "%d", volume);
	call.route = route;
	call.volume = volume;
	script_queue(call.script, route, level, NULL);
}

void modem_call_volume_changed(void)
{
	if (call.route && strcmp(call.route, "off") != 0)
		call_route(call.route);
}

const char *modem_call_unavailable(void)
{
	return unavailable(provider && provider->call_dial);
}

enum modem_call modem_call_state(void)
{
	return call.state;
}

const char *modem_call_number(void)
{
	return call.number;
}

unsigned modem_call_seconds(void)
{
	return call.connected ? lv_tick_elaps(call.connected) / 1000 : 0;
}

bool modem_call_dial(const char *number)
{
	if (modem_call_unavailable() || call.state != MODEM_CALL_NONE || !provider->call_dial(number))
		return false;
	call.incoming = call.refused = false;
	modem_set_call(MODEM_CALL_DIALING, number, MODEM_CALL_ENDED);
	return true;
}

void modem_woke(void)
{
	if (provider && provider->woke)
		provider->woke();
}

bool modem_woke_pending(void)
{
	return provider && provider->woke_pending && provider->woke_pending();
}

void modem_call_answer(void)
{
	if (call.state == MODEM_CALL_INCOMING && provider)
		provider->call_answer();
}

void modem_call_hang_up(void)
{
	if (call.state == MODEM_CALL_NONE || !provider)
		return;
	call.refused = call.state == MODEM_CALL_INCOMING;
	provider->call_hang_up();
}

void modem_call_mute(bool on)
{
	if (call.state != MODEM_CALL_NONE && provider && provider->call_mute)
		provider->call_mute(on);
}

void modem_call_tone(char key)
{
	if (call.state == MODEM_CALL_ACTIVE && provider && provider->call_tone && key && strchr("0123456789*#", key))
		provider->call_tone(key);
}

static void call_route_output(void)
{
	static const char *const routes[] = { "earpiece", "speaker", "bluetooth" };
	if (call.state == MODEM_CALL_DIALING || call.state == MODEM_CALL_ALERTING || call.state == MODEM_CALL_ACTIVE)
		call_route(routes[call.output]);
}

void modem_call_output_set(enum modem_call_output output)
{
	if (output == MODEM_CALL_BLUETOOTH && !bt_call_audio())
		return;
	call.output = output;
	call_route_output();
}

enum modem_call_output modem_call_output(void)
{
	return call.output;
}

void modem_call_headset(bool connected)
{
	if (connected == (call.output == MODEM_CALL_BLUETOOTH) || call.state == MODEM_CALL_NONE)
		return;
	call.output = connected ? MODEM_CALL_BLUETOOTH : MODEM_CALL_EARPIECE;
	call_route_output();
	call_output_changed();
}

void modem_set_call(enum modem_call s, const char *number, enum modem_call_end end)
{
	/* NON-NLS-BEGIN: for the log */
	static const char *const names[] = { "ended", "dialing", "ringing there", "incoming", "connected" };
	/* NON-NLS-END */
	bool renumbered = number && strcmp(number, call.number) != 0;
	if (number)
		snprintf(call.number, sizeof(call.number), "%s", number);
	if (s == call.state) {
		if (renumbered && s != MODEM_CALL_NONE)
			call_changed(s, end); /* the caller's number, after RING */
		return;
	}
	printf("phone-ui: [%.3f] modem: call %s\n", log_time(), names[s]); /* not the number: logs get shared */
	if (call.state == MODEM_CALL_NONE) {
		call.start = time(NULL);
		call.connected = 0;
		call.incoming = s == MODEM_CALL_INCOMING;
		if (s == MODEM_CALL_INCOMING)
			call.refused = false;
		call.output = bt_call_audio() ? MODEM_CALL_BLUETOOTH : MODEM_CALL_EARPIECE;
	}
	if (s == MODEM_CALL_ACTIVE && !call.connected)
		call.connected = lv_tick_get() ? lv_tick_get() : 1;
	call.state = s;
	if (s == MODEM_CALL_NONE) {
		call_route("off");
		enum call_kind kind = !call.incoming ? CALL_OUTGOING :
		                      call.connected || call.refused ? CALL_INCOMING : CALL_MISSED;
		call_log_add(call.number[0] ? call.number : "", kind, call.start, modem_call_seconds());
		call.connected = 0;
	} else if (s != MODEM_CALL_INCOMING) {
		call_route_output();
	}
	call_changed(s, end);
	if (s == MODEM_CALL_INCOMING)
		ui_vibrate(400);
}
