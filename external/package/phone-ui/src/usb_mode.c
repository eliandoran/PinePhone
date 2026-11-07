/*
 * File transfer over USB (MTP), as Android's "Use USB for: File transfer": the board's
 * USB_MODE_SCRIPT switches its USB gadget, `usb-mode.sh mtp` or `default` (on the
 * PinePhone MTP takes the serial console's place, the network stays). Without the script
 * there is no file transfer.
 *
 * It goes back to the default mode when the cable is pulled out, as Android's does. An
 * earlier phone-ui's file transfer (`usb-mode.sh status`) is kept while the cable is in:
 * restarting phone-ui (`just ui`) does not cut a computer off. The script refuses while the
 * system it runs on is on trial (pp-update), and says so: the toggle stays off.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui.h"
#include "usb_mode.h"

#ifndef USB_MODE_SCRIPT
#define USB_MODE_SCRIPT "/etc/phone-ui/usb-mode.sh"
#endif
#define UDC "/sys/class/udc"
#define BUSY_MS 100   /* the script's end looked for */
#define ON_MS 1000    /* the cable looked for while on */

static struct {
	bool on;          /* what was last asked */
	pid_t pid;        /* the script running, 0 none */
	lv_timer_t *timer; /* while running or on */
} usb;

bool usb_mode_available(void)
{
	return access(USB_MODE_SCRIPT, X_OK) == 0;
}

bool usb_computer(void)
{
	DIR *dir = opendir(UDC);
	struct dirent *ent;
	char path[300], state[32];
	bool configured = false;
	while (dir && !configured && (ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), UDC "/%s/state", ent->d_name);
		configured = ui_read_line(path, state, sizeof(state)) && strcmp(state, "configured") == 0;
	}
	if (dir)
		closedir(dir);
	return configured;
}

bool usb_file_transfer(void)
{
	return usb.on;
}

bool usb_mode_busy(void)
{
	return usb.pid > 0;
}

static void timer_cb(lv_timer_t *timer);

/* The timer while there is something to look for, at its pace */
static void timer_update(void)
{
	if (!usb.pid && !usb.on) {
		if (usb.timer)
			lv_timer_delete(usb.timer);
		usb.timer = NULL;
		return;
	}
	uint32_t period = usb.pid ? BUSY_MS : ON_MS;
	if (!usb.timer)
		usb.timer = lv_timer_create(timer_cb, period, NULL);
	else
		lv_timer_set_period(usb.timer, period);
}

static void run(bool on)
{
	const char *mode = on ? "mtp" : "default";
	usb.on = on;
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		execl(USB_MODE_SCRIPT, USB_MODE_SCRIPT, mode, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		printf("phone-ui: usb: cannot run " USB_MODE_SCRIPT "\n");
		usb.on = false;
	} else {
		printf("phone-ui: usb: " USB_MODE_SCRIPT " %s\n", mode);
		usb.pid = pid;
	}
	timer_update();
}

static void timer_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	int status = 0;
	if (usb.pid > 0) {
		pid_t done = waitpid(usb.pid, &status, WNOHANG);
		if (done == 0)
			return;
		usb.pid = 0;
		if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("phone-ui: usb: the switch failed, file transfer off\n");
			usb.on = false;
		}
	}
	/* Unplugged: back to the default mode, as at the next plug the computer would
	 * otherwise find the files without the phone having been asked */
	if (usb.on && !usb.pid && !ui_usb_powered()) {
		printf("phone-ui: usb: unplugged\n");
		run(false);
		return;
	}
	timer_update();
}

void usb_file_transfer_set(bool on)
{
	if (on == usb.on || usb.pid || !usb_mode_available())
		return;
	run(on);
}

/* An earlier phone-ui's mode (`just ui`, a crash): kept while the cable is in, so a
 * restart does not take the files from the computer; back to the default otherwise */
void usb_mode_init(void)
{
	if (!usb_mode_available())
		return;
	char mode[64] = "";
	FILE *f = popen(USB_MODE_SCRIPT " status", "r");
	if (f) {
		if (!fgets(mode, sizeof(mode), f))
			mode[0] = '\0';
		pclose(f);
	}
	if (strncmp(mode, "mtp ", 4) != 0)
		return;
	if (ui_usb_powered()) {
		printf("phone-ui: usb: file transfer on (an earlier run's)\n");
		usb.on = true;
		timer_update();
	} else {
		run(false);
	}
}
