/*
 * The account's IMAP server, through libetpan (email.h): its calls block, so they run
 * on a thread of their own, the jobs queued by the UI and done in order, one session
 * (mailimap) kept logged in across them and made again when it broke; each job's
 * result goes back to the UI thread through a list an lv_timer drains. TLS is
 * verified here, with OpenSSL's store and the host's name, as libetpan verifies
 * nothing by itself; plain IMAP goes to the loopback address only (the tests' server).
 */

#define _GNU_SOURCE /* timegm */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#include <libetpan/libetpan.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "email.h"

#define TIMEOUT_S 30 /* on the network */
#define DRAIN_MS 50  /* results looked for by the UI */

/* Milliseconds, for the log's timings of each phase */
static long ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int hex_value(char ch)
{
	return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

enum kind { CHECK_LOGIN, LIST_FOLDERS, LIST_INBOX, FETCH_BODY, MARK_SEEN, DISCONNECT };

struct job {
	enum kind kind;
	struct account account; /* copied when queued: the UI's may change meanwhile */
	char folder[128]; /* the job's, on the wire */
	unsigned long uid; /* the message, or LIST_INBOX: the newest of those listed is before it (0 the inbox's last) */
	unsigned max;
	void *user;
	bool cancelled;
	/* The result */
	int status;
	struct mail *mails;
	struct folder *folders;
	unsigned count;
	unsigned long uidvalidity; /* LIST_INBOX: the folder's */
	char *text;
	bool html;
	union {
		mail_done_fn done;
		mail_listed_fn listed;
		mail_folders_fn folders;
		mail_body_fn body;
	} cb;
	struct job *next;
};

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool started;
	struct job *queue, *running, *done; /* waiting (in order), on its way, finished */
	lv_timer_t *timer;                  /* the UI's, draining `done` */
	/* The worker's alone */
	mailimap *imap;
	char key[sizeof(account.imap) + sizeof(account.user) + 1]; /* whose session */
	char mailbox[128];                                           /* selected, "" none */
	bool broken;                                                 /* an answer not parsed: closed after the job */
} cl = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

/* URLs */

bool host_is_loopback(const char *host)
{
	return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 || strcmp(host, "::1") == 0;
}

/* scheme://host[:port]/...: false if not one */
static bool url_parse(const char *url, char *scheme, size_t scheme_size, char *host, size_t host_size, uint16_t *port)
{
	const char *p = strstr(url, "://");
	if (!p)
		return false;
	snprintf(scheme, scheme_size, "%.*s", (int)(p - url), url);
	p += 3;
	size_t n = strcspn(p, ":/");
	snprintf(host, host_size, "%.*s", (int)n, p);
	*port = 0;
	if (p[n] == ':')
		*port = (uint16_t)strtoul(p + n + 1, NULL, 10);
	return host[0] != '\0';
}

/* TLS */

/* Before the handshake: the server's name sent (SNI) and its certificate verified
 * against the system's store and that name, which libetpan leaves to the program */
static void ssl_setup(struct mailstream_ssl_context *ctx, void *data)
{
	const char *host = data;
	mailstream_ssl_set_server_name(ctx, (char *)host); /* not written to */
	SSL_CTX *c = mailstream_ssl_get_openssl_ssl_ctx(ctx);
	if (!c)
		return;
	SSL_CTX_set_default_verify_paths(c);
	X509_VERIFY_PARAM_set1_host(SSL_CTX_get0_param(c), host, 0);
	SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
}

/* The session */

static void session_close(void)
{
	if (cl.imap) {
		mailimap_logout(cl.imap);
		mailimap_free(cl.imap);
	}
	cl.imap = NULL;
	cl.mailbox[0] = '\0';
	cl.broken = false;
	cl.key[0] = '\0';
}

