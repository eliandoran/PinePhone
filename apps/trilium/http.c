/*
 * The sync's requests, blocking, on its thread: one libcurl handle for a sync, so its
 * connection is kept; the session's cookies (Trilium's express session, a relay's JWT)
 * taken from Set-Cookie and sent back as Trilium's request.ts does; JSON bodies; a
 * request given up after 120 s without data (Trilium's syncServerTimeout), not at a
 * fixed time: one answer can carry a large file.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "trilium.h"

#define TIMEOUT_S 120L

struct buffer {
	char *data;
	size_t size;
};

static size_t write_cb(char *data, size_t size, size_t n, void *user)
{
	struct buffer *b = user;
	size_t len = size * n;
	char *grown = realloc(b->data, b->size + len + 1);
	if (!grown)
		return 0;
	memcpy(grown + b->size, data, len);
	b->data = grown;
	b->size += len;
	b->data[b->size] = '\0';
	return len;
}

/* "Set-Cookie: name=value; Path=/..." kept as "name=value", replacing one of that name */
static void cookie_take(struct tr_http *h, const char *line, size_t len)
{
	static const char prefix[] = "set-cookie:";
	if (len < sizeof(prefix) || strncasecmp(line, prefix, sizeof(prefix) - 1))
		return;
	const char *p = line + sizeof(prefix) - 1;
	const char *end = line + len;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	size_t pair = strcspn(p, ";\r\n");
	if (p + pair > end)
		pair = (size_t)(end - p);
	const char *eq = memchr(p, '=', pair);
	if (!eq || eq == p)
		return;
	size_t name = (size_t)(eq - p);

	/* Without the one of that name */
	char kept[sizeof(h->cookies)] = "";
	size_t k = 0;
	for (const char *c = h->cookies; *c;) {
		size_t n = strcspn(c, ";");
		if (!(n > name && !strncmp(c, p, name) && c[name] == '=') && k + n + 3 < sizeof(kept))
			k += (size_t)snprintf(kept + k, sizeof(kept) - k, "%s%.*s", k ? "; " : "", (int)n, c);
		c += n;
		while (*c == ';' || *c == ' ')
			c++;
	}
	if (k + pair + 3 < sizeof(kept))
		snprintf(kept + k, sizeof(kept) - k, "%s%.*s", k ? "; " : "", (int)pair, p);
	snprintf(h->cookies, sizeof(h->cookies), "%s", kept);
}

static size_t header_cb(char *data, size_t size, size_t n, void *user)
{
	cookie_take(user, data, size * n);
	return size * n;
}

bool tr_http_init(struct tr_http *h, const char *host)
{
	memset(h, 0, sizeof(*h));
	snprintf(h->host, sizeof(h->host), "%s", host);
	h->curl = curl_easy_init();
	return h->curl != NULL;
}

void tr_http_free(struct tr_http *h)
{
	if (h->curl)
		curl_easy_cleanup(h->curl);
	h->curl = NULL;
}

long tr_http_request(struct tr_http *h, const char *method, const char *path, const char *body, char **out)
{
	*out = NULL;
	char url[sizeof(h->host) + 512];
	snprintf(url, sizeof(url), "%s%s", h->host, path);
	struct buffer b = {0};
	CURL *c = h->curl;
	curl_easy_reset(c);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PinePhone Trilium"); /* NON-NLS */
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, h);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, TIMEOUT_S);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

	/* As Trilium's request.ts: one page of JSON */
	char cookie[sizeof(h->cookies) + 16];
	snprintf(cookie, sizeof(cookie), "Cookie: %s", h->cookies); /* NON-NLS */
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json"); /* NON-NLS */
	headers = curl_slist_append(headers, "pageCount: 1"); /* NON-NLS */
	headers = curl_slist_append(headers, "pageIndex: 0"); /* NON-NLS */
	headers = curl_slist_append(headers, "requestId: n/a"); /* NON-NLS */
	if (h->cookies[0])
		headers = curl_slist_append(headers, cookie);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	if (body || strcmp(method, "GET")) {
		curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body ? body : "");
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	}

	long status = 0;
	CURLcode res = curl_easy_perform(c);
	if (res == CURLE_OK)
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	else
		fprintf(stderr, "trilium: %s %s: %s\n", method, path, curl_easy_strerror(res));
	curl_slist_free_all(headers);
	if (status)
		*out = b.data ? b.data : strdup("");
	else
		free(b.data);
	return status;
}
