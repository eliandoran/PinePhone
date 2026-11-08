/*
 * A plugin app for test_plugins.c, fake.so: what phone-ui looks for in one (external.c),
 * but its app_abi is FAKE_PLUGIN_ABI's (the environment, read as it is loaded) rather than
 * the headers' it was built with: a test loads one built for other headers without
 * building one. Opened, it shows a screen saying so.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ui.h"

char app_abi[16384];

__attribute__((constructor)) static void abi_init(void)
{
	const char *abi = getenv("FAKE_PLUGIN_ABI");
	snprintf(app_abi, sizeof(app_abi), "%s", abi ? abi : "");
}

/* Once all apps are found (app_start()): a line in /tmp/fake-plugin-started each time */
void app_start(void)
{
	FILE *f = fopen("/tmp/fake-plugin-started", "a");
	if (f) {
		fputs("started\n", f);
		fclose(f);
	}
}

void app_open(void)
{
	lv_obj_t *body;
	lv_obj_t *screen = ui_app_screen_create("Fake", NULL, &body);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text(label, "The fake plugin runs"); /* NON-NLS */
	ui_push(screen);
}