/* Connected and logged in as `a`, or why not; *out the session */
static int session_open(const struct account *a, mailimap **out)
{
	char scheme[16], host[128];
	uint16_t port;
	*out = NULL;
	if (!url_parse(a->imap, scheme, sizeof(scheme), host, sizeof(host), &port))
		return MAIL_FAILED;
	bool tls = strcasecmp(scheme, "imaps") == 0;
	if (!tls && !(strcasecmp(scheme, "imap") == 0 && host_is_loopback(host))) {
		printf("email: %s: only imaps:// is spoken\n", a->imap);
		return MAIL_UNREACHABLE; /* never in clear */
	}
	if (!port)
		port = tls ? 993 : 143;
	mailimap *imap = mailimap_new(0, NULL);
	if (!imap)
		return MAIL_FAILED;
	mailimap_set_timeout(imap, TIMEOUT_S);
	long t0 = ms_now();
	int r = tls ? mailimap_ssl_connect_with_callback(imap, host, port, ssl_setup, host) : mailimap_socket_connect(imap, host, port);
	if (r != MAILIMAP_NO_ERROR && r != MAILIMAP_NO_ERROR_AUTHENTICATED && r != MAILIMAP_NO_ERROR_NON_AUTHENTICATED) {
		printf("email: %s: connecting failed (libetpan %d)\n", a->imap, r);
		mailimap_free(imap);
		return r == MAILIMAP_ERROR_SSL ? MAIL_CERTIFICATE : MAIL_UNREACHABLE;
	}
	long t1 = ms_now();
	if (r != MAILIMAP_NO_ERROR_AUTHENTICATED) { /* PREAUTH needs none */
		r = mailimap_login(imap, a->user, a->password);
		if (r != MAILIMAP_NO_ERROR) {
			printf("email: %s: login failed (libetpan %d)\n", a->imap, r);
			/* Refused with a word from the server; a connection dropped meanwhile gives the
			 * same code, without one */
			bool refused = r == MAILIMAP_ERROR_LOGIN && imap->imap_response && imap->imap_response[0];
			mailimap_logout(imap);
			mailimap_free(imap);
			return refused ? MAIL_LOGIN_DENIED : MAIL_UNREACHABLE;
		}
	}
	printf("email: %s: connected in %ld ms, logged in in %ld ms\n", a->imap, t1 - t0, ms_now() - t1);
	*out = imap;
	return MAIL_OK;
}

/* The worker's session, as the job's account */
static int session_ensure(const struct job *j)
{
	char key[sizeof(cl.key)];
	snprintf(key, sizeof(key), "%s%s", j->account.imap, j->account.user);
	if (cl.imap && strcmp(key, cl.key) == 0)
		return MAIL_OK;
	session_close();
	int status = session_open(&j->account, &cl.imap);
	if (status == MAIL_OK)
		snprintf(cl.key, sizeof(cl.key), "%s", key);
	return status;
}

static int select_mailbox(const char *name)
{
	if (strcmp(cl.mailbox, name) == 0)
		return MAIL_OK;
	int r = mailimap_select(cl.imap, name);
	if (r != MAILIMAP_NO_ERROR) {
		printf("email: SELECT %s failed (libetpan %d)\n", name, r);
		cl.mailbox[0] = '\0';
		return r == MAILIMAP_ERROR_STREAM ? MAIL_UNREACHABLE : MAIL_FAILED;
	}
	snprintf(cl.mailbox, sizeof(cl.mailbox), "%s", name);
	return MAIL_OK;
}

/* A parse error is what libetpan makes of a connection dropped in the middle of an
 * answer too: whether the server is still there (its socket not closed), the session
 * dropped after the job either way, its stream left mid-answer */
