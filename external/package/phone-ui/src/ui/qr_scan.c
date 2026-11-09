/*
 * QR codes read from a camera's frames: the UI thread hands a frame's brightness over
 * (UYVY's Y, 2 x 2 pixels averaged: a 1280x720 frame takes quirc about a quarter of the
 * time at 640x360, and a code filling a tenth of the frame still has modules of 2 px or
 * more), a thread of its own finds and decodes the codes in it with quirc and keeps the
 * first one read for the UI thread to take.
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qr.h"
#include "quirc/quirc.h"

struct ui_qr_scan {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct quirc *q;
	uint8_t *gray; /* the frame handed over, at half its size */
	int w, h;
	bool pending, busy, done, quit;
	/* The outcome */
	bool found;
	char text[QUIRC_MAX_PAYLOAD + 1];
	float corners[4][2];
};

/* Decodes the frame's codes, the first one read kept; with the lock held, and let go
 * meanwhile */
static void scan(struct ui_qr_scan *s)
{
	int w = s->w, h = s->h;
	if (quirc_resize(s->q, w, h) < 0) {
		s->found = false;
		s->done = true;
		return;
	}
	memcpy(quirc_begin(s->q, NULL, NULL), s->gray, (size_t)w * h);
	pthread_mutex_unlock(&s->lock);

	struct quirc_code code;
	struct quirc_data data;
	bool found = false;
	quirc_end(s->q);
	for (int i = 0; i < quirc_count(s->q) && !found; i++) {
		quirc_extract(s->q, i, &code);
		quirc_decode_error_t err = quirc_decode(&code, &data);
		if (err == QUIRC_ERROR_DATA_ECC) { /* seen in a mirror: the front camera */
			quirc_flip(&code);
			err = quirc_decode(&code, &data);
		}
		found = err == QUIRC_SUCCESS;
	}

	pthread_mutex_lock(&s->lock);
	s->found = found;
	if (found) {
		memcpy(s->text, data.payload, (size_t)data.payload_len);
		s->text[data.payload_len] = '\0';
		for (int i = 0; i < 4; i++) {
			s->corners[i][0] = (float)code.corners[i].x * 2;
			s->corners[i][1] = (float)code.corners[i].y * 2;
		}
	}
	s->done = true;
}

static void *thread_main(void *arg)
{
	struct ui_qr_scan *s = arg;
	pthread_mutex_lock(&s->lock);
	for (;;) {
		while (!s->pending && !s->quit)
			pthread_cond_wait(&s->cond, &s->lock);
		if (s->quit)
			break;
		s->pending = false;
		s->busy = true;
		scan(s);
		s->busy = false;
	}
	pthread_mutex_unlock(&s->lock);
	return NULL;
}

struct ui_qr_scan *ui_qr_scan_create(void)
{
	struct ui_qr_scan *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->q = quirc_new();
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->cond, NULL);
	if (!s->q || pthread_create(&s->thread, NULL, thread_main, s) != 0) {
		if (s->q)
			quirc_destroy(s->q);
		pthread_mutex_destroy(&s->lock);
		pthread_cond_destroy(&s->cond);
		free(s);
		return NULL;
	}
	return s;
}

void ui_qr_scan_delete(struct ui_qr_scan *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	s->quit = true;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);
	pthread_join(s->thread, NULL);
	quirc_destroy(s->q);
	pthread_mutex_destroy(&s->lock);
	pthread_cond_destroy(&s->cond);
	free(s->gray);
	free(s);
}

bool ui_qr_scan_uyvy(struct ui_qr_scan *s, const uint8_t *uyvy, int width, int height, int stride)
{
	int w = width / 2, h = height / 2;
	pthread_mutex_lock(&s->lock);
	if (s->pending || s->busy) {
		pthread_mutex_unlock(&s->lock);
		return false;
	}
	if (w != s->w || h != s->h) {
		free(s->gray);
		s->gray = malloc((size_t)w * h);
		s->w = s->gray ? w : 0;
		s->h = s->gray ? h : 0;
	}
	if (s->gray) {
		/* U Y V Y: a line's two pixels of each group, and the next line's */
		for (int y = 0; y < h; y++) {
			const uint8_t *a = uyvy + (size_t)y * 2 * stride + 1, *b = a + stride;
			uint8_t *out = s->gray + (size_t)y * w;
			for (int x = 0; x < w; x++, a += 4, b += 4)
				out[x] = (uint8_t)((a[0] + a[2] + b[0] + b[2] + 2) / 4);
		}
		s->pending = true;
		pthread_cond_signal(&s->cond);
	}
	pthread_mutex_unlock(&s->lock);
	return s->gray != NULL;
}

