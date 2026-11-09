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
#include <time.h>

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
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
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
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (found) /* how long a frame takes, on the phone */
		printf("qr: read in %ld ms at %dx%d\n",
		       (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000), w, h);

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

/* A WIFI: code's field `key` (T, S, P, H), its escapes (\; \, \: \" \\) undone: false if none */
static bool wifi_field(const char *fields, char key, char *out, size_t size)
{
	const char *p = fields;
	while (*p) {
		bool match = toupper((unsigned char)p[0]) == key && p[1] == ':';
		const char *v = p + (match ? 2 : 0);
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

void ui_qr_parse(const char *text, struct ui_qr_content *c)
{
	memset(c, 0, sizeof(*c));
	if (strncasecmp(text, "WIFI:", 5) == 0 && wifi_field(text + 5, 'S', c->ssid, sizeof(c->ssid)) && c->ssid[0]) {
		char hidden[8] = "";
		c->kind = UI_QR_WIFI;
		wifi_field(text + 5, 'T', c->type, sizeof(c->type));
		wifi_field(text + 5, 'P', c->password, sizeof(c->password));
		wifi_field(text + 5, 'H', hidden, sizeof(hidden));
		c->hidden = strcasecmp(hidden, "true") == 0;
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
