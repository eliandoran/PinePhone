/*
 * libcurl in the browser: the easy, multi and share functions phone-ui calls (as
 * ../tests/fake_curl.c fakes them), the transfers made with the page's fetch(). A
 * transfer starts when its handle is added to a multi handle; JavaScript keeps what
 * arrives, and curl_multi_perform() hands it to the write callback, as libcurl does.
 *
 * The browser decides what differs from libcurl: it follows redirects itself (always),
 * decompresses, sends its own User-Agent, and refuses what another site does not let
 * pages read (CORS: most web pages), which fails as CURLE_COULDNT_CONNECT.
 */

#define CURL_DISABLE_TYPECHECK /* curl.h's checking macros would rename these definitions */
#include <ctype.h>
#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define MAX_HANDLES 32
#define READ_SIZE 65536 /* handed to the write callback at a time */

struct easy {
	char *url;
	curl_write_callback write;
	void *write_data;
	void *private;
	const struct curl_slist *headers;
	char *post;
	long timeout_ms;
	/* The transfer */
	int id; /* JavaScript's, 0: none */
	struct multi *multi;
	char *effective, *type;
	long code;
	bool done;
};

struct multi {
	struct easy *handles[MAX_HANDLES];
	int count;
	CURLMsg msgs[MAX_HANDLES];
	int msg_count;
};

EM_JS_DEPS(curl, "$UTF8ToString,$stringToNewUTF8");

/* fetch(): headers as "Name: value" lines, a POST when there is a body; aborted after
 * timeout_ms (0: never). What arrives is kept until read. */
EM_JS(void, js_fetch_start, (int id, const char *url, const char *headers, const char *body,
                             int timeout_ms, int err_connect, int err_timeout), {
	const t = { status: 0, url: "", type: "", chunks: [], done: false, error: 0 };
	Module.curlTransfers = Module.curlTransfers || {};
	Module.curlTransfers[id] = t;
	const init = { headers: {}, redirect: "follow" };
	for (const line of UTF8ToString(headers).split("\n")) {
		const i = line.indexOf(":");
		if (i > 0)
			init.headers[line.slice(0, i).trim()] = line.slice(i + 1).trim();
	}
	if (body) {
		init.method = "POST";
		init.body = UTF8ToString(body);
	}
	t.abort = new AbortController();
	init.signal = t.abort.signal;
	if (timeout_ms > 0)
		t.timer = setTimeout(() => { t.timedOut = true; t.abort.abort(); }, timeout_ms);
	fetch(UTF8ToString(url), init).then(async res => {
		t.status = res.status;
		t.url = res.url;
		t.type = res.headers.get("content-type") || "";
		const reader = res.body.getReader();
		for (;;) {
			const { done, value } = await reader.read();
			if (done)
				break;
			t.chunks.push(value);
		}
	}).catch(() => {
		t.error = t.timedOut ? err_timeout : err_connect;
	}).finally(() => {
		t.done = true;
		clearTimeout(t.timer);
	});
});

/* Copies what arrived into buf: its size, 0 if nothing yet, -1 at the end, -2 on error */
EM_JS(int, js_fetch_read, (int id, char *buf, int size), {
	const t = Module.curlTransfers[id];
	let n = 0;
	while (t.chunks.length && n < size) {
		const c = t.chunks[0];
		const k = Math.min(size - n, c.length);
		HEAPU8.set(c.subarray(0, k), buf + n);
		n += k;
		if (k == c.length)
			t.chunks.shift();
		else
			t.chunks[0] = c.subarray(k);
	}
	if (n)
		return n;
	if (!t.done)
		return 0;
	return t.error ? -2 : -1;
});

EM_JS(int, js_fetch_status, (int id), { return Module.curlTransfers[id].status; });
EM_JS(int, js_fetch_error, (int id), { return Module.curlTransfers[id].error; });

/* The URL after redirects or the content type, malloc'd */
EM_JS(char *, js_fetch_string, (int id, bool type), {
	const t = Module.curlTransfers[id];
	return stringToNewUTF8(type ? t.type : t.url);
});

EM_JS(void, js_fetch_end, (int id), {
	const t = Module.curlTransfers[id];
	if (!t.done)
		t.abort.abort();
	delete Module.curlTransfers[id];
});

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

/* Easy handles */

CURLcode curl_global_init(long flags)
{
	(void)flags;
	return CURLE_OK;
}

