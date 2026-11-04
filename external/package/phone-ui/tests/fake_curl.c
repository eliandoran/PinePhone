/*
 * libcurl for the UI tests (ui_test.h, "The network"): the easy and multi
 * functions phone-ui calls, answering from what the test serves instead of the
 * network (the sandbox has none). A request starts when its handle is added to a
 * multi handle and is answered by the next curl_multi_perform(), unless the
 * network is held; an address nothing serves fails as offline. phone-ui's own
 * code is unchanged: the Makefile links this instead of -lcurl.
 */

#define _GNU_SOURCE /* strdup */
#define CURL_DISABLE_TYPECHECK /* curl.h's checking macros would rename these definitions */
#include <ctype.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "ui_test.h"

#define MAX_RESPONSES 64
#define MAX_REQUESTS 256
#define MAX_HANDLES 32

struct response {
	char *pattern; /* fnmatch() of the URL */
	long status;
	char *type, *location;
	char *body;
	size_t size;
	int error; /* CURLcode */
	bool once, used; /* answers one request */
};

static struct response responses[MAX_RESPONSES];
static unsigned response_count;
static struct ui_test_request requests[MAX_REQUESTS];
static unsigned request_count;
static bool held;
static size_t piece; /* bytes written per curl_multi_perform(), 0: all */

struct easy {
	char *url;
	curl_write_callback write;
	void *write_data;
	void *private;
	const struct curl_slist *headers;
	char *post;
	bool follow;
	long max_redirs;
	/* The transfer */
	struct multi *multi;
	const struct response *r; /* answering */
	char *effective;
	size_t sent;
	long code;
	bool done;
};

struct multi {
	struct easy *handles[MAX_HANDLES];
	int count;
	CURLMsg msgs[MAX_HANDLES];
	int msg_count;
};

/* What the tests serve */

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

static struct response *response_add(const char *pattern)
{
	if (response_count == MAX_RESPONSES) {
		fprintf(stderr, "ui_test: more than %d responses served\n", MAX_RESPONSES);
		abort();
	}
	struct response *r = &responses[response_count++];
	memset(r, 0, sizeof(*r));
	r->pattern = strdup(pattern);
	return r;
}

void ui_test_serve_data(const char *url, long status, const char *type, const void *data, size_t size)
{
	struct response *r = response_add(url);
	r->status = status;
	r->type = dup_or_null(type);
	r->body = malloc(size + 1);
	memcpy(r->body, data, size);
	r->size = size;
}

void ui_test_serve(const char *url, long status, const char *type, const char *body)
{
	ui_test_serve_data(url, status, type, body, strlen(body));
}

void ui_test_serve_once(const char *url, long status, const char *type, const char *body)
{
	ui_test_serve(url, status, type, body);
	responses[response_count - 1].once = true;
}

void ui_test_serve_redirect(const char *url, const char *location)
{
	struct response *r = response_add(url);
	r->status = 302;
	r->location = strdup(location);
}

void ui_test_serve_error(const char *url, int error)
{
	response_add(url)->error = error;
}

void ui_test_network_hold(bool hold)
{
	held = hold;
}

void ui_test_serve_in_pieces(size_t bytes)
{
	piece = bytes;
}

unsigned ui_test_request_count(void)
{
	return request_count;
}

const struct ui_test_request *ui_test_request(int i)
{
	if (i < 0)
		i += (int)request_count;
	return i >= 0 && (unsigned)i < request_count ? &requests[i] : NULL;
}

const struct ui_test_request *ui_test_requested(const char *pattern)
{
	for (unsigned i = request_count; i-- > 0;)
		if (fnmatch(pattern, requests[i].url, 0) == 0)
			return &requests[i];
	return NULL;
}

/* The last served first: a test can serve another answer for the same address */
static const struct response *response_find(const char *url)
{
	for (unsigned i = response_count; i-- > 0;)
		if (!responses[i].used && fnmatch(responses[i].pattern, url, 0) == 0)
			return &responses[i];
	return NULL;
}

static void request_log(const struct easy *e)
{
	if (request_count == MAX_REQUESTS)
		return;
	struct ui_test_request *q = &requests[request_count++];
	q->url = strdup(e->url ? e->url : "");
	q->body = dup_or_null(e->post);
	size_t n = 1;
	for (const struct curl_slist *h = e->headers; h; h = h->next)
		n += strlen(h->data) + 1;
	char *headers = malloc(n), *p = headers;
	for (const struct curl_slist *h = e->headers; h; h = h->next)
		p += sprintf(p, "%s\n", h->data);
	*p = '\0';
	q->headers = headers;
}