bool ui_qr_scan_result(struct ui_qr_scan *s, bool *found, char *text, size_t size, float corners[4][2])
{
	pthread_mutex_lock(&s->lock);
	bool done = s->done;
	if (done) {
		s->done = false;
		*found = s->found;
		if (s->found) {
			snprintf(text, size, "%s", s->text);
			memcpy(corners, s->corners, sizeof(s->corners));
		}
	}
	pthread_mutex_unlock(&s->lock);
	return done;
}

/* What it says */

/* A WIFI: or MECARD: code's field `key` (WIFI's T, S, P, H; MECARD's N, TEL), its escapes
 * (\; \, \: \" \\) undone: false if none */
static bool field(const char *fields, const char *key, char *out, size_t size)
{
	size_t len = strlen(key);
	const char *p = fields;
	while (*p) {
		bool match = strncasecmp(p, key, len) == 0 && p[len] == ':';
		const char *v = p + (match ? len + 1 : 0);
		size_t n = 0;
		for (; *v && *v != ';'; v++) {
			if (*v == '\\' && v[1])
				v++;
			if (match && n + 1 < size)
				out[n++] = *v;
		}
		if (match) {
			out[n] = '\0';
			return true;
		}
		p = *v ? v + 1 : v;
	}
	return false;
}

/* vCards: their text values escape \ ; , and newlines with a backslash */

static size_t vcard_escape(char *out, size_t size, size_t len, const char *s, size_t n)
{
	for (size_t i = 0; i < n && len + 3 < size; i++) {
		if (strchr("\\;,", s[i]))
			out[len++] = '\\';
		out[len++] = s[i] == '\n' ? ' ' : s[i];
	}
	out[len < size ? len : size - 1] = '\0';
	return len;
}

void ui_qr_vcard(char *out, size_t size, const char *name, const char *number)
{
	/* N: the last word as the family name, the rest as the given ones */
	const char *last = strrchr(name, ' ');
	size_t len = (size_t)snprintf(out, size, "BEGIN:VCARD\r\nVERSION:3.0\r\nN:");
	if (last)
		len = vcard_escape(out, size, len, last + 1, strlen(last + 1));
	len += (size_t)snprintf(out + len, size > len ? size - len : 0, ";");
	len = vcard_escape(out, size, len, name, last ? (size_t)(last - name) : strlen(name));
	len += (size_t)snprintf(out + len, size > len ? size - len : 0, ";;;\r\nFN:");
	len = vcard_escape(out, size, len, name, strlen(name));
	snprintf(out + len, size > len ? size - len : 0, "\r\nTEL;TYPE=CELL:%s\r\nEND:VCARD\r\n", number);
}

/* The `index`th of a value's components split at unescaped ';' (N's family and given
 * names), unescaped */
static void vcard_component(const char *v, size_t len, int index, char *out, size_t size)
{
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		if (v[i] == ';') {
			index--;
			continue;
		}
		char ch = v[i];
		if (ch == '\\' && i + 1 < len) {
			ch = v[++i];
			ch = ch == 'n' || ch == 'N' ? ' ' : ch;
		}
		if (index == 0 && n + 1 < size)
			out[n++] = ch;
	}
	out[n] = '\0';
}

/* "given family", either missing */
static void name_join(char *out, size_t size, const char *given, const char *family)
{
	snprintf(out, size, "%s%s%s", given, given[0] && family[0] ? " " : "", family);
}