static bool session_alive(void)
{
	cl.broken = true;
	if (!cl.imap || !cl.imap->imap_stream || !cl.imap->imap_stream->low)
		return false;
	int fd = mailstream_low_get_fd(cl.imap->imap_stream->low);
	if (fd < 0)
		return false;
	char byte;
	ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	return n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

/* What libetpan hands back, as a status */
static int status_of(int r)
{
	switch (r) {
	case MAILIMAP_NO_ERROR:
		return MAIL_OK;
	case MAILIMAP_ERROR_STREAM:
	case MAILIMAP_ERROR_CONNECTION_REFUSED:
		return MAIL_UNREACHABLE;
	case MAILIMAP_ERROR_PARSE:
		return session_alive() ? MAIL_GARBLED : MAIL_UNREACHABLE;
	default:
		return MAIL_FAILED;
	}
}

/* Messages */

/* A header's text (RFC 2047 encoded words, folded lines) as one line of UTF-8; raw 8-bit
 * taken as UTF-8, what mail is today; a word's language tag (RFC 2231, =?UTF-8*ro?Q?...)
 * dropped, as libetpan does not know it */
static void header_text(const char *in, char *out, size_t size)
{
	out[0] = '\0';
	if (!in)
		return;
	char *copy = strdup(in);
	if (!copy)
		return;
	for (char *w = strstr(copy, "=?"); w; w = strstr(w + 2, "=?")) {
		char *q = strchr(w + 2, '?'), *star = memchr(w + 2, '*', q ? (size_t)(q - w - 2) : 0);
		if (q && star)
			memmove(star, q, strlen(q) + 1);
	}
	/* Decoded only with an encoded word in it: libetpan makes something of "=??" too */
	bool encoded = false;
	for (const char *w = strstr(copy, "=?"); w && !encoded; w = strstr(w + 2, "=?")) {
		const char *q = strchr(w + 2, '?');
		encoded = q && q > w + 2 && strchr("BbQq", q[1]) && q[1] && q[2] == '?' && strstr(q + 3, "?="); /* NON-NLS */
	}
	size_t idx = 0;
	char *decoded = NULL;
	if (encoded && mailmime_encoded_phrase_parse("UTF-8", copy, strlen(copy), &idx, "UTF-8", &decoded) == MAILIMF_NO_ERROR &&
	    decoded) {
		snprintf(out, size, "%s", decoded);
		free(decoded);
	} else {
		snprintf(out, size, "%s", copy);
	}
	free(copy);
	/* Cut between characters, not inside one: a last one left short goes */
	size_t n = strlen(out);
	if (n >= size - 1) {
		size_t start = n;
		while (start > 0 && ((unsigned char)out[start - 1] & 0xC0) == 0x80)
			start--;
		if (start > 0)
			start--;
		unsigned char lead = (unsigned char)out[start];
		size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
		if (n - start < want)
			n = start;
	}
	out[n] = '\0';
	for (char *p = out; *p; p++)
		if (*p == '\r' || *p == '\n' || *p == '\t')
			*p = ' ';
}

static time_t date_of(const struct mailimap_date_time *d)
{
	struct tm tm = { .tm_mday = d->dt_day, .tm_mon = d->dt_month - 1, .tm_year = d->dt_year - 1900,
	                 .tm_hour = d->dt_hour, .tm_min = d->dt_min, .tm_sec = d->dt_sec };
	time_t t = timegm(&tm);
	int zone = d->dt_zone; /* +0130 as 130 */
	return t - ((zone / 100) * 3600 + (zone % 100) * 60);
}

static void envelope_of(const struct mailimap_envelope *env, struct mail *m)
{
	header_text(env->env_subject, m->subject, sizeof(m->subject));
	const struct mailimap_address *from = NULL;
	if (env->env_from && env->env_from->frm_list && clist_begin(env->env_from->frm_list))
		from = clist_content(clist_begin(env->env_from->frm_list));
	if (!from)
		return;
	snprintf(m->address, sizeof(m->address), "%s@%s", from->ad_mailbox_name ? from->ad_mailbox_name : "",
	         from->ad_host_name ? from->ad_host_name : "");
	header_text(from->ad_personal_name, m->from, sizeof(m->from));
	if (!m->from[0])
		snprintf(m->from, sizeof(m->from), "%s", m->address);
}

/* A FETCH response's message */
static void mail_of(const struct mailimap_msg_att *att, struct mail *m)
{
	memset(m, 0, sizeof(*m));
	for (clistiter *it = clist_begin(att->att_list); it; it = clist_next(it)) {
		const struct mailimap_msg_att_item *item = clist_content(it);
		if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC) {
			const struct mailimap_msg_att_dynamic *dyn = item->att_data.att_dyn;
			if (!dyn || !dyn->att_list)
				continue;
			for (clistiter *f = clist_begin(dyn->att_list); f; f = clist_next(f)) {
				const struct mailimap_flag_fetch *ff = clist_content(f);
				if (ff->fl_type == MAILIMAP_FLAG_FETCH_OTHER && ff->fl_flag && ff->fl_flag->fl_type == MAILIMAP_FLAG_SEEN)
					m->seen = true;
			}
		} else if (item->att_type == MAILIMAP_MSG_ATT_ITEM_STATIC) {
			const struct mailimap_msg_att_static *s = item->att_data.att_static;
			if (s->att_type == MAILIMAP_MSG_ATT_UID)
				m->uid = s->att_data.att_uid;
			else if (s->att_type == MAILIMAP_MSG_ATT_INTERNALDATE && s->att_data.att_internal_date)
				m->date = date_of(s->att_data.att_internal_date);
			else if (s->att_type == MAILIMAP_MSG_ATT_ENVELOPE && s->att_data.att_env)
				envelope_of(s->att_data.att_env, m);
		}
	}
}

static int do_list(struct job *j)
{
	cl.mailbox[0] = '\0'; /* selected again: what arrived since, in EXISTS */
	long t0 = ms_now();
	int status = select_mailbox(j->folder);
	if (status != MAIL_OK)
		return status;
	long t1 = ms_now();
	uint32_t exists = cl.imap->imap_selection_info ? cl.imap->imap_selection_info->sel_exists : 0;
	j->uidvalidity = cl.imap->imap_selection_info ? cl.imap->imap_selection_info->sel_uidvalidity : 0;
	j->count = 0;
	uint32_t last = exists;
	if (j->uid == 1) {
		last = 0; /* nothing before the first UID */
	} else if (j->uid) {
		/* Those older than that message: UIDs grow with the sequence numbers, so they
		 * are the first ones, as many as the server finds with a smaller UID */
		struct mailimap_set *below = mailimap_set_new_interval(1, (uint32_t)(j->uid - 1));
		struct mailimap_search_key *key = mailimap_search_key_new_uid(below);
		clist *found = NULL;
		int r = mailimap_search(cl.imap, NULL, key, &found);
		mailimap_search_key_free(key); /* the set with it */
		if (r != MAILIMAP_NO_ERROR) {
			printf("email: SEARCH failed (libetpan %d)\n", r);
			return status_of(r);
		}
		last = (uint32_t)clist_count(found);
		mailimap_search_result_free(found);
	}
	if (!last)
		return MAIL_OK;
	uint32_t first = last > j->max ? last - j->max + 1 : 1;
	struct mailimap_set *set = mailimap_set_new_interval(first, last);
	struct mailimap_fetch_type *type = mailimap_fetch_type_new_fetch_att_list_empty();
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_uid());
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_flags());
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_internaldate());
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_envelope());
	clist *list = NULL;
	int r = mailimap_fetch(cl.imap, set, type, &list);
	mailimap_fetch_type_free(type);
	mailimap_set_free(set);
	printf("email: SELECT %ld ms, FETCH %ld ms\n", t1 - t0, ms_now() - t1);
	if (r != MAILIMAP_NO_ERROR) {
		printf("email: FETCH failed (libetpan %d)\n", r);
		return status_of(r);
	}
	/* The server lists them oldest first */
	unsigned n = (unsigned)clist_count(list);
	if (n > j->max)
		n = j->max;
	j->mails = calloc(n ? n : 1, sizeof(*j->mails));
	unsigned i = 0;
	for (clistiter *it = clist_begin(list); it && i < n; it = clist_next(it), i++)
		mail_of(clist_content(it), &j->mails[n - 1 - i]);
	j->count = n;
	mailimap_fetch_list_free(list);
	return MAIL_OK;
}

