/*
 * The apps' folders (folders.c): where they keep what a user puts on the phone, shared
 * over MTP and listed first in Files. The built-in apps' and those external apps declare
 * in app.json ("folders"), made at startup and written for the board's USB mode script.
 */

#ifndef PHONE_UI_FOLDERS_H
#define PHONE_UI_FOLDERS_H

#include <stdbool.h>

struct app_folder {
	const char *path; /* absolute, under /var/lib */
	const char *name; /* in English, as MTP shows it; Files shows _(name) */
	const char *app;  /* the app's name, for its icon (ui_app_find()) */
};

/* An external app's folder, `path` under /var/lib: false (and logged) if refused, out of
 * /var/lib or what umtprd.conf cannot quote */
bool folders_add(const char *app, const char *path, const char *name);
/* Once all apps are found: each folder made, the list written (FOLDERS_LIST) */
void folders_publish(void);
const struct app_folder *folders_get(unsigned *count);

#endif
