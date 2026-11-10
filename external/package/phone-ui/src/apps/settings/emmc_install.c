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
 * Once installed the phone restarts into it: Tow-Boot starts a system on the eMMC before
 * the card's, so the card may stay in. Running from the eMMC with that card in (the
 * script's `card-status`), Storage offers to erase it (`erase-card`: it no longer starts,
 * for other uses), once asked to confirm.
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
	IDLE,       /* not started */
	RUNNING,    /* installing */
	DONE,       /* installed */
	FAILED,     /* the script failed: reason */
	RESTARTING, /* reboot run */
};

static const char *const step_names[STEPS] = { "bootloader", "system", "data" };
static const char *const step_titles[STEPS] = { N_("Bootloader"), N_("System"), N_("Your data") };

static struct {
	int offered;       /* the script's status said "card": 1, not: 0, not asked yet: -1 */
	bool erasing;      /* the script runs erase-card, not install */
	enum stage stage;
	int step;          /* steps started, 0 to STEPS */
	bool kept;         /* the eMMC had a bootloader */
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

/* The card of this system, from the eMMC (Storage's panel) */
enum card_state {
	CARD_OFFERED,  /* Erase the card */
	CARD_CONFIRM,  /* asked */
	CARD_ERASING,
	CARD_ERASED,
	CARD_FAILED,   /* in.reason */
};

static struct {
	int offered;       /* the script's card-status said "system": 1, not: 0, not asked yet: -1 */
	enum card_state state;
	lv_obj_t *panel;   /* Storage's, NULL while it is not open */
	unsigned changes, shown;
} sd = { .offered = -1 };

/* The script's answer to `arg` is `answer` (a line) */
static bool script_says(const char *arg, const char *answer)
{
	char line[32] = "", cmd[128];
	snprintf(cmd, sizeof(cmd), EMMC_INSTALL_SCRIPT " %s", arg);
	FILE *f = access(EMMC_INSTALL_SCRIPT, X_OK) == 0 ? popen(cmd, "r") : NULL;
	if (f) {
		if (!fgets(line, sizeof(line), f))
			line[0] = '\0';
		pclose(f);
	}
	line[strcspn(line, "\n")] = '\0';
	return strcmp(line, answer) == 0;
}

bool emmc_install_offered(void)
{
	if (in.offered < 0)
		in.offered = script_says("status", "card");
	return in.offered;
}

bool emmc_install_running(void)
{
	return in.stage == RUNNING || in.stage == RESTARTING;
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

static void restart(void)
{
	in.stage = RESTARTING;
	in.changes++;
	printf("phone-ui: reboot\n");
	if (system("reboot") != 0) {
		printf("phone-ui: reboot failed\n");
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
	if (in.erasing) {
		in.erasing = false;
		sd.state = ok ? CARD_ERASED : CARD_FAILED;
		if (ok)
			sd.offered = 0; /* Storage opened again shows no panel */
		sd.changes++;
	} else if (in.stage == RUNNING) {
		ui_cpu_boost(false);
		in.stage = ok ? DONE : FAILED;
		if (!ok && !in.reason[0])
			snprintf(in.reason, sizeof(in.reason), "the installer stopped (%d)", /* NON-NLS */
			         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		in.changes++;
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

static void restart_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	restart();
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
		text(panel, _("The phone now starts from its own storage, with the card in or not. Once restarted, Settings › Storage can erase the card, to use it for something else."),
		     UI_COLOR_TEXT);
		button(panel, _("Restart"), UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, restart_cb);
		break;
	case FAILED:
		settings_title_line(panel, _("Installing failed"));
		text(panel, in.reason, UI_COLOR_DIM);
		text(panel, _("The card is unchanged. If the phone's storage was being written, it may not start until installed again."),
		     UI_COLOR_TEXT);
		button(panel, _("Try again"), UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, install_cb);
		break;
	case RESTARTING:
		text(panel, _("Restarting..."), UI_COLOR_TEXT);
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

/* Storage's panel for the card of this system, from the eMMC */

bool emmc_card_offered(void)
{
	if (sd.offered < 0)
		sd.offered = script_says("card-status", "system");
	return sd.offered;
}

static void card_set(enum card_state state)
{
	sd.state = state;
	sd.changes++;
}

static void card_erase_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	in.reason[0] = '\0';
	in.erasing = true;
	card_set(CARD_ERASING);
	if (!run("erase-card")) {
		in.erasing = false;
		card_set(CARD_FAILED);
	}
}

static void card_ask_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	card_set(CARD_CONFIRM);
}

static void card_cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	card_set(CARD_OFFERED);
}

static void card_build(lv_obj_t *panel)
{
	lv_obj_clean(panel);
	settings_title_line(panel, _("microSD card"));
	switch (sd.state) {
	case CARD_OFFERED:
	case CARD_CONFIRM:
		text(panel, _("The microSD card holds a copy of this system. Erase it to use it for something else."),
		     UI_COLOR_TEXT);
		if (sd.state == CARD_OFFERED) {
			button(panel, _("Erase the card"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, card_ask_cb);
			break;
		}
		text(panel, _("Everything on the card is erased."), UI_COLOR_ORANGE);
		lv_obj_t *row = ui_box(panel);
		lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(row, 16, 0);
		lv_obj_set_style_pad_top(row, 8, 0);
		lv_obj_t *no = ui_button(row, 1, 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Cancel"), card_cancel_cb,
		                         NULL);
		lv_obj_set_flex_grow(no, 1);
		lv_obj_t *yes = ui_button(row, 1, 88, UI_COLOR_RED, UI_COLOR_RED_PRESSED, _("Erase"), card_erase_cb, NULL);
		lv_obj_set_flex_grow(yes, 1);
		break;
	case CARD_ERASING:
		text(panel, _("Erasing..."), UI_COLOR_TEXT);
		break;
	case CARD_ERASED:
		text(panel, _("The card was erased."), UI_COLOR_TEXT);
		break;
	case CARD_FAILED:
		text(panel, _("The card could not be erased."), UI_COLOR_ORANGE);
		text(panel, in.reason, UI_COLOR_DIM);
		button(panel, _("Erase the card"), UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, card_ask_cb);
		break;
	}
	sd.shown = sd.changes;
}

static void card_refresh_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (sd.panel && sd.shown != sd.changes)
		card_build(sd.panel);
}

static void card_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	sd.panel = NULL;
}

void emmc_card_panel(lv_obj_t *page)
{
	sd.panel = settings_panel(page);
	lv_obj_add_event_cb(sd.panel, card_delete_cb, LV_EVENT_DELETE, NULL);
	if (sd.state != CARD_ERASING)
		sd.state = CARD_OFFERED;
	card_build(sd.panel);
	settings_timer(sd.panel, card_refresh_cb, POLL_MS);
}