/* A text part of a BODYSTRUCTURE: where it is, how it is encoded */
struct part {
	uint32_t section[8];
	int depth;
	int encoding; /* MAILIMAP_BODY_FLD_ENC_* */
	char charset[40];
	bool found;
};

/* The first text/html and text/plain parts, however nested */
static void parts_find(const struct mailimap_body *body, const uint32_t *path, int depth, struct part *html,
                       struct part *plain)
{
	if (body->bd_type == MAILIMAP_BODY_MPART) {
		const struct mailimap_body_type_mpart *mp = body->bd_data.bd_body_mpart;
		uint32_t child[8];
		if (depth)
			memcpy(child, path, (size_t)depth * sizeof(*path));
		uint32_t i = 1;
		for (clistiter *it = mp && mp->bd_list ? clist_begin(mp->bd_list) : NULL; it && depth < 7; it = clist_next(it), i++) {
			child[depth] = i;
			parts_find(clist_content(it), child, depth + 1, html, plain);
		}
		return;
	}
	const struct mailimap_body_type_1part *p1 = body->bd_data.bd_body_1part;
	if (!p1 || p1->bd_type != MAILIMAP_BODY_TYPE_1PART_TEXT || !p1->bd_data.bd_type_text)
		return;
	const struct mailimap_body_type_text *text = p1->bd_data.bd_type_text;
	struct part *part = strcasecmp(text->bd_media_text, "html") == 0 ? html : strcasecmp(text->bd_media_text, "plain") == 0 ? plain : NULL;
	if (!part || part->found)
		return;
	part->found = true;
	/* A message of one part: its section is 1 */
	if (depth) {
		memcpy(part->section, path, (size_t)depth * sizeof(*path));
		part->depth = depth;
	} else {
		part->section[0] = 1;
		part->depth = 1;
	}
	const struct mailimap_body_fields *fields = text->bd_fields;
	part->encoding = fields && fields->bd_encoding ? fields->bd_encoding->enc_type : MAILIMAP_BODY_FLD_ENC_7BIT;
	part->charset[0] = '\0';
	if (fields && fields->bd_parameter && fields->bd_parameter->pa_list)
		for (clistiter *it = clist_begin(fields->bd_parameter->pa_list); it; it = clist_next(it)) {
			const struct mailimap_single_body_fld_param *p = clist_content(it);
			if (strcasecmp(p->pa_name, "charset") == 0)
				snprintf(part->charset, sizeof(part->charset), "%s", p->pa_value);
		}
}

/* Quoted-printable, as mail has it: an escape that is not one kept as written, a soft
 * line break (= ending the line, spaces after it too) joined; libetpan drops the first */
static size_t qp_decode(const char *in, size_t len, unsigned char *out)
{
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '=') {
			int hi = i + 1 < len ? hex_value(in[i + 1]) : -1, lo = i + 2 < len ? hex_value(in[i + 2]) : -1;
			if (hi >= 0 && lo >= 0) {
				out[n++] = (unsigned char)(hi * 16 + lo);
				i += 2;
				continue;
			}
			size_t j = i + 1;
			while (j < len && (in[j] == ' ' || in[j] == '\t'))
				j++;
			if (j < len && in[j] == '\r')
				j++;
			if (j < len && in[j] == '\n') {
				i = j;
				continue;
			}
		}
		out[n++] = (unsigned char)in[i];
	}
	return n;
}