/* Easy handles */

CURLcode curl_global_init(long flags)
{
	(void)flags;
	return CURLE_OK;
}

CURL *curl_easy_init(void)
{
	struct easy *e = calloc(1, sizeof(*e));
	e->max_redirs = 30;
	return e;
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
	case CURLOPT_FOLLOWLOCATION:
		e->follow = va_arg(ap, long) != 0;
		break;
	case CURLOPT_MAXREDIRS:
		e->max_redirs = va_arg(ap, long);
		break;
	default:
		/* Timeouts, the user agent, encodings: nothing to do here */
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
		*va_arg(ap, char **) = e->code && e->r ? e->r->type : NULL;
		break;
	case CURLINFO_PRIVATE:
		*va_arg(ap, char **) = e->private;
		break;
	default:
		fprintf(stderr, "ui_test: curl_easy_getinfo(%d) is not faked\n", (int)info);
		result = CURLE_BAD_FUNCTION_ARGUMENT;
		break;
	}
	va_end(ap);
	return result;
}

static void multi_detach(struct multi *m, struct easy *e);

void curl_easy_cleanup(CURL *curl)
{
	struct easy *e = curl;
	if (!e)
		return;
	if (e->multi)
		multi_detach(e->multi, e);
	free(e->url);
	free(e->post);
	free(e->effective);
	free(e);
}

const char *curl_easy_strerror(CURLcode code)
{
	switch (code) {
	case CURLE_OK: return "No error";
	case CURLE_COULDNT_RESOLVE_HOST: return "Could not resolve hostname";
	case CURLE_COULDNT_CONNECT: return "Could not connect to server";
	case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
	case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
	case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
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
	struct multi *m = multi;
	struct easy *e = curl;
	if (e->multi || m->count == MAX_HANDLES)
		return CURLM_ADDED_ALREADY;
	m->handles[m->count++] = e;
	e->multi = m;
	e->r = NULL;
	e->sent = 0;
	e->code = 0;
	e->done = false;
	free(e->effective);
	e->effective = NULL;
	request_log(e);
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
	if (e && e->multi == multi)
		multi_detach(multi, e);
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

/* What answers its URL, after the redirects it follows */
static CURLcode resolve(struct easy *e)
{
	const char *url = e->url;
	long redirs = 0;
	for (;;) {
		e->r = response_find(url);
		if (e->r && e->r->once)
			responses[e->r - responses].used = true;
		if (!e->r) {
			fprintf(stderr, "  ui_test: nothing serves %s (offline)\n", url);
			return CURLE_COULDNT_RESOLVE_HOST;
		}
		if (e->r->error)
			return (CURLcode)e->r->error;
		e->code = e->r->status;
		if (!e->r->location || !e->follow)
			return CURLE_OK;
		if (++redirs > e->max_redirs)
			return CURLE_TOO_MANY_REDIRECTS;
		free(e->effective);
		e->effective = strdup(e->r->location);
		url = e->effective;
	}
}

static void transfer(struct multi *m, struct easy *e)
{
	if (!e->r) {
		CURLcode result = resolve(e);
		if (result != CURLE_OK) {
			finish(m, e, result);
			return;
		}
	}
	size_t n = e->r->size - e->sent;
	if (piece && n > piece)
		n = piece;
	if (n && e->write) {
		/* curl's buffer is writable */
		char *buf = malloc(n);
		memcpy(buf, e->r->body + e->sent, n);
		size_t written = e->write(buf, 1, n, e->write_data);
		free(buf);
		if (written != n) {
			finish(m, e, CURLE_WRITE_ERROR);
			return;
		}
	}
	e->sent += n;
	if (e->sent == e->r->size)
		finish(m, e, CURLE_OK);
}

CURLMcode curl_multi_perform(CURLM *multi, int *running)
{
	struct multi *m = multi;
	/* The handles of the moment: a write callback may remove one */
	struct easy *handles[MAX_HANDLES];
	int count = m->count;
	memcpy(handles, m->handles, sizeof(handles));
	for (int i = 0; i < count && !held; i++) {
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
		multi_detach(m, m->handles[0]);
	free(m);
	return CURLM_OK;
}
