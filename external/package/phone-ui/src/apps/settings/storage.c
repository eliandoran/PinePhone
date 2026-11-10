/* Settings, Storage: the root and data file systems, a section per disk (the eMMC, the microSD
 * card); from a card, installing it on the eMMC, and from the eMMC, erasing that card
 * (emmc_install.c) */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "settings.h"

#define SYS_BLOCK "/sys/block"
#define WATCH_MS 1000 /* the disks looked at while the page is open */

void storage_summary(lv_obj_t *value)
{
	char buf[32];
	struct statvfs vfs;
	if (statvfs("/data", &vfs) == 0) {
		ui_size_format(buf, sizeof(buf), (unsigned long long)vfs.f_bavail * vfs.f_frsize);
		ui_set_text_fmt(value, _("%s free"), buf);
	}
}

static void storage_card(lv_obj_t *page, const char *title, const char *path)
{
	char used[32], total[32], avail[32];
	struct statvfs vfs;
	struct stat st, root;

	lv_obj_t *card = settings_panel(page);
	lv_obj_t *value = settings_title_line(card, title);

	/* /data unmounted is just a directory of the root file system */
	bool mounted = stat(path, &st) == 0 && stat("/", &root) == 0 &&
	               (strcmp(path, "/") == 0 || st.st_dev != root.st_dev);
	if (!mounted || statvfs(path, &vfs) != 0 || !vfs.f_blocks) {
		ui_set_text(value, _("Not mounted"));
		return;
	}

	unsigned long long t = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
	unsigned long long u = (unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize;
	unsigned long long a = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
	ui_size_format(used, sizeof(used), u);
	ui_size_format(total, sizeof(total), t);
	ui_size_format(avail, sizeof(avail), a);
	ui_set_text_fmt(value, _("%s of %s"), used, total);

	lv_obj_t *bar = settings_bar(card, 1000, UI_COLOR_ACCENT);
	lv_bar_set_value(bar, (int32_t)(t ? u * 1000 / t : 0), LV_ANIM_OFF);

	lv_obj_t *free_label = lv_label_create(card);
	ui_set_text_fmt(free_label, vfs.f_flag & ST_RDONLY ? _("%s free, read-only") : _("%s free"), avail);
	lv_obj_set_style_text_font(free_label, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(free_label, lv_color_hex(UI_COLOR_DIM), 0);
}

static void install_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	emmc_install_open();
}

/* Running from the card: installing it on the eMMC */
static void install_panel(lv_obj_t *page)
{
	lv_obj_t *card = settings_panel(page);
	lv_obj_t *label = lv_label_create(card);
	lv_label_set_text_static(label, _("The system runs from the microSD card. Installed on the phone's storage, it starts faster, without the card."));
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_t *b = ui_button(card, LV_PCT(100), 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
	                        _("Install on the phone"), install_cb, NULL);
	lv_obj_set_style_margin_top(b, 8, 0);
}

/* The microSD card's bytes: the MMC disk without boot partitions (the eMMC has some); false
 * when no card is in the slot */
static bool card_bytes(unsigned long long *bytes)
{
	DIR *dir = opendir(SYS_BLOCK);
	struct dirent *ent;
	char path[300];
	long sectors = -1;
	while (dir && sectors < 0 && (ent = readdir(dir))) {
		const char *d = ent->d_name;
		if (strncmp(d, "mmcblk", 6) != 0 || strstr(d, "boot") || strstr(d, "rpmb"))
			continue;
		snprintf(path, sizeof(path), SYS_BLOCK "/%sboot0", d);
		if (access(path, F_OK) == 0)
			continue;
		snprintf(path, sizeof(path), SYS_BLOCK "/%s/size", d);
		if (!ui_read_long(path, &sectors))
			sectors = -1;
	}
	if (dir)
		closedir(dir);
	if (sectors < 0)
		return false;
	*bytes = (unsigned long long)sectors * 512;
	return true;
}

/* The card's section: its capacity, or that there is none; false without a card */
static bool card_section(lv_obj_t *page)
{
	unsigned long long bytes;
	char size[32];
	ui_section_label(page, _("microSD card"));
	if (!card_bytes(&bytes)) {
		lv_obj_t *panel = settings_panel(page);
		lv_obj_t *label = lv_label_create(panel);
		lv_label_set_text_static(label, _("No card is plugged in."));
		lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_DIM), 0);
		return false;
	}
	ui_size_format(size, sizeof(size), bytes);
	ui_set_text(settings_info_row(ui_card_create(page), _("Capacity")), size);
	return true;
}

/* The file systems the system runs on */
static void system_cards(lv_obj_t *page)
{
	storage_card(page, _("System"), "/");
	storage_card(page, _("Data"), "/data");
}

/* The MMC disks the kernel has (names and sizes): a card put in or taken out changes them */
static void disks_key(char *key, size_t size)
{
	DIR *dir = opendir(SYS_BLOCK);
	struct dirent *ent;
	char path[300], line[32];
	size_t n = 0;
	key[0] = '\0';
	while (dir && (ent = readdir(dir))) {
		if (strncmp(ent->d_name, "mmcblk", 6) != 0)
			continue;
		snprintf(path, sizeof(path), SYS_BLOCK "/%s/size", ent->d_name);
		if (!ui_read_line(path, line, sizeof(line)))
			line[0] = '\0';
		int w = snprintf(key + n, size - n, "%s=%s ", ent->d_name, line);
		if (w < 0 || (size_t)w >= size - n)
			break;
		n += (size_t)w;
	}
	if (dir)
		closedir(dir);
}

static struct {
	lv_obj_t *page;
	char disks[512];
} st;

/* A section per disk, the one the system runs from first */
static void build(lv_obj_t *page)
{
	lv_obj_clean(page);
	switch (settings_boot(NULL)) {
	case BOOT_CARD:
		card_section(page);
		system_cards(page);
		if (emmc_install_offered()) {
			ui_section_label(page, _("Phone storage (eMMC)"));
			install_panel(page);
		}
		break;
	case BOOT_EMMC:
		ui_section_label(page, _("Phone storage (eMMC)"));
		system_cards(page);
		if (card_section(page) && emmc_card_offered())
			emmc_card_panel(page);
		break;
	default: /* neither of ours */
		system_cards(page);
		break;
	}
}

/* Built again, the card asked about again, when the disks change */
static void watch_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	char disks[sizeof(st.disks)];
	disks_key(disks, sizeof(disks));
	if (strcmp(disks, st.disks) == 0)
		return;
	snprintf(st.disks, sizeof(st.disks), "%s", disks);
	emmc_card_forget();
	build(st.page);
}

void storage_open(void)
{
	lv_obj_t *scr = settings_page(_("Storage"), &st.page);
	st.disks[0] = '\1'; /* none read yet: built by the first check */
	st.disks[1] = '\0';
	settings_timer(scr, watch_cb, WATCH_MS);
	ui_push(scr);
}
