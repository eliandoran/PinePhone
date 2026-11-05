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
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "modem_provider.h"

#ifndef MODEM_CONF
#define MODEM_CONF "/etc/phone-ui/modem.conf"
#endif
#define MAX_KEYS 16

static const struct modem_provider *const providers[] = { &modem_at };

static enum modem_state state;
static enum modem_network network;
static char operator_name[64];
static int bars = -1;           /* while registered */
static enum modem_tech tech;
static char status_text[80], summary[96];
static char keys[MAX_KEYS][2][128];
static unsigned key_count;

static const char *const texts[][2] = {
	[MODEM_NONE] = { "", "Not available" },
	[MODEM_ABSENT] = { "No modem", "No modem" },
	[MODEM_STARTING] = { "Modem...", "Starting" },
	[MODEM_NO_SIM] = { "No SIM", "No SIM card" },
	[MODEM_SIM_PIN] = { "SIM locked", "SIM locked (PIN)" },
	[MODEM_SIM_PUK] = { "SIM blocked", "SIM blocked (PUK)" },
	[MODEM_SIM_ERROR] = { "SIM error", "SIM error" },
	[MODEM_READY] = { "SIM ready", "SIM ready" }, /* the network's state not known yet */
};

static const char *const network_texts[][2] = {
	[MODEM_NET_NONE] = { "No service", "No service" },
	[MODEM_NET_SEARCHING] = { "Searching...", "Searching" },
	[MODEM_NET_DENIED] = { "No service", "No service (denied)" },
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

/* The texts of the state now, in status_text and summary */
static void texts_update(void)
{
	const char *name = operator_name[0] ? operator_name : "Registered";
	if (state != MODEM_READY || network == MODEM_NET_UNKNOWN) {
		snprintf(status_text, sizeof(status_text), "%s", texts[state][0]);
		snprintf(summary, sizeof(summary), "%s", texts[state][1]);
	} else if (network == MODEM_NET_HOME || network == MODEM_NET_ROAMING) {
		status_text[0] = '\0'; /* all is well: the name in the shade, not the status bar */
		snprintf(summary, sizeof(summary), network == MODEM_NET_ROAMING ? "%s (roaming)" : "%s", name);
	} else {
		snprintf(status_text, sizeof(status_text), "%s", network_texts[network][0]);
		snprintf(summary, sizeof(summary), "%s", network_texts[network][1]);
	}
}

const char *modem_status_text(void)
{
	return status_text;
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
	if (s != MODEM_READY) {
		network = MODEM_NET_UNKNOWN;
		operator_name[0] = '\0';
		bars = -1;
		tech = MODEM_TECH_UNKNOWN;
	}
	texts_update();
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