/* The part's bytes as the message carries them, decoded to UTF-8 (malloc'd) */
static char *part_decode(const struct part *part, const char *data, size_t len)
{
	size_t idx = 0, decoded_len = 0;
	char *decoded = NULL;
	bool own = part->encoding == MAILIMAP_BODY_FLD_ENC_QUOTED_PRINTABLE;
	if (own) {
		decoded = malloc(len + 1);
		if (!decoded)
			return NULL;
		decoded_len = qp_decode(data, len, (unsigned char *)decoded);
	} else {
		int mechanism = part->encoding == MAILIMAP_BODY_FLD_ENC_BASE64 ? MAILMIME_MECHANISM_BASE64 : MAILMIME_MECHANISM_8BIT;
		if (mailmime_part_parse(data, len, &idx, mechanism, &decoded, &decoded_len) != MAILIMF_NO_ERROR || !decoded)
			return NULL;
	}
	const char *bytes = decoded;
	size_t n = decoded_len;
	char *converted = NULL;
	size_t converted_len = 0;
	if (part->charset[0] && strcasecmp(part->charset, "utf-8") != 0 &&
	    charconv_buffer("UTF-8", part->charset, decoded, decoded_len, &converted, &converted_len) == MAIL_CHARCONV_NO_ERROR &&
	    converted) {
		bytes = converted;
		n = converted_len;
	}
	char *text = malloc(n + 1);
	if (text) {
		memcpy(text, bytes, n);
		text[n] = '\0';
		for (size_t i = 0; i < n; i++) /* no NULs inside: a label stops there */
			if (!text[i])
				text[i] = ' ';
	}
	if (converted)
		charconv_buffer_free(converted);
	if (own)
		free(decoded);
	else
		mmap_string_unref(decoded);
	return text;
}

static int do_body(struct job *j)
{
	int status = select_mailbox(j->folder);
	if (status != MAIL_OK)
		return status;
	struct mailimap_set *set = mailimap_set_new_single((uint32_t)j->uid);
	struct mailimap_fetch_type *type = mailimap_fetch_type_new_fetch_att_list_empty();
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_bodystructure());
	clist *list = NULL;
	int r = mailimap_uid_fetch(cl.imap, set, type, &list);
	mailimap_fetch_type_free(type);
	if (r != MAILIMAP_NO_ERROR) {
		printf("email: UID FETCH BODYSTRUCTURE failed (libetpan %d)\n", r);
		mailimap_set_free(set);
		return r == MAILIMAP_ERROR_PARSE && session_alive() ? MAIL_NO_TEXT : status_of(r); /* a structure not made sense of */
	}
	struct part html = {0}, plain = {0};
	for (clistiter *it = clist_begin(list); it; it = clist_next(it)) {
		const struct mailimap_msg_att *att = clist_content(it);
		for (clistiter *i = clist_begin(att->att_list); i; i = clist_next(i)) {
			const struct mailimap_msg_att_item *item = clist_content(i);
			if (item->att_type == MAILIMAP_MSG_ATT_ITEM_STATIC &&
			    item->att_data.att_static->att_type == MAILIMAP_MSG_ATT_BODYSTRUCTURE &&
			    item->att_data.att_static->att_data.att_bodystructure)
				parts_find(item->att_data.att_static->att_data.att_bodystructure, NULL, 0, &html, &plain);
		}
	}
	mailimap_fetch_list_free(list);
	/* The page when there is one, the text otherwise */
	const struct part *part = html.found ? &html : plain.found ? &plain : NULL;
	if (!part) {
		mailimap_set_free(set);
		return MAIL_NO_TEXT;
	}
	clist *ids = clist_new();
	for (int i = 0; i < part->depth; i++) {
		uint32_t *n = malloc(sizeof(*n));
		*n = part->section[i];
		clist_append(ids, n);
	}
	struct mailimap_section *section = mailimap_section_new_part(mailimap_section_part_new(ids));
	type = mailimap_fetch_type_new_fetch_att_list_empty();
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_body_peek_section(section));
	list = NULL;
	r = mailimap_uid_fetch(cl.imap, set, type, &list);
	mailimap_fetch_type_free(type); /* the section with it */
	mailimap_set_free(set);
	if (r != MAILIMAP_NO_ERROR) {
		printf("email: UID FETCH BODY failed (libetpan %d)\n", r);
		return r == MAILIMAP_ERROR_PARSE && session_alive() ? MAIL_NO_TEXT : status_of(r);
	}
	status = MAIL_NO_TEXT;
	for (clistiter *it = clist_begin(list); it && status != MAIL_OK; it = clist_next(it)) {
		const struct mailimap_msg_att *att = clist_content(it);
		for (clistiter *i = clist_begin(att->att_list); i; i = clist_next(i)) {
			const struct mailimap_msg_att_item *item = clist_content(i);
			if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC ||
			    item->att_data.att_static->att_type != MAILIMAP_MSG_ATT_BODY_SECTION)
				continue;
			const struct mailimap_msg_att_body_section *sec = item->att_data.att_static->att_data.att_body_section;
			if (!sec || !sec->sec_body_part)
				continue;
			j->text = part_decode(part, sec->sec_body_part, sec->sec_length);
			j->html = part == &html;
			if (j->text)
				status = MAIL_OK;
			break;
		}
	}
	mailimap_fetch_list_free(list);
	return status;
}

