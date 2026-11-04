/*
 * Settings, Wi-Fi: the networks around, connecting to one (its password asked the
 * first time), forgetting it. Through wpa_supplicant's control interface (S15wifi
 * starts it on wlan0, its networks saved in /var/lib/wpa_supplicant): a datagram
 * socket bound in /tmp and connected to the interface's, as wpa_ctrl does, attached
 * for its events. The address (DHCP) is wpa_cli's action script's business.
 */

#include <ctype.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "settings.h"

#ifndef WPA_CTRL_DIR
#define WPA_CTRL_DIR "/run/wpa_supplicant"
#endif
#define WPA_IFACE "wlan0"
#define WPA_TIMEOUT_MS 1000
#define SCAN_PERIOD 10 /* ticks of a second */
#define REPLY_SIZE 4096 /* wpa_supplicant's own */
#define MAX_NETS 32
#define ROW_H 112

/* The control interface */

struct wpa {
	int fd; /* -1: closed */
	char local[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static void event(const char *msg);

static void wpa_close(struct wpa *w)
{
	if (w->fd >= 0) {
		close(w->fd);
		unlink(w->local);
	}
	w->fd = -1;
}

static bool wpa_open(struct wpa *w)
{
	static unsigned count;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	w->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (w->fd < 0)
		return false;
	snprintf(w->local, sizeof(w->local), "/tmp/phone-ui-wpa-%d-%u", (int)getpid(), count++);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", w->local);
	unlink(w->local);
	if (bind(w->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(w->fd);
		w->fd = -1;
		return false;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), WPA_CTRL_DIR "/" WPA_IFACE);
	if (connect(w->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		wpa_close(w);
		return false;
	}
	return true;
}

/* Sends a command and waits for its reply (cut to `size`); events arriving meanwhile
 * go to event(), whole. False (and closed) if wpa_supplicant does not answer. */
static bool wpa_request(struct wpa *w, const char *cmd, char *reply, size_t size)
{
	char msg[REPLY_SIZE + 1];
	if (w->fd < 0)
		return false;
	if (send(w->fd, cmd, strlen(cmd), 0) < 0) {
		wpa_close(w);
		return false;
	}
	for (;;) {
		struct pollfd p = { .fd = w->fd, .events = POLLIN };
		ssize_t n;
		if (poll(&p, 1, WPA_TIMEOUT_MS) <= 0 || (n = recv(w->fd, msg, sizeof(msg) - 1, 0)) < 0) {
			printf("phone-ui: wpa_supplicant does not answer %s\n", cmd);
			wpa_close(w);
			return false;
		}
		msg[n] = '\0';
		if (msg[0] != '<') {
			snprintf(reply, size, "%s", msg);
			return true;
		}
		event(msg);
	}
}

/* The events received since the last call */
static void wpa_events(struct wpa *w)
{
	char msg[512];
	ssize_t n;
	while (w->fd >= 0 && (n = recv(w->fd, msg, sizeof(msg) - 1, MSG_DONTWAIT)) > 0) {
		msg[n] = '\0';
		if (msg[0] == '<')
			event(msg);
	}
}

/* The value of `key=` in a reply of lines, "" if none */
static void reply_value(const char *reply, const char *key, char *out, size_t size)
{
	size_t len = strlen(key);
	out[0] = '\0';
	for (const char *line = reply; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL)
		if (strncmp(line, key, len) == 0 && line[len] == '=') {
			const char *v = line + len + 1;
			snprintf(out, size, "%.*s", (int)strcspn(v, "\n"), v);
			return;
		}
}

/* A text of wpa_supplicant's replies (printf_encode(): \\, \", \xNN...) as it is */
static void decode(char *out, size_t size, const char *s, size_t len)
{
	size_t n = 0;
	for (size_t i = 0; i < len && n + 1 < size; i++) {
		if (s[i] != '\\' || i + 1 >= len) {
			out[n++] = s[i];
			continue;
		}
		char c = s[++i];
		if (c == 'x' && i + 2 < len && isxdigit((unsigned char)s[i + 1]) &&
		    isxdigit((unsigned char)s[i + 2])) {
			char hex[3] = { s[i + 1], s[i + 2], 0 };
			out[n++] = (char)strtol(hex, NULL, 16);
			i += 2;
		} else {
			out[n++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'e' ? '\033' : c;
		}
	}
	out[n] = '\0';
}

/* The page */

struct net {
	char ssid[33];
	int signal;  /* dBm */
	bool secured, psk, sae, unsupported;
	int id;      /* saved: its id, -1 if not */
};

static struct {
	struct wpa w;
	lv_obj_t *status, *address_row, *address, *forget_row;
	lv_obj_t *networks_label, *list;
	unsigned ticks;
	int wrong_key; /* event: the network whose password was refused, -1 none */
	int trying;    /* the network just added, -1 none */
	char trying_ssid[33];
	int current;   /* the network wpa_supplicant uses, -1 none */
	char note[80]; /* what happened to the last attempt, shown instead of "Not connected" */
	char shown[2048]; /* what the list shows: rebuilt when it changes */
	struct net nets[MAX_NETS];
	unsigned count;
	bool open;
} st = { .w.fd = -1 };

static void event(const char *msg)
{
	const char *id;
	if (st.open && strstr(msg, "CTRL-EVENT-SSID-TEMP-DISABLED") && strstr(msg, "reason=WRONG_KEY") &&
	    (id = strstr(msg, " id=")))
		st.wrong_key = atoi(id + 4);
}

static bool command(const char *fmt, ...) LV_FORMAT_ATTRIBUTE(1, 2);
static bool command(const char *fmt, ...)
{
	char cmd[256], reply[64];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return wpa_request(&st.w, cmd, reply, sizeof(reply)) && strncmp(reply, "OK", 2) == 0;
}

static int by_signal(const void *a, const void *b)
{
	const struct net *x = a, *y = b;
	if ((x->id >= 0 && x->id == st.current) != (y->id >= 0 && y->id == st.current))
		return x->id >= 0 && x->id == st.current ? -1 : 1;
	return y->signal - x->signal;
}

/* The scan results, one entry per SSID (its strongest), hidden ones left out, with the
 * saved networks' ids */
static bool nets_read(void)
{
	static char reply[REPLY_SIZE + 1];
	char saved[MAX_NETS][33];
	int saved_id[MAX_NETS];
	unsigned saved_count = 0;

	if (!wpa_request(&st.w, "LIST_NETWORKS", reply, sizeof(reply)))
		return false;
	/* network id / ssid / bssid / flags */
	for (char *line = strchr(reply, '\n'); line && line[1] && saved_count < MAX_NETS; line = strchr(line, '\n')) {
		char *f = ++line, *end = f + strcspn(f, "\n"), *tab = strchr(f, '\t');
		if (!tab || tab > end)
			continue;
		size_t len = strcspn(tab + 1, "\t\n");
		saved_id[saved_count] = atoi(f);
		decode(saved[saved_count++], sizeof(saved[0]), tab + 1, len);
	}

	if (!wpa_request(&st.w, "SCAN_RESULTS", reply, sizeof(reply)))
		return false;
	st.count = 0;
	/* bssid / frequency / signal level / flags / ssid */
	for (char *line = strchr(reply, '\n'); line && line[1]; line = strchr(line, '\n')) {
		char *f[5];
		f[0] = ++line;
		size_t end = strcspn(line, "\n");
		unsigned k;
		for (k = 1; k < 5; k++) {
			char *tab = memchr(f[k - 1], '\t', (size_t)(line + end - f[k - 1]));
			if (!tab)
				break;
			f[k] = tab + 1;
		}
		if (k < 5)
			continue;
		struct net n = { .signal = atoi(f[2]), .id = -1 };
		decode(n.ssid, sizeof(n.ssid), f[4], (size_t)(line + end - f[4]));
		if (!n.ssid[0])
			continue;
		size_t flags_len = (size_t)(f[4] - f[3]);
		char flags[256];
		snprintf(flags, sizeof(flags), "%.*s", (int)flags_len, f[3]);
		n.psk = strstr(flags, "-PSK") != NULL;
		n.sae = strstr(flags, "SAE") != NULL;
		n.secured = n.psk || n.sae || strstr(flags, "WEP") || strstr(flags, "EAP");
		n.unsupported = n.secured && !n.psk && !n.sae;
		for (unsigned i = 0; i < saved_count; i++)
			if (strcmp(saved[i], n.ssid) == 0)
				n.id = saved_id[i];

		unsigned i;
		for (i = 0; i < st.count && strcmp(st.nets[i].ssid, n.ssid) != 0; i++)
			;
		if (i < st.count) {
			if (n.signal > st.nets[i].signal)
				st.nets[i] = n;
		} else if (st.count < MAX_NETS) {
			st.nets[st.count++] = n;
		}
	}
	if (st.count)
		qsort(st.nets, st.count, sizeof(st.nets[0]), by_signal);
	return true;
}

static const char *signal_text(int dbm)
{
	return dbm >= -55 ? "Excellent" : dbm >= -67 ? "Good" : dbm >= -75 ? "Fair" : "Weak";
}

static void password_open(const struct net *n);
static void refresh(void);

/* Adds the network and connects to it: with its password, or open */
static void connect_new(const struct net *n, const char *password)
{
	char reply[32], hex[sizeof(n->ssid) * 2 + 1];
	if (!wpa_request(&st.w, "ADD_NETWORK", reply, sizeof(reply)) || !isdigit((unsigned char)reply[0]))
		return;
	int id = atoi(reply);
	for (size_t i = 0; n->ssid[i]; i++)
		snprintf(hex + i * 2, 3, "%02x", (unsigned char)n->ssid[i]);
	bool ok = command("SET_NETWORK %d ssid %s", id, hex);
	if (!password)
		ok = ok && command("SET_NETWORK %d key_mgmt NONE", id);
	else
		ok = ok && command("SET_NETWORK %d psk \"%s\"", id, password);
	/* WPA3: SAE with protected management frames, optional in transition mode */
	if (password && n->sae)
		ok = ok && command("SET_NETWORK %d key_mgmt %s", id, n->psk ? "WPA-PSK SAE" : "SAE") &&
		     command("SET_NETWORK %d ieee80211w %d", id, n->psk ? 1 : 2);
	st.note[0] = '\0';
	st.trying = id;
	snprintf(st.trying_ssid, sizeof(st.trying_ssid), "%s", n->ssid);
	if (!ok || !command("SELECT_NETWORK %d", id) || !command("SAVE_CONFIG")) {
		command("REMOVE_NETWORK %d", id);
		st.trying = -1;
		snprintf(st.note, sizeof(st.note), "Could not connect to %s", n->ssid);
	}
}

static void net_cb(lv_event_t *e)
{
	const struct net *n = lv_event_get_user_data(e);
	if (n->id >= 0) {
		st.note[0] = '\0';
		if (n->id != st.current)
			command("SELECT_NETWORK %d", n->id);
	} else if (!n->secured) {
		connect_new(n, NULL);
	} else {
		password_open(n);
		return;
	}
	refresh();
}

static void list_update(void)
{
	char shown[sizeof(st.shown)];
	size_t len = 0;
	for (unsigned i = 0; i < st.count; i++) {
		const struct net *n = &st.nets[i];
		len += (size_t)snprintf(shown + len, sizeof(shown) - len, "%s|%s|%d|%d|%d\n", n->ssid,
		                        signal_text(n->signal), n->secured, n->id, n->id >= 0 && n->id == st.current);
		if (len >= sizeof(shown))
			break;
	}
	if (!len)
		shown[0] = '\0';
	if (strcmp(shown, st.shown) == 0 && lv_obj_get_child_count(st.list))
		return;
	snprintf(st.shown, sizeof(st.shown), "%s", shown);

	lv_obj_clean(st.list);
	if (!st.count) {
		lv_obj_t *row = ui_row_create(st.list, ROW_H, false, NULL, NULL);
		ui_dim_label(row, "Searching...", &ui_font_24);
		return;
	}
	for (unsigned i = 0; i < st.count; i++) {
		const struct net *n = &st.nets[i];
		lv_obj_t *row = ui_row_create(st.list, ROW_H, i + 1 < st.count, n->unsupported ? NULL : net_cb,
		                              (void *)n);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_set_width(ui_one_line_label(text, ui_intern(n->ssid), &ui_font_26), LV_PCT(100));
		char info[64];
		snprintf(info, sizeof(info), "%s \xc2\xb7 %s",
		         n->unsupported ? "Not supported" : n->secured ? "Secured" : "Open", signal_text(n->signal));
		lv_obj_set_width(ui_dim_label(text, ui_intern(info), &ui_font_20), LV_PCT(100));
		if (n->id >= 0 && n->id == st.current) {
			lv_obj_t *check = lv_label_create(row);
			lv_label_set_text_static(check, LV_SYMBOL_OK);
			lv_obj_set_style_text_color(check, lv_color_hex(UI_COLOR_ACCENT), 0);
		} else if (n->id >= 0) {
			ui_dim_label(row, "Saved", &ui_font_20);
		}
	}
}

static void unavailable(void)
{
	ui_set_text(st.status, "Not available");
	lv_obj_set_hidden(st.address_row, true);
	lv_obj_set_hidden(st.forget_row, true);
	lv_obj_set_hidden(st.networks_label, true);
	lv_obj_set_hidden(st.list, true);
	st.current = -1;
}

static void tick_cb(lv_timer_t *t)
{
	LV_UNUSED(t);
	static char reply[REPLY_SIZE + 1];
	char state[32], ssid_raw[140], ssid[33], ip[48], id[16];

	if (st.w.fd < 0) {
		if (!wpa_open(&st.w) || !command("ATTACH")) {
			wpa_close(&st.w);
			unavailable();
			return;
		}
		st.ticks = 0;
	}
	wpa_events(&st.w);

	if (st.wrong_key >= 0) {
		/* Refused: forgotten when just added, so that the password is asked again */
		for (unsigned i = 0; i < st.count; i++)
			if (st.nets[i].id == st.wrong_key)
				snprintf(st.note, sizeof(st.note), "Wrong password for %s", st.nets[i].ssid);
		if (st.wrong_key == st.trying) {
			snprintf(st.note, sizeof(st.note), "Wrong password for %s", st.trying_ssid);
			command("REMOVE_NETWORK %d", st.trying);
			command("SAVE_CONFIG");
			st.trying = -1;
		}
		st.wrong_key = -1;
	}
	if (st.ticks++ % SCAN_PERIOD == 0)
		command("SCAN"); /* FAIL-BUSY while one runs: fine */

	if (!wpa_request(&st.w, "STATUS", reply, sizeof(reply))) {
		unavailable();
		return;
	}
	reply_value(reply, "wpa_state", state, sizeof(state));
	reply_value(reply, "ssid", ssid_raw, sizeof(ssid_raw));
	decode(ssid, sizeof(ssid), ssid_raw, strlen(ssid_raw));
	reply_value(reply, "ip_address", ip, sizeof(ip));
	reply_value(reply, "id", id, sizeof(id));
	st.current = id[0] ? atoi(id) : -1;

	bool completed = strcmp(state, "COMPLETED") == 0;
	if (completed) {
		st.note[0] = '\0';
		if (st.current == st.trying)
			st.trying = -1;
		ui_set_text_fmt(st.status, ip[0] ? "Connected to %s" : "Getting an address from %s", ssid);
	} else if (strcmp(state, "INTERFACE_DISABLED") == 0) {
		ui_set_text(st.status, "Off");
	} else if (st.note[0]) {
		ui_set_text(st.status, st.note);
	} else if (ssid[0] && (strcmp(state, "AUTHENTICATING") == 0 || strcmp(state, "ASSOCIATING") == 0 ||
	                       strcmp(state, "ASSOCIATED") == 0 || strstr(state, "HANDSHAKE"))) {
		ui_set_text_fmt(st.status, "Connecting to %s...", ssid);
	} else {
		ui_set_text(st.status, "Not connected");
	}
	if (ip[0] && completed)
		ui_set_text(st.address, ip);
	lv_obj_set_hidden(st.address_row, !(ip[0] && completed));
	lv_obj_set_hidden(st.forget_row, st.current < 0);
	lv_obj_set_hidden(st.networks_label, false);
	lv_obj_set_hidden(st.list, false);

	if (nets_read())
		list_update();
}

static lv_timer_t *timer;

/* What a tap changed, shown now rather than at the next tick */
static void refresh(void)
{
	if (timer)
		lv_timer_ready(timer);
}

static void forget_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(15);
	if (st.current >= 0 && command("REMOVE_NETWORK %d", st.current))
		command("SAVE_CONFIG");
	st.note[0] = '\0';
	refresh();
}

static void page_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(timer);
	timer = NULL;
	wpa_close(&st.w);
	memset(&st, 0, sizeof(st));
	st.w.fd = -1;
}

void wifi_open(void)
{
	lv_obj_t *page;
	lv_obj_t *scr = settings_page("Wi-Fi", &page);
	lv_obj_add_event_cb(scr, page_delete_cb, LV_EVENT_DELETE, NULL);
	st.open = true;
	st.wrong_key = st.trying = st.current = -1;

	lv_obj_t *card = ui_card_create(page);
	st.status = settings_info_row(card, "Wi-Fi");
	st.address = settings_info_row(card, "IP address");
	st.address_row = lv_obj_get_parent(st.address);
	st.forget_row = settings_row(card);
	settings_row_tappable(st.forget_row, forget_cb, NULL);
	lv_obj_t *forget = lv_label_create(st.forget_row);
	lv_label_set_text_static(forget, "Forget this network");
	lv_obj_set_style_text_color(forget, lv_color_hex(UI_COLOR_RED), 0);

	st.networks_label = ui_section_label(page, "Networks");
	st.list = ui_card_create(page);

	timer = lv_timer_create(tick_cb, 1000, NULL);
	tick_cb(timer);
	ui_push(scr);
}

/* The password of a network not saved yet */

struct password {
	struct net net;
	lv_obj_t *ta, *kb, *error;
};

static void password_submit(struct password *p)
{
	if (strlen(lv_textarea_get_text(p->ta)) < 8) {
		lv_obj_set_hidden(p->error, false);
		return;
	}
	connect_new(&p->net, lv_textarea_get_text(p->ta));
	refresh();
	ui_pop();
}

static void password_ta_cb(lv_event_t *e)
{
	struct password *p = lv_event_get_user_data(e);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(p->kb, true);
		break;
	case LV_EVENT_READY:
		password_submit(p);
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(p->kb, false);
		break;
	default:
		break;
	}
}

static void password_ok_cb(lv_event_t *e)
{
	password_submit(lv_event_get_user_data(e));
}

static void password_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void password_open(const struct net *n)
{
	struct password *p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->net = *n;

	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(ui_intern(n->ssid), &header, &body);
	lv_obj_add_event_cb(scr, password_delete_cb, LV_EVENT_DELETE, p);
	ui_header_button(header, LV_SYMBOL_OK, password_ok_cb, p);
	lv_obj_set_scrollable(body, false);

	lv_obj_t *content = ui_box(body);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(content, 16, 0);
	lv_obj_set_style_pad_row(content, 12, 0);

	ui_section_label(content, "Password");
	p->ta = ui_text_field(content, 88, 63, password_ta_cb, p);
	lv_obj_set_width(p->ta, LV_PCT(100));
	lv_textarea_set_placeholder_text(p->ta, "Password");
	lv_textarea_set_password_mode(p->ta, true);
	p->error = ui_dim_label(content, "At least 8 characters", &ui_font_20);
	lv_obj_set_style_text_color(p->error, lv_color_hex(UI_COLOR_RED), 0);
	lv_obj_set_style_pad_hor(p->error, 28, 0);
	lv_obj_set_hidden(p->error, true);

	p->kb = ui_keyboard_create(body, p->ta);
	ui_keyboard_show(p->kb, true);
	ui_push(scr);
}

/* For the list of Settings and the Network page */

void wifi_summary(lv_obj_t *value)
{
	struct wpa w = { .fd = -1 };
	char reply[REPLY_SIZE + 1], state[32], ssid_raw[140], ssid[33];
	if (!wpa_open(&w) || !wpa_request(&w, "STATUS", reply, sizeof(reply))) {
		wpa_close(&w);
		ui_set_text(value, "Not available");
		return;
	}
	wpa_close(&w);
	reply_value(reply, "wpa_state", state, sizeof(state));
	reply_value(reply, "ssid", ssid_raw, sizeof(ssid_raw));
	decode(ssid, sizeof(ssid), ssid_raw, strlen(ssid_raw));
	if (strcmp(state, "COMPLETED") == 0)
		ui_set_text(value, ssid);
	else
		ui_set_text(value, strcmp(state, "INTERFACE_DISABLED") == 0 ? "Off" : "Not connected");
}
