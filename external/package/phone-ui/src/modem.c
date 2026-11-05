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

#include <stdio.h>
#include <string.h>

#include "modem_provider.h"

#ifndef MODEM_CONF
#define MODEM_CONF "/etc/phone-ui/modem.conf"
#endif
#define MAX_KEYS 16

static const struct modem_provider *const providers[] = { &modem_at };

static enum modem_state state;
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
	[MODEM_READY] = { "SIM ready", "SIM ready" },
};

enum modem_state modem_state(void)
{
	return state;
}

const char *modem_status_text(void)
{
	return texts[state][0];
}

const char *modem_summary(void)
{
	return texts[state][1];
}

void modem_set_state(enum modem_state s)
{
	if (s != state)
		printf("phone-ui: modem: %s\n", texts[s][1]);
	state = s;
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
			return;
		}
	}
	printf("phone-ui: modem: no provider \"%s\" (" MODEM_CONF ")\n", name ? name : "");
}
