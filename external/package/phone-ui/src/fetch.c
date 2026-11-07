/*
 * HTTP requests one at a time per struct ui_fetch (Weather, Assistant), see
 * ui.h: libcurl's multi interface polled by a timer. Each request has handles of its
 * own, but all share one cache of connections, DNS answers and TLS sessions: a request
 * to a host just asked takes the same connection, without its TCP and TLS handshakes
 * (0.47 s for a GitHub request on the phone over Wi-Fi, 0.30 s with a connection
 * reused). Idle connections are closed after MAXAGE_CONN: one made over Wi-Fi may be
 * dead without saying so once mobile data took over, and a request on it would wait.
 */

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "ui.h"

#define USER_AGENT "phone-ui/1.0 (PinePhone feature phone firmware)" /* NON-NLS */
#define MAXAGE_CONN 60L /* seconds an idle connection is reused within */

/* The requests' shared caches (NULL if libcurl could not make it: none shared). The UI's
 * thread is the only one using it, so it needs no locks */
static CURLSH *share;

static size_t write_cb(char *data, size_t size, size_t n, void *user)
{
	struct ui_fetch *f = user;
	/* One more byte, for the terminating NUL */
	char *p = realloc(f->data, f->size + size * n + 1);
	if (!p)
		return 0;
	memcpy(p + f->size, data, size * n);
	f->data = p;
	f->size += size * n;
	p[f->size] = '\0';
	return size * n;
}

void ui_fetch_stop(struct ui_fetch *f)
{
	if (f->timer)
		lv_timer_delete(f->timer);
	if (f->curl) {
		curl_multi_remove_handle(f->multi, f->curl);
		curl_easy_cleanup(f->curl);
	}
	if (f->multi)
		curl_multi_cleanup(f->multi);
	curl_slist_free_all(f->headers);
	free(f->data);
	memset(f, 0, sizeof(*f));
}

static void fetch_tick(lv_timer_t *timer)
{
	struct ui_fetch *f = lv_timer_get_user_data(timer);
	int running, left;
	size_t before = f->size;
	curl_multi_perform(f->multi, &running);
	CURLMsg *m;
	while ((m = curl_multi_info_read(f->multi, &left))) {
		if (m->msg != CURLMSG_DONE)
			continue;
		long code = 0;
		curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &code);
		char *url = NULL;
		curl_easy_getinfo(f->curl, CURLINFO_EFFECTIVE_URL, &url);
		if (m->data.result != CURLE_OK)
			printf("phone-ui: %s: %s\n", url ? url : "", curl_easy_strerror(m->data.result));
		else if (code / 100 != 2)
			printf("phone-ui: %s: HTTP %ld\n", url ? url : "", code);
		if (m->data.result != CURLE_OK)
			code = 0;
		/* Stopped first: done() may start the next one */
		char *data = f->data;
		size_t size = f->size;
		void (*done)(const char *, long) = f->done;
		f->data = NULL;
		ui_fetch_stop(f);
		/* Its size for done() (binary data), until the next request */
		f->size = size;
		done(data, code);
		free(data);
		return;
	}
	if (f->size != before && f->progress)
		f->progress(f->data, f->size);
}

