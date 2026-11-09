/*
 * Setting up sync: the address of a Trilium server or sync relay and the password of
 * the notes, then the document's credentials (documentId, documentSecret) asked of
 * it as Trilium's setup.ts asks them (GET /api/setup/sync-seed, the password in
 * trilium-cred, a second factor's code in trilium-totp when the server asks for one),
 * kept with the address in the database. The password is not kept: the document's
 * secret is what sync logs in with. The form shrinks above the keyboard; Enter moves
 * on to what is missing, then connects.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "trilium.h"

#define FIELD_H 88
#define ADDRESS_MAX 512
#define PASSWORD_MAX 256
#define CODE_MAX 16

static struct {
	lv_obj_t *form, *hint, *address, *password, *code, *error, *button, *kb;
	bool busy;
	char host[ADDRESS_MAX + 16]; /* being tried */
	struct ui_fetch fetch;
} setup;

static void type_into(lv_obj_t *ta)
{
	lv_obj_t *fields[] = {setup.address, setup.password, setup.code};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		if (fields[i] != ta)
			lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
	lv_keyboard_set_textarea(setup.kb, ta);
	ui_keyboard_show(setup.kb, true);
}

static void error(const char *text)
{
	lv_label_set_text(setup.error, text);
	lv_obj_set_hidden(setup.error, !text[0]);
}

static void busy(bool on)
{
	setup.busy = on;
	lv_obj_set_state(setup.button, LV_STATE_DISABLED, on);
	lv_label_set_text_static(lv_obj_get_child(setup.button, 0), on ? _("Wait...") : _("Connect"));
}

static void fail(const char *text, lv_obj_t *ta)
{
	ui_vibrate(UI_VIBRATE_TAP);
	error(text);
	if (ta)
		type_into(ta);
}

/* The address as typed, without spaces or slashes at its end, https:// unless it has
 * a scheme: "relay.example/t/doc/" is "https://relay.example/t/doc"; false if it has no
 * host */
static bool host_tidy(const char *typed, char *out, size_t size)
{
	char text[ADDRESS_MAX + 1];
	size_t n = 0;
	for (; *typed && n < sizeof(text) - 1; typed++)
		if (!isspace((unsigned char)*typed))
			text[n++] = *typed;
	while (n && text[n - 1] == '/')
		n--;
	text[n] = '\0';
	const char *scheme = strstr(text, "://");
	snprintf(out, size, "%s%s", scheme ? "" : "https://", text); /* NON-NLS */
	return (scheme ? scheme + 3 : text)[0] != '\0';
}

static void base64(const char *in, char *out, size_t size)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t len = strlen(in), o = 0;
	for (size_t i = 0; i < len && o + 4 < size; i += 3) {
		unsigned v = (unsigned char)in[i] << 16;
		if (i + 1 < len)
			v |= (unsigned char)in[i + 1] << 8;
		if (i + 2 < len)
			v |= (unsigned char)in[i + 2];
		out[o++] = digits[v >> 18 & 63];
		out[o++] = digits[v >> 12 & 63];
		out[o++] = i + 1 < len ? digits[v >> 6 & 63] : '=';
		out[o++] = i + 2 < len ? digits[v & 63] : '=';
	}
	out[o] = '\0';
}

/* The server wants a second factor: its field, empty, typed into */
static void code_ask(const char *text)
{
	lv_obj_set_hidden(setup.code, false);
	lv_textarea_set_text(setup.code, "");
	fail(text, setup.code);
}

/* The seed: the document's options, kept with the address; then the document */
static bool seed_take(const char *data)
{
	struct json *root = json_parse(data);
	const struct json *version = json_get(root, "syncVersion");
	const struct json *options = json_get(root, "options");
	bool ok = false;
	if (!version || !options || options->kind != JSON_ARRAY) {
		error(_("No Trilium server at this address."));
	} else if (json_int(version, 0) != TR_SYNC_VERSION) {
		char text[200];
		snprintf(text, sizeof(text), _("The server runs another version of Trilium (sync version %lld, this app needs %d)."),
		         json_int(version, 0), TR_SYNC_VERSION);
		error(text);
	} else {
		sqlite3 *db = tr_db_open();
		ok = db && tr_db_set_up(db, setup.host, options);
		if (db)
			sqlite3_close(db);
		if (!ok)
			error(_("The server's answer could not be kept."));
	}
	json_free(root);
	return ok;
}

