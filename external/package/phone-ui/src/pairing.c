/*
 * Computers asking for SSH access, as Android asks before USB debugging: a computer on
 * the USB network connects to PAIRING_PORT and sends its public key, one line as in
 * authorized_keys ("ssh-ed25519 AAAA... user@host"). The phone asks whether to allow
 * it, showing its comment and its fingerprint as `ssh-keygen -l` prints it. Allowed,
 * the key is added to KEYS (/root/.ssh links there, the root file system being
 * read-only), which dropbear reads at each login.
 *
 * The answer is one line: "allowed" (also for a key already there, without asking),
 * "denied" (also unanswered for ASK_MS), "busy" (another computer is being asked),
 * "invalid" (not one key of a type dropbear takes, or options before it).
 *
 * Only on PAIRING_DEVICE: the USB cable, not a Wi-Fi network. The socket listens
 * once it exists (tried again every RETRY_MS) and is polled by a timer. The
 * question goes on the top layer, over the lock screen as the power menu does.
 *
 * Settings › Developer mode lists the computers allowed (pairing_computers()) and
 * revokes them (pairing_revoke(): the next login is refused, a session already open
 * goes on). No key is trusted by default, not even that of the computer that built the
 * image: each computer is asked for on its first connection.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "pairing.h"
#include "ui.h"

#ifndef PAIRING_DEVICE
#define PAIRING_DEVICE "usb0"
#endif
#define PAIRING_PORT 7022
#define SSH_DIR "/var/lib/ssh"
#define KEYS SSH_DIR "/authorized_keys"

#define POLL_MS 100
#define RETRY_MS 5000
#define LINE_MS 5000   /* for a computer to send its key */
#define ASK_MS 60000   /* unanswered: denied */
#define CLIENTS 4      /* connections waiting for their line */
#define KEY_LINE 4096  /* an RSA key of 16384 bits fits */

static const char *const types[] = {
	"ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
	"sk-ssh-ed25519@openssh.com", "sk-ecdsa-sha2-nistp256@openssh.com",
};

struct client {
	int fd; /* -1: free */
	uint32_t since;
	size_t len;
	char line[KEY_LINE];
};

static struct {
	int fd; /* listening, -1: not yet */
	uint32_t tried;
	struct client clients[CLIENTS];
	/* The computer being asked: its connection, its key's line to add */
	int asking;
	uint32_t asked;
	char *key;
	lv_obj_t *scrim;
} pr = { .fd = -1, .asking = -1 };

/* SHA-256 (FIPS 180-4), for fingerprints */

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	size_t i = 0;
	for (; len - i >= 64; i += 64)
		sha256_block(h, data + i);
	uint8_t last[128] = { 0 };
	size_t rest = len - i;
	memcpy(last, data + i, rest);
	last[rest] = 0x80;
	size_t n = rest < 56 ? 64 : 128;
	uint64_t bits = (uint64_t)len * 8;
	for (int b = 0; b < 8; b++)
		last[n - 1 - b] = bits >> (8 * b);
	sha256_block(h, last);
	if (n == 128)
		sha256_block(h, last + 64);
	for (int b = 0; b < 32; b++)
		out[b] = h[b / 4] >> (24 - 8 * (b % 4));
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 with its padding; the size decoded, -1 if it is not that */
static long b64_decode(const char *s, size_t len, uint8_t *out)
{
	if (len == 0 || len % 4)
		return -1;
	long n = 0;
	uint32_t acc = 0;
	for (size_t i = 0; i < len; i++) {
		const char *c = s[i] ? strchr(B64, s[i]) : NULL;
		int pad = s[i] == '=' ? 1 : 0;
		if (pad && (i < len - 2 || (i == len - 2 && s[len - 1] != '=')))
			return -1;
		if (!c && !pad)
			return -1;
		acc = acc << 6 | (pad ? 0 : (uint32_t)(c - B64));
		if (i % 4 == 3) {
			int pads = (s[i] == '=') + (s[i - 1] == '=');
			out[n++] = acc >> 16;
			if (pads < 2)
				out[n++] = acc >> 8;
			if (pads < 1)
				out[n++] = acc;
		}
	}
	return n;
}

