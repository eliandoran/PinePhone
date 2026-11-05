/*
 * GitHub's screen: the token's form until there is a token, then the
 * notifications. The token is a classic personal access token, kept in
 * TOKEN_FILE and sent with every request.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "github.h"

static char token[256]; /* "" while signed out */

/* The form, while the screen is open */
static struct {
	lv_obj_t *form, *ta, *error, *kb;
} form;

static void token_load(void)
{
	if (!ui_read_line(TOKEN_FILE, token, sizeof(token)))
		token[0] = '\0';
}

static bool token_save(const char *t)
{
	mkdir("/var/lib/app-data", 0755);
	mkdir(DIR, 0700);
	int fd = open(TOKEN_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("github: %s: %s\n", TOKEN_FILE, strerror(errno));
		return false;
	}
	FILE *f = fdopen(fd, "w");
	bool ok = fprintf(f, "%s\n", t) > 0;
	if (fclose(f) != 0 || !ok || rename(TOKEN_FILE ".new", TOKEN_FILE) != 0) {
		printf("github: %s: %s\n", TOKEN_FILE, strerror(errno));
		return false;
	}
	return true;
}

void api_request(struct ui_fetch *f, const char *method, const char *path, const char *body,
                 void (*done)(const char *data, long status))
{
	char url[512], auth[300];
	snprintf(url, sizeof(url), API "%s", path);
	snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
	const char *headers[] = {
		auth,
		"Accept: application/vnd.github+json",
		"X-GitHub-Api-Version: 2022-11-28",
		NULL,
	};
	ui_fetch_request(f, method, url, headers, body, done);
}

/* The form or the notifications */
static void show(const char *error)
{
	if (!form.form)
		return;
	bool signed_in = token[0];
	lv_obj_set_hidden(form.form, signed_in);
	if (signed_in)
		ui_keyboard_show(form.kb, false);
	lv_label_set_text_static(form.error, error ? error : "");
	lv_obj_set_hidden(form.error, !error);
	notifications_show(signed_in);
}

void github_sign_out(const char *error)
{
	token[0] = '\0';
	unlink(TOKEN_FILE);
	show(error);
}

static void sign_in(void)
{
	char t[sizeof(token)];
	snprintf(t, sizeof(t), "%s", lv_textarea_get_text(form.ta));
	/* Without the spaces around it */
	char *s = t + strspn(t, " ");
	s[strcspn(s, " ")] = '\0';
	if (!*s)
		return;
	if (!token_save(s)) {
		show("The token could not be saved.");
		return;
	}
	snprintf(token, sizeof(token), "%s", s);
	lv_textarea_set_text(form.ta, "");
	show(NULL);
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(form.kb, true);
		break;
	case LV_EVENT_READY:
		sign_in();
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(form.kb, false);
		break;
	default:
		break;
	}
}

static void button_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	sign_in();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&form, 0, sizeof(form));
}

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text_static(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void form_create(lv_obj_t *body)
{
	/* Shrinks above the keyboard */
	form.form = lv_obj_create(body);
	lv_obj_remove_style_all(form.form);
	lv_obj_set_width(form.form, LV_PCT(100));
	lv_obj_set_flex_grow(form.form, 1);
	lv_obj_set_flex_flow(form.form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(form.form, 32, 0);
	lv_obj_set_style_pad_ver(form.form, 48, 0);
	lv_obj_set_style_pad_row(form.form, 24, 0);
	lv_obj_set_scroll_dir(form.form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(form.form, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(form.form, delete_cb, LV_EVENT_DELETE, NULL);

	text_label(form.form, "Sign in to GitHub", &ui_font_32, UI_COLOR_TEXT);
	text_label(form.form,
		   "On github.com, in Settings \xc2\xbb Developer settings \xc2\xbb Personal access tokens "
		   "\xc2\xbb Tokens (classic), generate a token with the notifications and repo scopes, "
		   "then type it here. It can also be put in " TOKEN_FILE ".",
		   &ui_font_20, UI_COLOR_DIM);

	form.ta = ui_text_field(form.form, 88, sizeof(token) - 1, ta_cb, NULL);
	lv_obj_set_width(form.ta, LV_PCT(100));
	lv_textarea_set_placeholder_text(form.ta, "Token");

	form.error = text_label(form.form, "", &ui_font_20, UI_COLOR_RED);

	lv_obj_t *button = ui_button(form.form, LV_PCT(100), 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
				     "Sign in", button_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), &ui_font_26, 0);

	/* Right under the form: the notifications are hidden while it shows */
	form.kb = ui_keyboard_create(body, form.ta);
}

void github_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("GitHub", &header, &body);
	lv_obj_set_scrollable(body, false);

	token_load();
	form_create(body);
	notifications_create(body, header);
	show(NULL);
	ui_push(scr);
}
