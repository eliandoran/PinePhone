/*
 * An IMAP session (email.h) on a TLS socket of phone-ui's: a NOOP (the greeting may be
 * lost on the way, see imap_connect()), LOGIN, then commands one at a time, each tagged A001, A002... The bytes received
 * are cut into responses: a line up to CRLF, continued past a literal ({n} ending the
 * line: n bytes follow the CRLF, then the line goes on) until a line ends without one.
 * Untagged responses (* ...) go to the command on its way, the tagged one ends it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "email.h"

enum state { ST_CLOSED, ST_CONNECTING, ST_HELLO, ST_LOGIN, ST_READY, ST_BUSY };

#define HELLO_TAG "H000" /* the NOOP sent on connecting */
#define LOGIN_TAG "L000"

#define IN_MAX (4 << 20) /* bytes of a response: more is not IMAP */

static void send_text(struct imap_session *s, const char *text)
{
	ui_socket_send(&s->sock, text, strlen(text));
}

void imap_quote(char *out, size_t size, const char *str)
{
	size_t n = 0;
	if (size < 3)
		return;
	out[n++] = '"';
	for (; *str && n < size - 2; str++) {
		if ((*str == '"' || *str == '\\') && n < size - 3)
			out[n++] = '\\';
		out[n++] = *str;
	}
	out[n++] = '"';
	out[n] = '\0';
}

static void queue_drop(struct imap_session *s, unsigned i)
{
	free(s->queue[i].text);
	memmove(&s->queue[i], &s->queue[i + 1], (s->queued - i - 1) * sizeof(s->queue[0]));
	s->queued--;
}

void imap_close(struct imap_session *s)
{
	ui_socket_close(&s->sock);
	free(s->in);
	while (s->queued)
		queue_drop(s, 0);
	memset(s, 0, sizeof(*s));
}

/* The first queued command sent */
static void send_next(struct imap_session *s)
{
	if (s->state != ST_READY || !s->queued)
		return;
	snprintf(s->tag, sizeof(s->tag), "A%03u", ++s->tags % 1000);
	char *line = malloc(strlen(s->tag) + strlen(s->queue[0].text) + 4);
	sprintf(line, "%s %s\r\n", s->tag, s->queue[0].text);
	s->state = ST_BUSY;
	send_text(s, line);
	free(line);
}

void imap_command(struct imap_session *s, const char *text, imap_untagged_fn untagged, imap_done_fn done, void *user)
{
	if (s->queued == IMAP_QUEUE || s->state == ST_CLOSED) {
		if (done)
			done(IMAP_LOST, user);
		return;
	}
	struct imap_command *c = &s->queue[s->queued++];
	c->text = strdup(text);
	c->untagged = untagged;
	c->done = done;
	c->user = user;
	send_next(s);
}

/* The command on its way ended: its done() called, the next one sent */
static void command_done(struct imap_session *s, int result)
{
	struct imap_command c = s->queue[0];
	s->queue[0].text = NULL;
	queue_drop(s, 0);
	s->state = ST_READY;
	if (c.done)
		c.done(result, c.user);
	free(c.text);
	if (s->state == ST_READY) /* not closed by done() */
		send_next(s);
}

/* Everything on its way told it is over */
static void lost(struct imap_session *s, int status)
{
	enum state state = s->state;
	void (*ready)(int, void *) = s->ready;
	void *ready_user = s->ready_user;
	struct imap_command c = {0};
	if (state == ST_BUSY) {
		c = s->queue[0];
		s->queue[0].text = NULL;
		queue_drop(s, 0);
	}
	imap_close(s);
	if (state == ST_CONNECTING || state == ST_HELLO || state == ST_LOGIN)
		ready(status, ready_user);
	else if (c.done)
		c.done(IMAP_LOST, c.user);
	free(c.text);
}

static int result_of(const char *word, size_t len)
{
	if (len >= 2 && strncasecmp(word, "OK", 2) == 0 && (len == 2 || word[2] == ' '))
		return IMAP_OK;
	if (len >= 2 && strncasecmp(word, "NO", 2) == 0 && (len == 2 || word[2] == ' '))
		return IMAP_NO;
	return IMAP_BAD;
}

