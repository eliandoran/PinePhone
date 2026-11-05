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
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static enum modem_state state;
static enum modem_network network;
static char operator_name[64];
static char imei[32], number[32];
static int bars = -1;           /* while registered */
static enum modem_tech tech;
static const char *status_icon = "";
static char summary[96];
static char keys[MAX_KEYS][2][128];
static unsigned key_count;

static struct {
	const char *iface, *script; /* NULL: no mobile data */
	enum modem_data state;      /* the provider's, while on */
	const char *queue[SCRIPT_QUEUE];
	unsigned queued;
	pid_t pid;                  /* the script running, 0 none */
} data;

/* Each state: its status bar symbol ("" none: the bars unlit say it), its text */
static const char *const texts[][2] = {
	[MODEM_NONE] = { "", "Not available" },
	[MODEM_ABSENT] = { LV_SYMBOL_WARNING, "No modem" },
	[MODEM_STARTING] = { "", "Starting" },
	[MODEM_NO_SIM] = { UI_SYMBOL_SIM_CARD, "No SIM card" },
	[MODEM_SIM_PIN] = { UI_SYMBOL_LOCK, "SIM locked (PIN)" },
	[MODEM_SIM_PUK] = { UI_SYMBOL_LOCK, "SIM blocked (PUK)" },
	[MODEM_SIM_ERROR] = { LV_SYMBOL_WARNING, "SIM error" },
	[MODEM_READY] = { "", "SIM ready" }, /* the network's state not known yet */
};

static const char *const network_texts[][2] = {
	[MODEM_NET_NONE] = { LV_SYMBOL_CLOSE, "No service" },
	[MODEM_NET_SEARCHING] = { "", "Searching" },
	[MODEM_NET_DENIED] = { LV_SYMBOL_CLOSE, "No service (denied)" },
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

/* The state now, in status_icon and summary */
static void texts_update(void)
{
	const char *name = operator_name[0] ? operator_name : "Registered";
	if (state != MODEM_READY || network == MODEM_NET_UNKNOWN) {
		status_icon = texts[state][0];
		snprintf(summary, sizeof(summary), "%s", texts[state][1]);
	} else if (network == MODEM_NET_HOME || network == MODEM_NET_ROAMING) {
		status_icon = ""; /* all is well: the bars, the name in the shade */
		snprintf(summary, sizeof(summary), network == MODEM_NET_ROAMING ? "%s (roaming)" : "%s", name);
	} else {
		status_icon = network_texts[network][0];
		snprintf(summary, sizeof(summary), "%s", network_texts[network][1]);
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
		number[0] = '\0';
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
	return number;
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
	if (strcmp(s, number) != 0) /* not the number itself: logs get shared */
		printf("phone-ui: modem: the SIM's number %s\n", s[0] ? "known" : "not on it");
	snprintf(number, sizeof(number), "%s", s);
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

void modem_data_script(const char *action)
{
	if (!data.script)
		return;
	if (data.queued && data.queue[data.queued - 1] == action)
		return; /* twice in a row: once */
	if (data.queued < SCRIPT_QUEUE)
		data.queue[data.queued++] = action;
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
static void script_tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	int status;
	if (data.pid > 0) {
		if (waitpid(data.pid, &status, WNOHANG) == 0)
			return;
		data.pid = 0;
	}
	if (!data.queued)
		return;
	const char *action = data.queue[0];
	memmove(data.queue, data.queue + 1, --data.queued * sizeof(data.queue[0]));
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		execl(data.script, data.script, action, data.iface, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		printf("phone-ui: modem: cannot run %s\n", data.script);
	else
		data.pid = pid;
	printf("phone-ui: modem: %s %s %s\n", data.script, action, data.iface);
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
	if (data.iface && data.script) {
		lv_timer_create(script_tick_cb, SCRIPT_TICK_MS, NULL);
		/* What an earlier run left up (phone-ui does not end it as it exits) */
		modem_data_script("down");
	}
	for (unsigned i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
		if (name && strcmp(providers[i]->name, name) == 0) {
			state = MODEM_ABSENT;
			if (!providers[i]->start())
				state = MODEM_NONE;
			texts_update();
			return;
		}
	}
	printf("phone-ui: modem: no provider \"%s\" (" MODEM_CONF ")\n", name ? name : "");
}