CURL *curl_easy_init(void)
{
	return calloc(1, sizeof(struct easy));
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
	struct easy *e = curl;
	va_list ap;
	va_start(ap, option);
	switch (option) {
	case CURLOPT_URL:
		free(e->url);
		e->url = dup_or_null(va_arg(ap, const char *));
		break;
	case CURLOPT_WRITEFUNCTION:
		e->write = va_arg(ap, curl_write_callback);
		break;
	case CURLOPT_WRITEDATA:
		e->write_data = va_arg(ap, void *);
		break;
	case CURLOPT_PRIVATE:
		e->private = va_arg(ap, void *);
		break;
	case CURLOPT_HTTPHEADER:
		e->headers = va_arg(ap, const struct curl_slist *);
		break;
	case CURLOPT_COPYPOSTFIELDS:
		free(e->post);
		e->post = dup_or_null(va_arg(ap, const char *));
		break;
	case CURLOPT_TIMEOUT:
		e->timeout_ms = va_arg(ap, long) * 1000;
		break;
	default:
		/* Redirects, connection timeouts, the user agent, encodings: the browser's */
		if (option < CURLOPTTYPE_OBJECTPOINT)
			(void)va_arg(ap, long);
		else if (option >= CURLOPTTYPE_OFF_T && option < CURLOPTTYPE_BLOB)
			(void)va_arg(ap, curl_off_t);
		else
			(void)va_arg(ap, void *);
		break;
	}
	va_end(ap);
	return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
	struct easy *e = curl;
	va_list ap;
	va_start(ap, info);
	CURLcode result = CURLE_OK;
	switch (info) {
	case CURLINFO_RESPONSE_CODE:
		*va_arg(ap, long *) = e->code;
		break;
	case CURLINFO_EFFECTIVE_URL:
		*va_arg(ap, char **) = e->effective ? e->effective : e->url;
		break;
	case CURLINFO_CONTENT_TYPE:
		*va_arg(ap, char **) = e->type && *e->type ? e->type : NULL;
		break;
	case CURLINFO_PRIVATE:
		*va_arg(ap, char **) = e->private;
		break;
	default:
		result = CURLE_BAD_FUNCTION_ARGUMENT;
		break;
	}
	va_end(ap);
	return result;
}

static void multi_detach(struct multi *m, struct easy *e);

/* The transfer's end on JavaScript's side, and what it found */
static void transfer_forget(struct easy *e)
{
	if (e->id)
		js_fetch_end(e->id);
	e->id = 0;
	free(e->effective);
	free(e->type);
	e->effective = e->type = NULL;
}

void curl_easy_cleanup(CURL *curl)
{
	struct easy *e = curl;
	if (!e)
		return;
	if (e->multi)
		multi_detach(e->multi, e);
	transfer_forget(e);
	free(e->url);
	free(e->post);
	free(e);
}

const char *curl_easy_strerror(CURLcode code)
{
	switch (code) {
	case CURLE_OK: return "No error";
	case CURLE_COULDNT_CONNECT: return "Could not connect to server";
	case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
	case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
	default: return "Error";
	}
}

/* As curl: all but letters, digits and -._~ as %XX */
char *curl_easy_escape(CURL *curl, const char *s, int length)
{
	(void)curl;
	size_t n = length ? (size_t)length : strlen(s);
	char *out = malloc(n * 3 + 1), *o = out;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
			*o++ = (char)c;
		else
			o += sprintf(o, "%%%02X", c);
	}
	*o = '\0';
	return out;
}

void curl_free(void *p)
{
	free(p);
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
	struct curl_slist *item = malloc(sizeof(*item));
	item->data = strdup(data);
	item->next = NULL;
	if (!list)
		return item;
	struct curl_slist *last = list;
	while (last->next)
		last = last->next;
	last->next = item;
	return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
	while (list) {
		struct curl_slist *next = list->next;
		free(list->data);
		free(list);
		list = next;
	}
}

/* Multi handles */

CURLM *curl_multi_init(void)
{
	return calloc(1, sizeof(struct multi));
}

CURLMcode curl_multi_add_handle(CURLM *multi, CURL *curl)
{
	static int ids;
	struct multi *m = multi;
	struct easy *e = curl;
	if (e->multi || m->count == MAX_HANDLES)
		return CURLM_ADDED_ALREADY;
	m->handles[m->count++] = e;
	e->multi = m;
	transfer_forget(e);
	e->code = 0;
	e->done = false;

	size_t n = 1;
	for (const struct curl_slist *h = e->headers; h; h = h->next)
		n += strlen(h->data) + 1;
	char *headers = malloc(n), *p = headers;
	*p = '\0';
	for (const struct curl_slist *h = e->headers; h; h = h->next)
		p += sprintf(p, "%s\n", h->data);
	e->id = ++ids;
	js_fetch_start(e->id, e->url ? e->url : "", headers, e->post, (int)e->timeout_ms,
	               CURLE_COULDNT_CONNECT, CURLE_OPERATION_TIMEDOUT);
	free(headers);
	return CURLM_OK;
}

