/*
 * Small files (sysfs attributes) and the vibrator.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ui.h"

bool ui_haptics = true;

bool ui_read_line(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

bool ui_read_long(const char *path, long *v)
{
	char buf[32];
	char *end;
	if (!ui_read_line(path, buf, sizeof(buf)))
		return false;
	*v = strtol(buf, &end, 10);
	return end != buf;
}

void ui_write_long(const char *path, long v)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%ld\n", v);
	fclose(f);
}

/* Vibration: the gpio-vibrator input device, as a force feedback rumble effect */

void ui_vibrate(uint32_t ms)
{
	static int fd = -2;
	static struct ff_effect effect = { .type = FF_RUMBLE, .id = -1 };

	if (!ui_haptics)
		return;
	if (fd == -2) {
		fd = -1;
		DIR *dir = opendir("/dev/input");
		struct dirent *ent;
		while (dir && fd < 0 && (ent = readdir(dir))) {
			char path[300], name[64] = "";
			if (strncmp(ent->d_name, "event", 5) != 0)
				continue;
			snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
			int f = open(path, O_RDWR | O_CLOEXEC);
			if (f >= 0 && ioctl(f, EVIOCGNAME(sizeof(name)), name) >= 0 &&
			    strcmp(name, "gpio-vibrator") == 0)
				fd = f;
			else if (f >= 0)
				close(f);
		}
		if (dir)
			closedir(dir);
		if (fd < 0)
			printf("phone-ui: no vibrator\n");
	}
	if (fd < 0)
		return;

	effect.u.rumble.strong_magnitude = 0xffff;
	effect.replay.length = (uint16_t)ms;
	if (ioctl(fd, EVIOCSFF, &effect) < 0)
		return;
	struct input_event play = { .type = EV_FF, .code = (uint16_t)effect.id, .value = 1 };
	if (write(fd, &play, sizeof(play)) < 0)
		printf("phone-ui: vibrate: %s\n", strerror(errno));
}