/* "SHA256:" and the hash in base64 without its padding, as ssh-keygen -l */
static void fingerprint(const uint8_t *blob, size_t len, char *out)
{
	uint8_t h[33];
	sha256(blob, len, h);
	h[32] = 0;
	char *o = out + sprintf(out, "SHA256:");
	for (int i = 0; i < 32; i += 3) {
		uint32_t v = h[i] << 16 | h[i + 1] << 8 | (i + 2 < 32 ? h[i + 2] : 0);
		*o++ = B64[v >> 18 & 63];
		*o++ = B64[v >> 12 & 63];
		*o++ = B64[v >> 6 & 63];
		if (i + 2 < 32)
			*o++ = B64[v & 63];
	}
	*o = '\0';
}

/* A key's line: type, base64 of a blob of that type, maybe a comment. Splits it in
 * place; false if it is not one */
struct key {
	char *type, *b64, *comment; /* comment "" if none */
	char fp[64];
};

static bool key_parse(char *line, struct key *k)
{
	for (const char *c = line; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7f)
			return false;
	k->type = line;
	char *sp = strchr(line, ' ');
	if (!sp)
		return false;
	*sp = '\0';
	k->b64 = sp + 1;
	sp = strchr(k->b64, ' ');
	k->comment = "";
	if (sp) {
		*sp = '\0';
		k->comment = sp + 1;
	}

	bool known = false;
	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		known |= strcmp(k->type, types[i]) == 0;
	if (!known)
		return false;

	size_t len = strlen(k->b64);
	uint8_t *blob = malloc(len / 4 * 3 + 3);
	long n = b64_decode(k->b64, len, blob);
	size_t tl = strlen(k->type);
	/* The type again inside, then at least a key's worth */
	bool ok = n >= (long)(4 + tl + 32) && blob[0] == 0 && blob[1] == 0 && blob[2] == 0 && blob[3] == tl &&
	          memcmp(blob + 4, k->type, tl) == 0;
	if (ok)
		fingerprint(blob, n, k->fp);
	free(blob);
	return ok;
}

/* Whether a line of KEYS has that key (base64), whatever its comment and options */
static bool line_has(const char *line, const char *b64)
{
	size_t len = strlen(b64);
	for (const char *p = strstr(line, b64); p; p = strstr(p + 1, b64))
		if ((p == line || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\n' || !p[len]))
			return true;
	return false;
}

static bool key_known(const char *b64)
{
	FILE *f = fopen(KEYS, "r");
	if (!f)
		return false;
	char line[KEY_LINE + 256];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f))
		found = line_has(line, b64);
	fclose(f);
	return found;
}

/* The key in a line of KEYS, after its options if any, split in `copy` */
static bool line_key(const char *line, char *copy, size_t size, struct key *k)
{
	for (const char *p = line; p; p = strchr(p, ' ')) {
		p += *p == ' ';
		snprintf(copy, size, "%s", p);
		copy[strcspn(copy, "\r\n")] = '\0';
		if (key_parse(copy, k))
			return true;
	}
	return false;
}

struct pairing_computer *pairing_computers(int *count)
{
	char line[KEY_LINE + 256], copy[KEY_LINE + 256];
	struct key k;
	struct pairing_computer *c = NULL;
	*count = 0;
	FILE *f = fopen(KEYS, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || !line_key(line, copy, sizeof(copy), &k))
			continue;
		c = realloc(c, (*count + 1) * sizeof(*c));
		struct pairing_computer *p = &c[(*count)++];
		snprintf(p->name, sizeof(p->name), "%s", k.comment);
		snprintf(p->fp, sizeof(p->fp), "%s", k.fp);
		snprintf(p->b64, sizeof(p->b64), "%s", k.b64);
	}
	if (f)
		fclose(f);
	return c;
}

