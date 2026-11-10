/*
 * The clipboard: one text in CLIPBOARD (RAM, gone at reboot), which the keyboard offers to
 * paste. A file rather than memory: the computer puts text there too (`just clipboard`, over
 * SSH), and an earlier phone-ui's copy outlives `just ui`. Forgotten FORGET_S after it was
 * written (its time), as Android does: what is copied is often a password or a key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "ui.h"

#ifndef CLIPBOARD
#define CLIPBOARD "/run/phone-ui.clipboard"
#endif

#define FORGET_S 3600
#define MAX_BYTES (1 << 20)
#define NOTICE_MS 1500

static lv_obj_t *notice;

static void notice_gone_cb(lv_timer_t *t)
{
	lv_obj_delete(lv_timer_get_user_data(t));
	notice = NULL;
}

/* "Copied" under the status bar, for a moment; taps go through it */
static void notice_show(void)
{
	if (notice)
		return;
	notice = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(notice);
	lv_obj_set_size(notice, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, UI_STATUS_H + 24);
	lv_obj_set_style_bg_color(notice, lv_color_hex(UI_COLOR_CARD), 0);
	lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(notice, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(notice, 32, 0);
	lv_obj_set_style_shadow_opa(notice, LV_OPA_40, 0);
	lv_obj_set_style_shadow_color(notice, lv_color_black(), 0);
	lv_obj_set_style_pad_hor(notice, 32, 0);
	lv_obj_set_style_pad_ver(notice, 16, 0);
	lv_obj_set_clickable(notice, false);
	lv_obj_t *label = lv_label_create(notice);
	lv_label_set_text_static(label, _("Copied"));
	lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_timer_t *t = lv_timer_create(notice_gone_cb, NOTICE_MS, notice);
	lv_timer_set_repeat_count(t, 1);
}

void ui_clipboard_set(const char *text)
{
	/* Written whole, then in place: a paste never reads half of it */
	FILE *f = fopen(CLIPBOARD ".new", "w");
	bool ok = f && fputs(text, f) != EOF;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok || rename(CLIPBOARD ".new", CLIPBOARD) != 0) {
		printf("phone-ui: clipboard: cannot write %s\n", CLIPBOARD);
		return;
	}
	chmod(CLIPBOARD, 0600);
	ui_vibrate(UI_VIBRATE_TAP);
	notice_show();
}

char *ui_clipboard_get(void)
{
	struct stat st;
	if (stat(CLIPBOARD, &st) != 0 || st.st_size == 0)
		return NULL;
	if (time(NULL) - st.st_mtime > FORGET_S) {
		unlink(CLIPBOARD);
		return NULL;
	}
	FILE *f = fopen(CLIPBOARD, "r");
	if (!f)
		return NULL;
	size_t size = st.st_size < MAX_BYTES ? (size_t)st.st_size : MAX_BYTES;
	char *text = malloc(size + 1);
	size_t n = text ? fread(text, 1, size, f) : 0;
	fclose(f);
	if (!n) {
		free(text);
		return NULL;
	}
	text[n] = '\0';
	return text;
}