static CURL *fetch_init(struct ui_fetch *f, const char *url, void (*done)(const char *, long))
{
	static bool curl_ready;
	if (!curl_ready) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_ready = true;
		share = curl_share_init();
		if (share) {
			curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
			curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
			curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
		}
	}
	ui_fetch_stop(f);
	f->multi = curl_multi_init();
	f->curl = curl_easy_init();
	if (!f->multi || !f->curl) {
		ui_fetch_stop(f);
		if (done)
			done(NULL, 0);
		return NULL;
	}
	f->done = done;
	curl_easy_setopt(f->curl, CURLOPT_URL, url);
	curl_easy_setopt(f->curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(f->curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(f->curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if (share)
		curl_easy_setopt(f->curl, CURLOPT_SHARE, share);
	curl_easy_setopt(f->curl, CURLOPT_MAXAGE_CONN, MAXAGE_CONN);
	return f->curl;
}

static void fetch_run(struct ui_fetch *f)
{
	curl_multi_add_handle(f->multi, f->curl);
	f->timer = lv_timer_create(fetch_tick, 50, f);
}

void ui_fetch_start(struct ui_fetch *f, const char *url, void (*done)(const char *data, long status))
{
	if (!fetch_init(f, url, done))
		return;
	curl_easy_setopt(f->curl, CURLOPT_TIMEOUT, 30L);
	fetch_run(f);
}

static void fetch_headers(struct ui_fetch *f, const char *const *headers)
{
	for (; headers && *headers; headers++)
		f->headers = curl_slist_append(f->headers, *headers);
	curl_easy_setopt(f->curl, CURLOPT_HTTPHEADER, (struct curl_slist *)f->headers);
}

void ui_fetch_request(struct ui_fetch *f, const char *method, const char *url, const char *const *headers,
                      const char *body, void (*done)(const char *data, long status))
{
	if (!fetch_init(f, url, done))
		return;
	fetch_headers(f, headers);
	if (body)
		curl_easy_setopt(f->curl, CURLOPT_COPYPOSTFIELDS, body);
	curl_easy_setopt(f->curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(f->curl, CURLOPT_TIMEOUT, 30L);
	fetch_run(f);
}

void ui_fetch_post(struct ui_fetch *f, const char *url, const char *const *headers, const char *body,
                   void (*progress)(const char *data, size_t size),
                   void (*done)(const char *data, long status))
{
	if (!fetch_init(f, url, done))
		return;
	fetch_headers(f, headers);
	curl_easy_setopt(f->curl, CURLOPT_COPYPOSTFIELDS, body);
	/* Answers can take minutes: only a connection that went quiet ends it */
	curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_TIME, 120L);
	f->progress = progress;
	fetch_run(f);
}


/* Sockets (ui.h): libcurl connects (CURLOPT_CONNECT_ONLY on a multi handle, polled as
 * the requests are) and then hands the connection over to curl_easy_send()/recv() */

#define SOCKET_POLL_MS 50
#define RECV_CHUNK 16384

void ui_socket_close(struct ui_socket *s)
{
	if (s->timer)
		lv_timer_delete(s->timer);
	if (s->curl) {
		if (s->multi)
			curl_multi_remove_handle(s->multi, s->curl);
		curl_easy_cleanup(s->curl);
	}
	if (s->multi)
		curl_multi_cleanup(s->multi);
	free(s->out);
	memset(s, 0, sizeof(*s));
}

/* What waits is sent, as much as the connection takes */
static void socket_flush(struct ui_socket *s)
{
	while (s->out_len) {
		size_t n = 0;
		CURLcode r = curl_easy_send(s->curl, s->out, s->out_len, &n);
		if (r == CURLE_AGAIN || (r == CURLE_OK && !n))
			return;
		if (r != CURLE_OK) {
			printf("phone-ui: socket: %s\n", curl_easy_strerror(r));
			return;
		}
		memmove(s->out, s->out + n, s->out_len - n);
		s->out_len -= n;
	}
}

static void socket_tick(lv_timer_t *timer)
{
	struct ui_socket *s = lv_timer_get_user_data(timer);
	if (!s->connected) {
		int running, left;
		curl_multi_perform(s->multi, &running);
		CURLMsg *m;
		while ((m = curl_multi_info_read(s->multi, &left))) {
			if (m->msg != CURLMSG_DONE)
				continue;
			if (m->data.result != CURLE_OK) {
				char *url = NULL;
				curl_easy_getinfo(s->curl, CURLINFO_EFFECTIVE_URL, &url);
				printf("phone-ui: %s: %s\n", url ? url : "", curl_easy_strerror(m->data.result));
				void (*opened)(struct ui_socket *, long) = s->opened;
				void *user_data = s->user_data;
				ui_socket_close(s);
				s->user_data = user_data; /* for the callback */
				opened(s, -(long)m->data.result);
				return;
			}
			s->connected = true;
			s->opened(s, 0);
			if (!s->curl) /* closed by opened() */
				return;
			break;
		}
		if (!s->connected)
			return;
	}
	socket_flush(s);
	char buf[RECV_CHUNK];
	for (;;) {
		size_t n = 0;
		CURLcode r = curl_easy_recv(s->curl, buf, sizeof(buf), &n);
		if (r == CURLE_AGAIN)
			return;
		if (r != CURLE_OK || n == 0) { /* hung up, or broken */
			if (r != CURLE_OK)
				printf("phone-ui: socket: %s\n", curl_easy_strerror(r));
			void (*received)(struct ui_socket *, const char *, size_t) = s->received;
			void *user_data = s->user_data;
			ui_socket_close(s);
			s->user_data = user_data; /* for the callback */
			received(s, NULL, 0);
			return;
		}
		s->received(s, buf, n);
		if (!s->curl) /* closed by received() */
			return;
	}
}

void ui_socket_open(struct ui_socket *s, const char *url, void (*opened)(struct ui_socket *s, long status),
                    void (*received)(struct ui_socket *s, const char *data, size_t size), void *user_data)
{
	ui_socket_close(s);
	s->opened = opened;
	s->received = received;
	s->user_data = user_data;
	/* The requests' caches too: a server just asked over HTTP is resolved */
	struct ui_fetch f = {0};
	if (!fetch_init(&f, url, NULL)) {
		opened(s, -(long)CURLE_FAILED_INIT);
		return;
	}
	s->multi = f.multi;
	s->curl = f.curl;
	curl_easy_setopt(s->curl, CURLOPT_CONNECT_ONLY, 1L);
	curl_multi_add_handle(s->multi, s->curl);
	s->timer = lv_timer_create(socket_tick, SOCKET_POLL_MS, s);
}

void ui_socket_send(struct ui_socket *s, const void *data, size_t size)
{
	if (!s->curl)
		return;
	char *p = realloc(s->out, s->out_len + size);
	if (!p)
		return;
	memcpy(p + s->out_len, data, size);
	s->out = p;
	s->out_len += size;
	if (s->connected)
		socket_flush(s);
}