/* KEYS without the lines of that key, written beside it and renamed over it */
bool pairing_revoke(const char *b64)
{
	FILE *in = fopen(KEYS, "r");
	if (!in)
		return false;
	FILE *out = fopen(KEYS ".new", "w");
	if (!out) {
		fclose(in);
		return false;
	}
	fchmod(fileno(out), 0600);
	char line[KEY_LINE + 256];
	while (fgets(line, sizeof(line), in))
		if (!line_has(line, b64))
			fputs(line, out);
	fclose(in);
	bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
	ok = fclose(out) == 0 && ok && rename(KEYS ".new", KEYS) == 0;
	printf("phone-ui: pairing: %s\n", ok ? "revoked a computer" : "cannot write " KEYS);
	if (!ok)
		unlink(KEYS ".new");
	return ok;
}

/* At the end of KEYS, on a line of its own, synced */
static bool key_add(const char *line)
{
	mkdir(SSH_DIR, 0700);
	int fd = open(KEYS, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		printf("phone-ui: pairing: cannot open " KEYS ": %s\n", strerror(errno));
		return false;
	}
	char last = '\n';
	off_t end = lseek(fd, 0, SEEK_END);
	if (end > 0 && pread(fd, &last, 1, end - 1) != 1)
		last = '\n';
	char text[KEY_LINE + 2];
	int n = snprintf(text, sizeof(text), "%s%s\n", last == '\n' ? "" : "\n", line);
	bool ok = write(fd, text, n) == n && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	if (!ok)
		printf("phone-ui: pairing: cannot write " KEYS ": %s\n", strerror(errno));
	return ok;
}

static void client_end(struct client *c, const char *answer)
{
	if (answer && write(c->fd, answer, strlen(answer)) < 0)
		printf("phone-ui: pairing: cannot answer: %s\n", strerror(errno));
	close(c->fd);
	c->fd = -1;
}

/* The question goes, the computer gets its answer */
static void answered(const char *answer)
{
	if (pr.scrim)
		lv_obj_delete_async(pr.scrim);
	pr.scrim = NULL;
	struct client *c = &pr.clients[pr.asking];
	printf("phone-ui: pairing: %s\n", answer ? answer : "computer gone");
	client_end(c, answer ? (strcmp(answer, "allowed") == 0 ? "allowed\n" : "denied\n") : NULL);
	pr.asking = -1;
	free(pr.key);
	pr.key = NULL;
}

static void allow_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pr.asking >= 0)
		answered(key_add(pr.key) ? "allowed" : "denied");
}

static void deny_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (pr.asking >= 0)
		answered("denied");
}

