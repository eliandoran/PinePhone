/* Settings, Storage: the root and data file systems */

#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "settings.h"

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
	lv_obj_set_style_text_font(free_label, &ui_font_20, 0);
	lv_obj_set_style_text_color(free_label, lv_color_hex(UI_COLOR_DIM), 0);
}

void storage_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page(_("Storage"), &page);
	storage_card(page, _("System"), "/");
	storage_card(page, _("Data"), "/data");
	ui_push(scr);
}