/* BEGIN:VCARD...: FN (else N's names) and a mobile number (else the first) */
static bool vcard_read(const char *text, struct ui_qr_content *c)
{
	char fn[sizeof(c->name)] = "", given[sizeof(c->name)] = "", family[sizeof(c->name)] = "";
	char number[sizeof(c->number)] = "";
	bool cell = false;
	/* Lines unfolded: a line break followed by a space or tab goes */
	size_t size = strlen(text) + 1, n = 0;
	char *s = malloc(size);
	if (!s)
		return false;
	for (const char *p = text; *p; p++) {
		if (*p == '\r')
			continue;
		if (*p == '\n' && (p[1] == ' ' || p[1] == '\t')) {
			p++;
			continue;
		}
		s[n++] = *p;
	}
	s[n] = '\0';

	for (char *line = s; *line; ) {
		char *end = line + strcspn(line, "\n"), *colon = memchr(line, ':', (size_t)(end - line));
		if (colon) {
			char *name = line, *dot = memchr(line, '.', (size_t)(colon - line));
			size_t name_len = strcspn(line, ";:");
			if (dot && dot < line + name_len) { /* a group: item1.TEL */
				name = dot + 1;
				name_len = strcspn(name, ";:");
			}
			const char *params = name + name_len, *v = colon + 1;
			size_t len = (size_t)(end - v);
			if (name_len == 2 && strncasecmp(name, "FN", 2) == 0) {
				vcard_component(v, len, 0, fn, sizeof(fn));
			} else if (name_len == 1 && strncasecmp(name, "N", 1) == 0) {
				vcard_component(v, len, 0, family, sizeof(family));
				vcard_component(v, len, 1, given, sizeof(given));
			} else if (name_len == 3 && strncasecmp(name, "TEL", 3) == 0 && !cell) {
				char p[128];
				snprintf(p, sizeof(p), "%.*s", (int)(colon - params), params);
				for (char *q = p; *q; q++)
					*q = (char)toupper((unsigned char)*q);
				bool mobile = strstr(p, "CELL") != NULL;
				if (strncasecmp(v, "tel:", 4) == 0)
					v += 4, len -= 4;
				if (len && (mobile || !number[0])) {
					snprintf(number, sizeof(number), "%.*s", (int)len, v);
					cell = mobile;
				}
			}
		}
		line = *end ? end + 1 : end;
	}
	free(s);
	if (fn[0])
		snprintf(c->name, sizeof(c->name), "%s", fn);
	else
		name_join(c->name, sizeof(c->name), given, family);
	snprintf(c->number, sizeof(c->number), "%s", number);
	return c->name[0] || c->number[0];
}

void ui_qr_parse(const char *text, struct ui_qr_content *c)
{
	memset(c, 0, sizeof(*c));
	if (strncasecmp(text, "WIFI:", 5) == 0 && field(text + 5, "S", c->ssid, sizeof(c->ssid)) && c->ssid[0]) {
		char hidden[8] = "";
		c->kind = UI_QR_WIFI;
		field(text + 5, "T", c->type, sizeof(c->type));
		field(text + 5, "P", c->password, sizeof(c->password));
		field(text + 5, "H", hidden, sizeof(hidden));
		c->hidden = strcasecmp(hidden, "true") == 0;
	} else if (strncasecmp(text, "BEGIN:VCARD", 11) == 0 && vcard_read(text, c)) {
		c->kind = UI_QR_CONTACT;
	} else if (strncasecmp(text, "MECARD:", 7) == 0) {
		char n[sizeof(c->name)] = "", *comma;
		field(text + 7, "N", n, sizeof(n));
		field(text + 7, "TEL", c->number, sizeof(c->number));
		if ((comma = strchr(n, ','))) { /* family,given */
			*comma = '\0';
			name_join(c->name, sizeof(c->name), comma + 1, n);
		} else {
			snprintf(c->name, sizeof(c->name), "%s", n);
		}
		c->kind = c->name[0] || c->number[0] ? UI_QR_CONTACT : UI_QR_TEXT;
	} else if (strncasecmp(text, "http://", 7) == 0 || strncasecmp(text, "https://", 8) == 0) {
		c->kind = UI_QR_URL;
	} else if (strncasecmp(text, "tel:", 4) == 0 && text[4]) {
		c->kind = UI_QR_PHONE;
		size_t n = 0;
		for (const char *p = text + 4; *p && n + 1 < sizeof(c->number); p++)
			if (isdigit((unsigned char)*p) || strchr("+*#", *p))
				c->number[n++] = *p;
		c->number[n] = '\0';
		if (!n)
			c->kind = UI_QR_TEXT;
	}
}