static void ask(const struct key *k)
{
	pr.scrim = ui_box(lv_layer_top());
	lv_obj_set_size(pr.scrim, LV_PCT(100), LV_PCT(100));
	/* Square: one colored quad (see power.c) */
	lv_obj_set_style_bg_color(pr.scrim, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(pr.scrim, LV_OPA_50, 0);
	lv_obj_set_clickable(pr.scrim, true); /* not what is under it */

	lv_obj_t *card = ui_card_create(pr.scrim);
	lv_obj_set_width(card, 640);
	lv_obj_center(card);
	lv_obj_set_style_pad_all(card, 32, 0);
	lv_obj_set_style_pad_row(card, 16, 0);
	lv_obj_set_style_text_font(card, UI_FONT_BODY, 0);

	lv_obj_t *title = lv_label_create(card);
	lv_label_set_text_static(title, _("Allow this computer?"));
	lv_obj_set_style_text_font(title, UI_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);

	lv_obj_t *text = lv_label_create(card);
	lv_label_set_text_static(text, _("It gets full control of the phone over USB: its files, apps and settings."));
	lv_obj_set_width(text, LV_PCT(100));
	lv_obj_set_style_text_color(text, lv_color_hex(UI_COLOR_DIM), 0);

	if (k->comment[0]) {
		lv_obj_t *who = lv_label_create(card);
		ui_set_text(who, k->comment);
		lv_obj_set_width(who, LV_PCT(100));
		lv_obj_set_style_text_font(who, UI_FONT_TITLE, 0);
		lv_obj_set_style_text_color(who, lv_color_hex(UI_COLOR_TEXT), 0);
	}
	lv_obj_t *fp = lv_label_create(card);
	ui_set_text(fp, k->fp);
	lv_obj_set_width(fp, LV_PCT(100));
	lv_obj_set_style_text_font(fp, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(fp, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *buttons = ui_box(card);
	lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(buttons, 16, 0);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_text_font(buttons, UI_FONT_TITLE, 0);
	ui_button(buttons, 268, 88, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, _("Deny"), deny_cb, NULL);
	ui_button(buttons, 268, 88, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED, _("Allow"), allow_cb, NULL);
	ui_vibrate(UI_VIBRATE_TAP);
}

/* A computer's whole line arrived */
static void request(struct client *c)
{
	char *nl = memchr(c->line, '\n', c->len);
	*nl = '\0';
	if (nl > c->line && nl[-1] == '\r')
		nl[-1] = '\0';
	struct key k;
	char *line = strdup(c->line);
	bool valid = nl == c->line + c->len - 1 && key_parse(c->line, &k);
	if (!valid) {
		printf("phone-ui: pairing: not a key\n");
		client_end(c, "invalid\n");
	} else if (key_known(k.b64)) {
		printf("phone-ui: pairing: %s already allowed\n", k.fp);
		client_end(c, "allowed\n");
	} else if (pr.asking >= 0) {
		client_end(c, "busy\n");
	} else {
		printf("phone-ui: pairing: asking for %s %s\n", k.fp, k.comment);
		pr.asking = c - pr.clients;
		pr.asked = lv_tick_get();
		free(pr.key);
		pr.key = line;
		line = NULL;
		ask(&k);
	}
	free(line);
}

static void listen_start(void)
{
	pr.tried = lv_tick_get();
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PAIRING_PORT) };
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, PAIRING_DEVICE, sizeof(PAIRING_DEVICE)) != 0 ||
	    bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, CLIENTS) != 0) {
		close(fd);
		return;
	}
	pr.fd = fd;
	printf("phone-ui: pairing: listening on " PAIRING_DEVICE ", port %d\n", PAIRING_PORT);
}

static void poll_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (pr.fd < 0) {
		if (lv_tick_elaps(pr.tried) >= RETRY_MS)
			listen_start();
		if (pr.fd < 0)
			return;
	}

	int fd;
	while ((fd = accept(pr.fd, NULL, NULL)) >= 0) {
		struct client *c = NULL;
		for (int i = 0; i < CLIENTS && !c; i++)
			if (pr.clients[i].fd < 0)
				c = &pr.clients[i];
		if (!c) {
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		c->fd = fd;
		c->len = 0;
		c->since = lv_tick_get();
	}

	for (int i = 0; i < CLIENTS; i++) {
		struct client *c = &pr.clients[i];
		if (c->fd < 0)
			continue;
		if (i == pr.asking) {
			/* Gone (Ctrl+C), or unanswered */
			char b;
			ssize_t n = recv(c->fd, &b, 1, MSG_PEEK);
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
				answered(NULL);
			else if (lv_tick_elaps(pr.asked) >= ASK_MS)
				answered("denied");
			else
				lv_display_trigger_activity(NULL); /* awake while asking (power.c) */
			continue;
		}
		ssize_t n = read(c->fd, c->line + c->len, sizeof(c->line) - 1 - c->len);
		if (n > 0)
			c->len += n;
		c->line[c->len] = '\0';
		if (memchr(c->line, '\n', c->len))
			request(c);
		else if (n == 0 || c->len == sizeof(c->line) - 1)
			client_end(c, "invalid\n");
		else if (lv_tick_elaps(c->since) >= LINE_MS)
			client_end(c, NULL);
	}
}

void pairing_init(void)
{
	for (int i = 0; i < CLIENTS; i++)
		pr.clients[i].fd = -1;
	listen_start();
	lv_timer_create(poll_cb, POLL_MS, NULL);
}