/* One whole response */
static void dispatch(struct imap_session *s, const char *resp, size_t len)
{
	if (len >= 2 && resp[0] == '*' && resp[1] == ' ') {
		if (s->state == ST_HELLO) { /* the greeting, when it arrives at all */
			if (strncasecmp(resp + 2, "BYE", 3) == 0)
				lost(s, -CURLE_RECV_ERROR);
			else if (strncasecmp(resp + 2, "PREAUTH", 7) == 0)
				s->preauth = true;
			return;
		}
		if (s->state == ST_BUSY && s->queue[0].untagged)
			s->queue[0].untagged(resp, len, s->queue[0].user);
		return;
	}
	if (len >= 2 && resp[0] == '+' && resp[1] == ' ')
		return; /* a continuation request: nothing of ours waits for one */
	/* Tagged */
	size_t tag_len = strcspn(resp, " ");
	if (tag_len >= len)
		return;
	const char *word = resp + tag_len + 1;
	int result = result_of(word, len - tag_len - 1);
	if (s->state == ST_HELLO && tag_len == 4 && strncmp(resp, HELLO_TAG, 4) == 0) {
		if (s->preauth) {
			queue_drop(s, 0);
			queue_drop(s, 0);
			s->state = ST_READY;
			s->ready(0, s->ready_user);
			if (s->state == ST_READY)
				send_next(s);
			return;
		}
		/* Logging in, the password quoted (so no 8-bit characters: a literal would need
		 * the server's go-ahead) */
		char user[2 * sizeof(account.user) + 3], password[2 * sizeof(account.password) + 3];
		imap_quote(user, sizeof(user), s->queue[0].text); /* kept there until now */
		imap_quote(password, sizeof(password), s->queue[1].text);
		queue_drop(s, 0);
		queue_drop(s, 0);
		char *line = malloc(strlen(user) + strlen(password) + 16);
		sprintf(line, LOGIN_TAG " LOGIN %s %s\r\n", user, password);
		s->state = ST_LOGIN;
		send_text(s, line);
		free(line);
		return;
	}
	if (s->state == ST_LOGIN && tag_len == 4 && strncmp(resp, LOGIN_TAG, 4) == 0) {
		if (result != IMAP_OK) {
			lost(s, result == IMAP_NO ? -CURLE_LOGIN_DENIED : -CURLE_RECV_ERROR);
			return;
		}
		s->state = ST_READY;
		s->ready(0, s->ready_user);
		if (s->state == ST_READY)
			send_next(s);
		return;
	}
	if (s->state == ST_BUSY && tag_len == strlen(s->tag) && strncmp(resp, s->tag, tag_len) == 0)
		command_done(s, result);
}

/* The bytes of a whole response at the start of `in`, 0 if not all there yet */
static size_t response_len(const char *in, size_t len)
{
	size_t at = 0;
	for (;;) {
		const char *nl = memchr(in + at, '\n', len - at);
		if (!nl)
			return 0;
		size_t line_end = (size_t)(nl - in) + 1; /* after the LF */
		/* A literal's size right before the CRLF: {n} */
		size_t end = line_end - 1;
		if (end && in[end - 1] == '\r')
			end--;
		if (end && in[end - 1] == '}') {
			size_t open = end - 1;
			while (open && in[open - 1] >= '0' && in[open - 1] <= '9')
				open--;
			if (open && in[open - 1] == '{' && open < end - 1) {
				size_t n = (size_t)strtoul(in + open, NULL, 10);
				if (line_end + n > len)
					return 0; /* the literal is not all here */
				at = line_end + n;
				continue; /* the line goes on after it */
			}
		}
		return line_end;
	}
}

static void received(struct ui_socket *sock, const char *data, size_t size)
{
	struct imap_session *s = sock->user_data;
	if (!data) {
		lost(s, -CURLE_RECV_ERROR);
		return;
	}
	if (s->in_len + size > IN_MAX) {
		lost(s, -CURLE_RECV_ERROR);
		return;
	}
	char *p = realloc(s->in, s->in_len + size + 1);
	if (!p)
		return;
	memcpy(p + s->in_len, data, size);
	s->in = p;
	s->in_len += size;
	s->in[s->in_len] = '\0';
	size_t n;
	while (s->in && (n = response_len(s->in, s->in_len)) > 0) {
		/* Taken out first: dispatch() may close the session */
		char *resp = malloc(n + 1);
		memcpy(resp, s->in, n);
		resp[n] = '\0';
		memmove(s->in, s->in + n, s->in_len - n);
		s->in_len -= n;
		s->in[s->in_len] = '\0';
		dispatch(s, resp, n);
		free(resp);
	}
}

static void opened(struct ui_socket *sock, long status)
{
	struct imap_session *s = sock->user_data;
	if (status) {
		lost(s, (int)status);
		return;
	}
	/* Nothing waits for the greeting: libcurl loses what the server sends before the
	 * first read, so a NOOP goes first and its answer says the server is there */
	s->state = ST_HELLO;
	send_text(s, HELLO_TAG " NOOP\r\n");
}

void imap_connect(struct imap_session *s, const char *url, const char *user, const char *password,
                  void (*ready)(int status, void *user), void *ready_user)
{
	imap_close(s);
	/* Never in clear: the login goes over TLS or not at all */
	if (strncasecmp(url, "imaps://", 8) != 0) {
		ready(-CURLE_UNSUPPORTED_PROTOCOL, ready_user);
		return;
	}
	s->ready = ready;
	s->ready_user = ready_user;
	/* The login waits in the queue until the greeting */
	s->queue[0].text = strdup(user);
	s->queue[1].text = strdup(password);
	s->queued = 2;
	s->state = ST_CONNECTING;
	ui_socket_open(&s->sock, url, opened, received, s);
}
