/*
 * Settings, Storage, Install on the phone: the system running from a microSD card, and
 * its data, installed on the phone's storage (the eMMC) by the board's
 * EMMC_INSTALL_SCRIPT (`install`), Tow-Boot too when the eMMC has no bootloader. Offered
 * only while the script's `status` says the system runs from a card. Without the script
 * there is nothing to offer.
 *
 * The script prints each step as it starts ("step bootloader", "step system", "step
 * data"), "kept bootloader" when the eMMC had one, "done" at the end, "error: <why>"
 * (English, shown as it is) when it fails; it only reads the card. It runs on while the
 * screen is left: opened again, the screen shows where it is. Power does not put the
 * phone to sleep meanwhile (power.c).
 *
 * Once installed the phone is switched off for the card to come out, the card kept as
 * it is (it starts this system again when put back) or erased first (`erase-card`: it
 * no longer starts, for other uses).
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "settings.h"

#ifndef EMMC_INSTALL_SCRIPT
#define EMMC_INSTALL_SCRIPT "/etc/phone-ui/emmc-install.sh"
#endif
#define POLL_MS 100
#define STEPS 3

enum stage {
	IDLE,      /* not started */
	RUNNING,   /* installing */
	DONE,      /* installed */
	FAILED,    /* the script failed: reason */
	ERASING,   /* the card being erased, then switched off */
	POWERING,  /* poweroff run */
};

static const char *const step_names[STEPS] = { "bootloader", "system", "data" };
static const char *const step_titles[STEPS] = { N_("Bootloader"), N_("System"), N_("Your data") };

static struct {
	int offered;       /* the script's status said "card": 1, not: 0, not asked yet: -1 */
	enum stage stage;
	int step;          /* steps started, 0 to STEPS */
	bool kept;         /* the eMMC had a bootloader */
	bool erase_failed; /* the card could not be erased: not switched off */
	char reason[256];
	pid_t pid;         /* the script running, 0 none */
	int fd;            /* its output */
	char line[256];
	size_t len;
	lv_timer_t *timer;
	unsigned changes;  /* counts what the screen shows changing */
	lv_obj_t *page;    /* the screen's body, NULL while it is not open */
	unsigned shown;    /* changes it shows */
} in = { .offered = -1, .fd = -1 };

bool emmc_install_offered(void)
{
	if (in.offered < 0) {
		char status[32] = "";
		FILE *f = access(EMMC_INSTALL_SCRIPT, X_OK) == 0 ? popen(EMMC_INSTALL_SCRIPT " status", "r") : NULL;
		if (f) {
			if (!fgets(status, sizeof(status), f))
				status[0] = '\0';
			pclose(f);
		}
		in.offered = strcmp(status, "card\n") == 0;
	}
	return in.offered;
}

bool emmc_install_running(void)
{
	return in.stage == RUNNING || in.stage == ERASING || in.stage == POWERING;
}

static void line_got(const char *line)
{
	printf("phone-ui: emmc install: %s\n", line);
	if (strncmp(line, "step ", 5) == 0) {
		for (int i = 0; i < STEPS; i++)
			if (strcmp(line + 5, step_names[i]) == 0)
				in.step = i + 1;
	} else if (strcmp(line, "kept bootloader") == 0) {
		in.kept = true;
	} else if (strncmp(line, "error: ", 7) == 0) {
		snprintf(in.reason, sizeof(in.reason), "%.*s", (int)sizeof(in.reason) - 1, line + 7);
	} else {
		return;
	}
	in.changes++;
}

/* What the script printed since, line by line */
static void output_read(void)
{
	char buf[256];
	ssize_t n;
	while (in.fd >= 0 && (n = read(in.fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				in.line[in.len] = '\0';
				line_got(in.line);
				in.len = 0;
			} else if (in.len < sizeof(in.line) - 1) {
				in.line[in.len++] = buf[i];
			}
		}
	}
}

static void timer_cb(lv_timer_t *timer);

/* The script with `arg`, its output read as it comes; false if it could not start */
static bool run(const char *arg)
{
	int p[2];
	if (pipe(p) != 0)
		return false;
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], STDOUT_FILENO);
		dup2(p[1], STDERR_FILENO);
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0)
			dup2(null, STDIN_FILENO);
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		signal(SIGPIPE, SIG_DFL);
		execl(EMMC_INSTALL_SCRIPT, EMMC_INSTALL_SCRIPT, arg, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return false;
	}
	printf("phone-ui: emmc install: " EMMC_INSTALL_SCRIPT " %s\n", arg);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	in.pid = pid;
	in.fd = p[0];
	in.len = 0;
	if (!in.timer)
		in.timer = lv_timer_create(timer_cb, POLL_MS, NULL);
	return true;
}

static void power_off(void)
{
	in.stage = POWERING;
	in.changes++;
	printf("phone-ui: poweroff\n");
	if (system("poweroff") != 0) {
		printf("phone-ui: poweroff failed\n");
		in.stage = DONE;
	}
}