static void multi_detach(struct multi *m, struct easy *e)
{
	for (int i = 0; i < m->count; i++)
		if (m->handles[i] == e)
			m->handles[i--] = m->handles[--m->count];
	for (int i = 0; i < m->msg_count; i++)
		if (m->msgs[i].easy_handle == e) {
			memmove(&m->msgs[i], &m->msgs[i + 1], (size_t)(m->msg_count - i - 1) * sizeof(CURLMsg));
			m->msg_count--;
			i--;
		}
	e->multi = NULL;
}

CURLMcode curl_multi_remove_handle(CURLM *multi, CURL *curl)
{
	struct easy *e = curl;
	if (e && e->multi == multi) {
		multi_detach(multi, e);
		if (!e->done)
			transfer_forget(e);
	}
	return CURLM_OK;
}

static void finish(struct multi *m, struct easy *e, CURLcode result)
{
	e->done = true;
	if (result != CURLE_OK)
		e->code = 0;
	CURLMsg *msg = &m->msgs[m->msg_count++];
	msg->msg = CURLMSG_DONE;
	msg->easy_handle = e;
	msg->data.result = result;
}

/* What arrived for it, to its write callback */
static void transfer(struct multi *m, struct easy *e)
{
	static char buf[READ_SIZE];
	for (;;) {
		int n = js_fetch_read(e->id, buf, sizeof(buf));
		if (n == 0)
			return;
		if (!e->code)
			e->code = js_fetch_status(e->id);
		if (!e->effective) {
			e->effective = js_fetch_string(e->id, false);
			e->type = js_fetch_string(e->id, true);
		}
		if (n == -1) {
			finish(m, e, CURLE_OK);
			return;
		}
		if (n == -2) {
			finish(m, e, (CURLcode)js_fetch_error(e->id));
			return;
		}
		if (e->write && e->write(buf, 1, (size_t)n, e->write_data) != (size_t)n) {
			finish(m, e, CURLE_WRITE_ERROR);
			return;
		}
		if (e->multi != m) /* removed by the write callback */
			return;
	}
}

CURLMcode curl_multi_perform(CURLM *multi, int *running)
{
	struct multi *m = multi;
	/* The handles of the moment: a write callback may remove one */
	struct easy *handles[MAX_HANDLES];
	int count = m->count;
	memcpy(handles, m->handles, sizeof(handles));
	for (int i = 0; i < count; i++) {
		struct easy *e = handles[i];
		bool still_added = false;
		for (int j = 0; j < m->count; j++)
			still_added |= m->handles[j] == e;
		if (still_added && !e->done)
			transfer(m, e);
	}
	int n = 0;
	for (int i = 0; i < m->count; i++)
		n += !m->handles[i]->done;
	*running = n;
	return CURLM_OK;
}

CURLMsg *curl_multi_info_read(CURLM *multi, int *left)
{
	struct multi *m = multi;
	static CURLMsg msg;
	if (!m->msg_count) {
		*left = 0;
		return NULL;
	}
	msg = m->msgs[0];
	memmove(&m->msgs[0], &m->msgs[1], (size_t)(m->msg_count - 1) * sizeof(CURLMsg));
	*left = --m->msg_count;
	return &msg;
}

CURLMcode curl_multi_cleanup(CURLM *multi)
{
	struct multi *m = multi;
	if (!m)
		return CURLM_OK;
	while (m->count)
		curl_multi_remove_handle(m, m->handles[0]);
	free(m);
	return CURLM_OK;
}

/* Share handles: the browser keeps its connections, DNS answers and TLS sessions
 * itself; a share only has to exist */

CURLSH *curl_share_init(void)
{
	return calloc(1, 1);
}

CURLSHcode curl_share_setopt(CURLSH *share, CURLSHoption option, ...)
{
	(void)share;
	(void)option;
	return CURLSHE_OK;
}

CURLSHcode curl_share_cleanup(CURLSH *share)
{
	free(share);
	return CURLSHE_OK;
}