static int do_mark_seen(struct job *j)
{
	int status = select_mailbox(j->folder);
	if (status != MAIL_OK)
		return status;
	struct mailimap_set *set = mailimap_set_new_single((uint32_t)j->uid);
	struct mailimap_flag_list *flags = mailimap_flag_list_new_empty();
	mailimap_flag_list_add(flags, mailimap_flag_new_seen());
	struct mailimap_store_att_flags *att = mailimap_store_att_flags_new_add_flags(flags);
	int r = mailimap_uid_store(cl.imap, set, att);
	mailimap_store_att_flags_free(att);
	mailimap_set_free(set);
	if (r != MAILIMAP_NO_ERROR)
		printf("email: UID STORE failed (libetpan %d)\n", r);
	return status_of(r);
}

/* Folders */

/* A folder's role: LIST's special-use attribute (RFC 6154: \Sent, \Drafts, \Trash,
 * \Junk, \Archive, which Dovecot, Gmail and Outlook give), or its usual name (`last`:
 * the last part of its path, decoded) on a server without them */
static enum folder_role role_of(const struct mailimap_mailbox_list *mb, const char *last)
{
	/* NON-NLS-BEGIN: the servers' names */
	static const struct {
		const char *text;
		enum folder_role role;
	} attributes[] = { { "Sent", FOLDER_SENT }, { "Drafts", FOLDER_DRAFTS }, { "Trash", FOLDER_TRASH },
		           { "Junk", FOLDER_JUNK }, { "Archive", FOLDER_ARCHIVE } },
	  names[] = { { "Sent", FOLDER_SENT }, { "Sent Items", FOLDER_SENT }, { "Sent Mail", FOLDER_SENT },
		      { "Sent Messages", FOLDER_SENT }, { "Drafts", FOLDER_DRAFTS }, { "Trash", FOLDER_TRASH },
		      { "Deleted", FOLDER_TRASH }, { "Deleted Items", FOLDER_TRASH }, { "Deleted Messages", FOLDER_TRASH },
		      { "Junk", FOLDER_JUNK }, { "Junk E-mail", FOLDER_JUNK }, { "Junk Email", FOLDER_JUNK },
		      { "Spam", FOLDER_JUNK }, { "Archive", FOLDER_ARCHIVE }, { "Archives", FOLDER_ARCHIVE } };
	/* NON-NLS-END */
	if (strcasecmp(mb->mb_name, "INBOX") == 0)
		return FOLDER_INBOX;
	if (mb->mb_flag && mb->mb_flag->mbf_oflags)
		for (clistiter *it = clist_begin(mb->mb_flag->mbf_oflags); it; it = clist_next(it)) {
			const struct mailimap_mbx_list_oflag *of = clist_content(it);
			if (of->of_type != MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT || !of->of_flag_ext)
				continue;
			const char *text = of->of_flag_ext + (of->of_flag_ext[0] == '\\');
			for (size_t i = 0; i < sizeof(attributes) / sizeof(*attributes); i++)
				if (strcasecmp(text, attributes[i].text) == 0)
					return attributes[i].role;
		}
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (strcasecmp(last, names[i].text) == 0)
			return names[i].role;
	return FOLDER_OTHER;
}

/* Whether it can hold messages (\Noselect, \NonExistent: it cannot) */
static bool selectable(const struct mailimap_mailbox_list *mb)
{
	if (!mb->mb_flag)
		return true;
	if (mb->mb_flag->mbf_sflag == MAILIMAP_MBX_LIST_SFLAG_NOSELECT)
		return false;
	if (mb->mb_flag->mbf_oflags)
		for (clistiter *it = clist_begin(mb->mb_flag->mbf_oflags); it; it = clist_next(it)) {
			const struct mailimap_mbx_list_oflag *of = clist_content(it);
			if (of->of_type == MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT && of->of_flag_ext &&
			    strcasecmp(of->of_flag_ext + (of->of_flag_ext[0] == '\\'), "NonExistent") == 0)
				return false;
		}
	return true;
}

/* The inbox first, the special ones by role, the rest by name */
static int folder_order(const void *a, const void *b)
{
	static const int rank[] = { [FOLDER_INBOX] = 0, [FOLDER_SENT] = 1,  [FOLDER_DRAFTS] = 2, [FOLDER_TRASH] = 3,
		                    [FOLDER_JUNK] = 4,  [FOLDER_ARCHIVE] = 5, [FOLDER_OTHER] = 6 };
	const struct folder *fa = a, *fb = b;
	if (rank[fa->role] != rank[fb->role])
		return rank[fa->role] - rank[fb->role];
	return strcasecmp(fa->title, fb->title);
}

