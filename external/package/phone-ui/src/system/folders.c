/*
 * The apps' folders: the built-in apps' below, then those external apps declare
 * (external.c), sorted by app. Each is made at startup, so that Files and a computer
 * find it before the app wrote anything, and the list is written as umtprd.conf's
 * storage lines to FOLDERS_LIST, which the board's usb-mode.sh adds to its
 * umtprd.conf: a computer sees these folders over MTP, not the rest of /var/lib
 * (Wi-Fi passwords, messages, SSH keys).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "i18n.h"
#include "system/folders.h"

#ifndef FOLDERS_LIST
#define FOLDERS_LIST "/run/phone-ui.folders"
#endif

#define VAR_LIB "/var/lib/"

static const struct app_folder builtin[] = {
	{ "/var/lib/photos", N_("Photos"), "Gallery" }, /* NON-NLS: the paths and apps */
	{ "/var/lib/music", N_("Music"), "Music" },     /* NON-NLS */
	{ "/var/lib/books", N_("Books"), "Books" },     /* NON-NLS */
	{ "/var/lib/notes", N_("Notes"), "Notes" },     /* NON-NLS */
	{ "/var/lib/memos", N_("Memos"), "Recorder" },  /* NON-NLS */
};
#define BUILTIN (sizeof(builtin) / sizeof(builtin[0]))

static struct app_folder *folders;
static unsigned count;

/* What a storage line can hold between its quotes, and one line */
static bool quotable(const char *s)
{
	return *s && !strpbrk(s, "\"\\\n\r");
}

bool folders_add(const char *app, const char *path, const char *name)
{
	/* Under /var/lib: relative, no "." or ".." */
	bool ok = path && quotable(path) && path[0] != '/' && name && quotable(name);
	for (const char *p = path; ok; p++) {
		size_t n = strcspn(p, "/");
		ok = n && strncmp(p, ".", n) != 0 && strncmp(p, "..", n) != 0;
		p += n;
		if (!*p)
			break;
	}
	if (!ok) {
		printf("phone-ui: %s: folder \"%s\" refused (a folder under /var/lib)\n", app,
		       path ? path : "");
		return false;
	}
	if (!folders) {
		folders = malloc(BUILTIN * sizeof(*folders));
		memcpy(folders, builtin, sizeof(builtin));
		count = BUILTIN;
	}
	char *full = malloc(strlen(VAR_LIB) + strlen(path) + 1);
	strcpy(full, VAR_LIB);
	strcat(full, path);
	folders = realloc(folders, (count + 1) * sizeof(*folders));
	folders[count++] = (struct app_folder){ full, strdup(name), strdup(app) };
	return true;
}

/* The external apps' by app (found in the folder's order), each app's in its order */
static void sort_by_app(struct app_folder *list, unsigned n)
{
	for (unsigned i = 1; i < n; i++) {
		struct app_folder f = list[i];
		unsigned j = i;
		for (; j > 0 && strcasecmp(list[j - 1].app, f.app) > 0; j--)
			list[j] = list[j - 1];
		list[j] = f;
	}
}

/* `path` and its parents */
static void mkdir_p(const char *path)
{
	char buf[4096];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(buf, 0755);
			*p = '/';
		}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST)
		printf("phone-ui: %s: %s\n", path, strerror(errno));
}

void folders_publish(void)
{
	if (folders)
		sort_by_app(folders + BUILTIN, count - BUILTIN);
	unsigned n;
	const struct app_folder *list = folders_get(&n);
	FILE *f = fopen(FOLDERS_LIST ".new", "w");
	for (unsigned i = 0; i < n; i++) {
		mkdir_p(list[i].path);
		if (f)
			fprintf(f, "storage \"%s\" \"%s\" \"rw\"\n", list[i].path, list[i].name);
	}
	if (f && fclose(f) == 0)
		rename(FOLDERS_LIST ".new", FOLDERS_LIST);
}

const struct app_folder *folders_get(unsigned *n)
{
	*n = folders ? count : BUILTIN;
	return folders ? folders : builtin;
}
