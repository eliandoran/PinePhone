/*
 * Settings: key=value lines in /var/lib/phone-ui/settings, loaded once, written back
 * on every change (they change rarely).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui.h"

#define SETTINGS_DIR "/var/lib/phone-ui"
#define SETTINGS SETTINGS_DIR "/settings"

static struct setting {
	char *key, *value;
} *settings;
static unsigned setting_count;
static bool settings_loaded;

static struct setting *setting_find(const char *key)
{
	if (!settings_loaded) {
		settings_loaded = true;
		FILE *f = fopen(SETTINGS, "r");
		char line[512];
		while (f && fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = '\0';
			eq[strcspn(eq + 1, "\n") + 1] = '\0';
			settings = realloc(settings, sizeof(*settings) * (setting_count + 1));
			settings[setting_count].key = strdup(line);
			settings[setting_count++].value = strdup(eq + 1);
		}
		if (f)
			fclose(f);
	}
	for (unsigned i = 0; i < setting_count; i++)
		if (strcmp(settings[i].key, key) == 0)
			return &settings[i];
	return NULL;
}

const char *ui_setting_get(const char *key, const char *fallback)
{
	struct setting *s = setting_find(key);
	return s ? s->value : fallback;
}

int ui_setting_get_int(const char *key, int fallback)
{
	const char *v = ui_setting_get(key, NULL);
	return v ? atoi(v) : fallback;
}

void ui_setting_set(const char *key, const char *value)
{
	struct setting *s = setting_find(key);
	if (s && strcmp(s->value, value) == 0)
		return;
	if (s) {
		free(s->value);
		s->value = strdup(value);
	} else {
		settings = realloc(settings, sizeof(*settings) * (setting_count + 1));
		settings[setting_count].key = strdup(key);
		settings[setting_count++].value = strdup(value);
	}

	mkdir(SETTINGS_DIR, 0755);
	FILE *f = fopen(SETTINGS ".new", "w");
	if (!f) {
		printf("phone-ui: cannot save settings: %s\n", strerror(errno));
		return;
	}
	for (unsigned i = 0; i < setting_count; i++)
		fprintf(f, "%s=%s\n", settings[i].key, settings[i].value);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(SETTINGS ".new", SETTINGS);
}

void ui_setting_set_int(const char *key, int value)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	ui_setting_set(key, buf);
}