static int do_folders(struct job *j)
{
	clist *list = NULL;
	int r = mailimap_list(cl.imap, "", "*", &list);
	if (r != MAILIMAP_NO_ERROR) {
		printf("email: LIST failed (libetpan %d)\n", r);
		return status_of(r);
	}
	unsigned n = (unsigned)clist_count(list);
	j->folders = calloc(n ? n : 1, sizeof(*j->folders));
	j->count = 0;
	for (clistiter *it = clist_begin(list); it; it = clist_next(it)) {
		const struct mailimap_mailbox_list *mb = clist_content(it);
		if (!mb->mb_name || !selectable(mb))
			continue;
		struct folder *f = &j->folders[j->count];
		snprintf(f->name, sizeof(f->name), "%s", mb->mb_name);
		/* The name as the user reads it: decoded, " / " between a path's parts */
		char *decoded = charconv_decode_mutf7(mb->mb_name);
		const char *text = decoded ? decoded : mb->mb_name, *last = text;
		size_t o = 0;
		for (const char *p = text; *p && o + 4 < sizeof(f->title); p++) {
			if (mb->mb_delimiter && *p == mb->mb_delimiter) {
				o += (size_t)snprintf(f->title + o, sizeof(f->title) - o, " / ");
				last = p + 1;
			} else {
				f->title[o++] = *p;
			}
		}
		f->title[o] = '\0';
		f->role = role_of(mb, last);
		free(decoded);
		j->count++;
	}
	mailimap_list_result_free(list);
	/* How many unread in each; a folder STATUS fails on left at 0 */
	for (unsigned i = 0; i < j->count; i++) {
		struct mailimap_status_att_list *atts = mailimap_status_att_list_new_empty();
		mailimap_status_att_list_add(atts, MAILIMAP_STATUS_ATT_UNSEEN);
		struct mailimap_mailbox_data_status *st = NULL;
		r = mailimap_status(cl.imap, j->folders[i].name, atts, &st);
		if (r == MAILIMAP_NO_ERROR && st && st->st_info_list)
			for (clistiter *it = clist_begin(st->st_info_list); it; it = clist_next(it)) {
				const struct mailimap_status_info *info = clist_content(it);
				if (info->st_att == MAILIMAP_STATUS_ATT_UNSEEN)
					j->folders[i].unseen = info->st_value;
			}
		if (st)
			mailimap_mailbox_data_status_free(st);
		mailimap_status_att_list_free(atts);
		if (r == MAILIMAP_ERROR_STREAM || r == MAILIMAP_ERROR_PARSE)
			return status_of(r);
	}
	qsort(j->folders, j->count, sizeof(*j->folders), folder_order);
	return MAIL_OK;
}

/* The worker */

static int run(struct job *j)
{
	if (j->kind == CHECK_LOGIN) {
		mailimap *imap;
		int status = session_open(&j->account, &imap);
		if (imap) {
			mailimap_logout(imap);
			mailimap_free(imap);
		}
		return status;
	}
	if (j->kind == DISCONNECT) {
		session_close();
		return MAIL_OK;
	}
	/* Once more with a new session when the old one went */
	static const char *const names[] = { "login", "folders", "list", "body", "mark seen", "disconnect" };
	for (int attempt = 0; attempt < 2; attempt++) {
		long t0 = ms_now();
		int status = session_ensure(j);
		if (status != MAIL_OK)
			return status;
		long t1 = ms_now();
		status = j->kind == LIST_FOLDERS ? do_folders(j)
		         : j->kind == LIST_INBOX  ? do_list(j)
		         : j->kind == FETCH_BODY  ? do_body(j)
		                                  : do_mark_seen(j);
		printf("email: %s: %ld ms (the session %ld ms before it)\n", names[j->kind], ms_now() - t1, t1 - t0);
		if (status != MAIL_UNREACHABLE) {
			if (cl.broken)
				session_close();
			return status;
		}
		session_close();
	}
	return MAIL_UNREACHABLE;
}

static void *worker(void *unused)
{
	LV_UNUSED(unused);
	pthread_mutex_lock(&cl.lock);
	for (;;) {
		while (!cl.queue)
			pthread_cond_wait(&cl.cond, &cl.lock);
		struct job *j = cl.queue;
		cl.queue = j->next;
		j->next = NULL;
		cl.running = j;
		pthread_mutex_unlock(&cl.lock);
		j->status = run(j);
		pthread_mutex_lock(&cl.lock);
		cl.running = NULL;
		/* Finished ones in order */
		struct job **tail = &cl.done;
		while (*tail)
			tail = &(*tail)->next;
		*tail = j;
	}
	return NULL;
}

static void job_free(struct job *j)
{
	free(j->mails);
	free(j->folders);
	free(j->text);
	free(j);
}