static void timer_cb(lv_timer_t *timer)
{
	output_read();
	int status = 0;
	pid_t done = waitpid(in.pid, &status, WNOHANG);
	if (done == 0)
		return;
	output_read();
	close(in.fd);
	in.fd = -1;
	in.pid = 0;
	lv_timer_delete(timer);
	in.timer = NULL;
	bool ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (in.stage == RUNNING) {
		ui_cpu_boost(false);
		in.stage = ok ? DONE : FAILED;
		if (!ok && !in.reason[0])
			snprintf(in.reason, sizeof(in.reason), "the installer stopped (%d)", /* NON-NLS */
			         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		in.changes++;
	} else if (in.stage == ERASING) {
		in.erase_failed = !ok;
		if (ok) {
			power_off();
		} else {
			in.stage = DONE;
			in.changes++;
		}
	}
}

static void install_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	in.step = 0;
	in.kept = false;
	in.reason[0] = '\0';
	in.stage = RUNNING;
	ui_cpu_boost(true);
	if (!run("install")) {
		ui_cpu_boost(false);
		in.stage = FAILED;
		snprintf(in.reason, sizeof(in.reason), "cannot run " EMMC_INSTALL_SCRIPT); /* NON-NLS */
	}
	in.changes++;
}

static void switch_off_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	power_off();
}

static void erase_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	in.erase_failed = false;
	in.stage = ERASING;
	if (!run("erase-card")) {
		in.erase_failed = true;
		in.stage = DONE;
	}
	in.changes++;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, s);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void button(lv_obj_t *parent, const char *s, uint32_t color, uint32_t pressed, lv_event_cb_t cb)
{
	lv_obj_t *b = ui_button(parent, LV_PCT(100), 88, color, pressed, s, cb, NULL);
	lv_obj_set_style_margin_top(b, 8, 0);
}

/* The steps, each with how far it went */
static void steps_card(lv_obj_t *page)
{
	lv_obj_t *card = ui_card_create(page);
	for (int i = 0; i < STEPS; i++) {
		lv_obj_t *value = settings_info_row(card, _(step_titles[i]));
		if (i == 0 && in.kept && in.step > 0)
			ui_set_text(value, _("Kept"));
		else if (i + 1 < in.step || (i + 1 == in.step && in.stage != RUNNING && in.stage != FAILED))
			ui_set_text(value, _("Done"));
		else if (i + 1 == in.step && in.stage == RUNNING)
			ui_set_text(value, _("Installing..."));
		else if (i + 1 == in.step)
			ui_set_text(value, _("Failed"));
	}
}

static void build(lv_obj_t *page)
{
	lv_obj_clean(page);
	if (in.stage != IDLE)
		steps_card(page);
	lv_obj_t *panel = settings_panel(page);
	switch (in.stage) {
	case IDLE:
		text(panel, _("The system and your data are copied to the phone's storage (eMMC), with Tow-Boot, which starts the phone, if it has none. The phone then starts without the card, faster."),
		     UI_COLOR_TEXT);
		text(panel, _("Everything else on the phone's storage is erased: another system, its files, photos and messages."),
		     UI_COLOR_ORANGE);
		button(panel, _("Erase and install"), UI_COLOR_RED, UI_COLOR_RED_PRESSED, install_cb);
		break;
	case RUNNING:
		text(panel, _("Keep the card in and the phone on until it is done."), UI_COLOR_TEXT);
		break;
	case DONE:
		settings_title_line(panel, _("Installed"));
		text(panel, _("Switch the phone off and take the card out: it then starts from its own storage. Keep the card as it is, or erase it to use it for something else."),
		     UI_COLOR_TEXT);
		if (in.erase_failed)
			text(panel, _("The card could not be erased."), UI_COLOR_ORANGE);
		button(panel, _("Switch off"), UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, switch_off_cb);
		button(panel, _("Erase the card and switch off"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, erase_cb);
		break;
	case FAILED:
		settings_title_line(panel, _("Installing failed"));
		text(panel, in.reason, UI_COLOR_DIM);
		text(panel, _("The card is unchanged. If the phone's storage was being written, it may not start until installed again."),
		     UI_COLOR_TEXT);
		button(panel, _("Try again"), UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, install_cb);
		break;
	case ERASING:
	case POWERING:
		text(panel, _("Powering off..."), UI_COLOR_TEXT);
		break;
	}
	in.shown = in.changes;
}

static void refresh_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (in.page && in.shown != in.changes)
		build(in.page);
}

static void page_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	in.page = NULL;
}

void emmc_install_open(void)
{
	lv_obj_t *scr = settings_page(_("Install on the phone"), &in.page);
	lv_obj_add_event_cb(scr, page_delete_cb, LV_EVENT_DELETE, NULL);
	build(in.page);
	settings_timer(scr, refresh_cb, POLL_MS);
	ui_push(scr);
}