static void seed_done(const char *data, long status)
{
	busy(false);
	if (status == 200 && seed_take(data ? data : "")) {
		tr_show(); /* this form goes */
		return;
	}
	if (status == 200)
		ui_vibrate(UI_VIBRATE_TAP);
	else if (!status)
		fail(_("Cannot reach the server."), NULL);
	else if (status == 401 && data && strstr(data, "TOTP token is required"))
		code_ask(_("Enter the code from your authenticator app."));
	else if (status == 401 && data && strstr(data, "Incorrect TOTP token"))
		code_ask(_("Wrong two-factor code."));
	else if (status == 401)
		fail(_("Wrong password."), setup.password);
	else if (status == 400) /* a relay or server with no document yet */
		fail(_("There are no notes on this server yet: sync to it from Trilium on a computer first."), NULL);
	else if (status == 429)
		fail(_("Too many attempts. Try again in a few minutes."), NULL);
	else if (status >= 500) {
		char text[100];
		snprintf(text, sizeof(text), _("The server answered with an error (%ld)."), status);
		fail(text, NULL);
	} else
		fail(_("No Trilium server at this address."), setup.address);
}

static void submit(void)
{
	if (setup.busy)
		return;
	if (!host_tidy(lv_textarea_get_text(setup.address), setup.host, sizeof(setup.host))) {
		fail(_("Enter the server's address."), setup.address);
		return;
	}
	ui_vibrate(UI_VIBRATE_TAP);
	ui_keyboard_show(setup.kb, false);
	busy(true);
	error("");

	char cred[PASSWORD_MAX + 8], cred64[sizeof(cred) * 4 / 3 + 8];
	snprintf(cred, sizeof(cred), "dummy:%s", lv_textarea_get_text(setup.password)); /* NON-NLS: the user's name, ignored */
	base64(cred, cred64, sizeof(cred64));
	char cred_header[sizeof(cred64) + 16], code_header[CODE_MAX + 16];
	snprintf(cred_header, sizeof(cred_header), "trilium-cred: %s", cred64); /* NON-NLS */
	const char *headers[3] = {cred_header, NULL, NULL};
	const char *code = lv_textarea_get_text(setup.code);
	if (!lv_obj_is_hidden(setup.code) && code[0]) {
		snprintf(code_header, sizeof(code_header), "trilium-totp: %s", code); /* NON-NLS */
		headers[1] = code_header;
	}
	char url[sizeof(setup.host) + 32];
	snprintf(url, sizeof(url), "%s/api/setup/sync-seed", setup.host);
	ui_fetch_request(&setup.fetch, "GET", url, headers, NULL, seed_done);
}

static void button_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	submit();
}

static void field_cb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target_obj(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		type_into(ta);
		break;
	case LV_EVENT_READY: /* Enter: on to the password, or connect */
		if (ta == setup.address && !lv_textarea_get_text(setup.password)[0])
			type_into(setup.password);
		else
			submit();
		break;
	case LV_EVENT_CANCEL: /* the keyboard's hide key */
		ui_keyboard_show(setup.kb, false);
		break;
	default:
		break;
	}
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_fetch_stop(&setup.fetch);
	memset(&setup, 0, sizeof(setup));
}

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static lv_obj_t *field_create(const char *placeholder, uint32_t max_length, bool password)
{
	lv_obj_t *ta = ui_text_field(setup.form, FIELD_H, max_length, field_cb, NULL);
	lv_obj_set_width(ta, LV_PCT(100));
	lv_textarea_set_placeholder_text(ta, placeholder);
	lv_textarea_set_password_mode(ta, password);
	return ta;
}

void tr_setup_create(lv_obj_t *body)
{
	/* Shrinks above the keyboard */
	setup.form = lv_obj_create(body);
	lv_obj_remove_style_all(setup.form);
	lv_obj_set_width(setup.form, LV_PCT(100));
	lv_obj_set_flex_grow(setup.form, 1);
	lv_obj_set_flex_flow(setup.form, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(setup.form, 32, 0);
	lv_obj_set_style_pad_ver(setup.form, 48, 0);
	lv_obj_set_style_pad_row(setup.form, 24, 0);
	lv_obj_set_scroll_dir(setup.form, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(setup.form, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(setup.form, delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = text_label(setup.form, UI_FONT_HEADLINE, UI_COLOR_TEXT);
	lv_label_set_text_static(title, _("Sync with Trilium"));
	setup.hint = text_label(setup.form, UI_FONT_BODY, UI_COLOR_DIM);
	lv_label_set_text_static(setup.hint, _("Enter the address of your Trilium server or sync relay, as in "
	                                       "Trilium's sync options, and the password of your notes."));

	setup.address = field_create(_("Server address"), ADDRESS_MAX, false);
	setup.password = field_create(_("Password"), PASSWORD_MAX, true);
	setup.code = field_create(_("Two-factor code"), CODE_MAX, false);
	lv_obj_set_hidden(setup.code, true);

	setup.error = text_label(setup.form, UI_FONT_BODY, UI_COLOR_RED);
	lv_obj_set_hidden(setup.error, true);

	setup.button = ui_button(setup.form, LV_PCT(100), 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Connect"),
	                         button_cb, NULL);

	setup.kb = ui_keyboard_create(body, setup.address);
}