/* On the UI thread: each finished job's callback, unless its screen is gone */
static void drain(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	pthread_mutex_lock(&cl.lock);
	struct job *done = cl.done;
	cl.done = NULL;
	pthread_mutex_unlock(&cl.lock);
	while (done) {
		struct job *j = done;
		done = j->next;
		if (!j->cancelled) {
			switch (j->kind) {
			case CHECK_LOGIN:
			case MARK_SEEN:
				if (j->cb.done)
					j->cb.done(j->status, j->user);
				break;
			case LIST_FOLDERS:
				if (j->cb.folders)
					j->cb.folders(j->status, j->folders, j->count, j->user);
				break;
			case LIST_INBOX:
				if (j->cb.listed)
					j->cb.listed(j->status, j->mails, j->count, j->uidvalidity, j->user);
				break;
			case FETCH_BODY:
				if (j->cb.body)
					j->cb.body(j->status, j->text, j->html, j->user);
				break;
			case DISCONNECT:
				break;
			}
		}
		job_free(j);
	}
}

static void enqueue(struct job *j)
{
	if (!cl.timer)
		cl.timer = lv_timer_create(drain, DRAIN_MS, NULL);
	pthread_mutex_lock(&cl.lock);
	if (!cl.started) {
		signal(SIGPIPE, SIG_IGN); /* a server gone: libetpan writes to it, told by the error */
		cl.started = pthread_create(&cl.thread, NULL, worker, NULL) == 0;
		if (!cl.started)
			printf("email: no worker thread\n");
	}
	struct job **tail = &cl.queue;
	while (*tail)
		tail = &(*tail)->next;
	*tail = j;
	pthread_cond_signal(&cl.cond);
	pthread_mutex_unlock(&cl.lock);
}

static struct job *job_new(enum kind kind, void *user)
{
	struct job *j = calloc(1, sizeof(*j));
	if (!j)
		return NULL;
	j->kind = kind;
	j->account = account;
	j->user = user;
	return j;
}

void mail_check_login(const struct account *a, mail_done_fn done, void *user)
{
	struct job *j = job_new(CHECK_LOGIN, user);
	if (!j)
		return;
	j->account = *a;
	j->cb.done = done;
	enqueue(j);
}

void mail_list_folders(mail_folders_fn done, void *user)
{
	struct job *j = job_new(LIST_FOLDERS, user);
	if (!j)
		return;
	j->cb.folders = done;
	enqueue(j);
}

void mail_list(const char *folder, unsigned max, mail_listed_fn done, void *user)
{
	struct job *j = job_new(LIST_INBOX, user);
	if (!j)
		return;
	snprintf(j->folder, sizeof(j->folder), "%s", folder);
	j->max = max;
	j->cb.listed = done;
	enqueue(j);
}

void mail_list_older(const char *folder, unsigned long before, unsigned max, mail_listed_fn done, void *user)
{
	struct job *j = job_new(LIST_INBOX, user);
	if (!j)
		return;
	snprintf(j->folder, sizeof(j->folder), "%s", folder);
	j->uid = before;
	j->max = max;
	j->cb.listed = done;
	enqueue(j);
}

void mail_fetch_body(const char *folder, unsigned long uid, mail_body_fn done, void *user)
{
	struct job *j = job_new(FETCH_BODY, user);
	if (!j)
		return;
	snprintf(j->folder, sizeof(j->folder), "%s", folder);
	j->uid = uid;
	j->cb.body = done;
	enqueue(j);
}

void mail_mark_seen(const char *folder, unsigned long uid, mail_done_fn done, void *user)
{
	struct job *j = job_new(MARK_SEEN, user);
	if (!j)
		return;
	snprintf(j->folder, sizeof(j->folder), "%s", folder);
	j->uid = uid;
	j->cb.done = done;
	enqueue(j);
}

void mail_cancel(void *user)
{
	pthread_mutex_lock(&cl.lock);
	for (struct job *j = cl.queue; j; j = j->next)
		if (j->user == user)
			j->cancelled = true;
	if (cl.running && cl.running->user == user)
		cl.running->cancelled = true;
	for (struct job *j = cl.done; j; j = j->next)
		if (j->user == user)
			j->cancelled = true;
	pthread_mutex_unlock(&cl.lock);
}

void mail_disconnect(void)
{
	struct job *j = job_new(DISCONNECT, NULL);
	if (j)
		enqueue(j);
}

void mail_error_text(int status, const char *host, char *out, size_t size)
{
	const char *reason = status == MAIL_LOGIN_DENIED ? _("%s refused the password.") :
	                     status == MAIL_CERTIFICATE ? _("The certificate of %s cannot be trusted.") :
	                     status == MAIL_FAILED ? _("%s refused the request.") :
	                     status == MAIL_GARBLED ? _("%s did not answer as a mail server does.") :
	                     _("Cannot reach %s.");
	snprintf(out, size, reason, host);
}
