/*
 * An IMAP server for the UI tests (ui_test.h): listening on the loopback address,
 * each connection answered from a thread of its own, as much of RFC 3501 as Email
 * uses, from the messages the test added. The tag comes back on the tagged line;
 * LOGIN is checked against the one account; FETCH takes sequence ranges (a:b, a:*,
 * a) of UID, FLAGS, INTERNALDATE and ENVELOPE, strings quoted or as literals when
 * asked, as Dovecot sends them; a literal the client sends ({n}CRLF, a password with
 * special characters) is taken after the continuation request.
 */

#define _GNU_SOURCE /* strcasecmp, strndup */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ui_test.h"

#define MAX_MESSAGES 2000
#define LINE_MAX_LEN 65536

struct message {
	char from_name[64], from_address[96], subject[192], date[48];
	bool seen;
	char *plain, *html;        /* the body's parts, NULL none */
	char *structure, *fetched; /* ui_test_imap_raw_body()'s, NULL none */
};

static struct {
	pthread_mutex_t lock;
	char user[128], password[128];
	struct message *messages;
	unsigned count;
	bool literals;
	char greeting[128]; /* "" the usual one */
	char refused[64], refusal[128]; /* commands starting so, answered so ("" hung up on) */
	char *extra;        /* ui_test_imap_fetch_extra()'s, NULL none */
	unsigned logins;
	char commands[65536];
	size_t commands_len;
	int listener;
	pthread_t thread;
} srv = { .lock = PTHREAD_MUTEX_INITIALIZER, .listener = -1 };

/* A connection */
struct conn {
	int fd;
	bool logged_in, selected;
};

static void send_all(struct conn *c, const char *text, size_t len)
{
	while (len) {
		ssize_t n = send(c->fd, text, len, MSG_NOSIGNAL);
		if (n <= 0)
			return;
		text += n;
		len -= (size_t)n;
	}
}

static void reply(struct conn *c, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	send_all(c, buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* A string as the server sends it: NIL when empty, quoted, or a literal */
static void string(char *out, size_t size, const char *s)
{
	if (!s[0]) {
		snprintf(out, size, "NIL");
		return;
	}
	if (srv.literals) {
		snprintf(out, size, "{%zu}\r\n%s", strlen(s), s);
		return;
	}
	size_t n = 0;
	out[n++] = '"';
	for (; *s && n < size - 2; s++) {
		if ((*s == '"' || *s == '\\') && n < size - 3)
			out[n++] = '\\';
		out[n++] = *s;
	}
	out[n++] = '"';
	out[n] = '\0';
}

/* Encoded for the wire: base64 in lines of 76, or quoted-printable */
static char *base64(const char *s)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t len = strlen(s);
	char *out = malloc(len / 3 * 4 + 8 + len / 57 * 2), *p = out;
	int col = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned char)s[i] << 16 | (i + 1 < len ? (unsigned char)s[i + 1] << 8 : 0) |
		             (i + 2 < len ? (unsigned char)s[i + 2] : 0);
		*p++ = digits[v >> 18];
		*p++ = digits[v >> 12 & 63];
		*p++ = i + 1 < len ? digits[v >> 6 & 63] : '=';
		*p++ = i + 2 < len ? digits[v & 63] : '=';
		if ((col += 4) == 76) {
			*p++ = '\r';
			*p++ = '\n';
			col = 0;
		}
	}
	*p = '\0';
	return out;
}

static char *quoted_printable(const char *s)
{
	char *out = malloc(3 * strlen(s) + strlen(s) / 20 * 3 + 8), *p = out;
	int col = 0;
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		if (ch == '\n') {
			*p++ = '\r';
			*p++ = '\n';
			col = 0;
			continue;
		}
		if (col >= 70) { /* a soft break */
			*p++ = '=';
			*p++ = '\r';
			*p++ = '\n';
			col = 0;
		}
		if (ch == '=' || ch >= 0x80 || ch < 0x20) {
			p += sprintf(p, "=%02X", ch);
			col += 3;
		} else {
			*p++ = (char)ch;
			col++;
		}
	}
	*p = '\0';
	return out;
}

/* The i-th message (1-based sequence number) as a FETCH response: its sender NIL
 * without an address, several senders when separated by ", " (the name the first's) */
static void fetch_one(struct conn *c, unsigned seq)
{
	const struct message *m = &srv.messages[seq - 1];
	char name[256], subject[256], addr[640] = "NIL";
	string(name, sizeof(name), m->from_name);
	string(subject, sizeof(subject), m->subject);
	if (m->from_address[0]) {
		size_t n = (size_t)snprintf(addr, sizeof(addr), "(");
		for (const char *a = m->from_address; *a;) {
			size_t len = strcspn(a, ",");
			const char *at = memchr(a, '@', len);
			int local = at ? (int)(at - a) : 0, domain = at ? (int)(len - (size_t)local - 1) : 0;
			n += (size_t)snprintf(addr + n, sizeof(addr) - n, "(%s NIL \"%.*s\" \"%.*s\")",
			                      a == m->from_address ? name : "NIL", local, a, domain, at ? at + 1 : "");
			a += len;
			while (*a == ',' || *a == ' ')
				a++;
		}
		snprintf(addr + n, sizeof(addr) - n, ")");
	}
	reply(c, "* %u FETCH (UID %u FLAGS (%s) INTERNALDATE \"%s\" ENVELOPE (\"%s\" %s %s %s %s "
	         "((NIL NIL \"me\" \"example.com\")) NIL NIL NIL \"<%u@fake>\"))\r\n",
	      seq, 100 + seq, m->seen ? "\\Seen" : "", m->date, m->date, subject, addr, addr, addr, 100 + seq);
}

/* The message with that UID, NULL if none */
static struct message *by_uid(unsigned long uid, unsigned *seq)
{
	if (uid < 101 || uid > 100 + srv.count)
		return NULL;
	*seq = (unsigned)(uid - 100);
	return &srv.messages[uid - 101];
}

/* A part of a BODYSTRUCTURE */
static void part_structure(char *out, size_t size, const char *subtype, const char *encoding, const char *text)
{
	snprintf(out, size, "(\"TEXT\" \"%s\" (\"CHARSET\" \"utf-8\") NIL NIL \"%s\" %zu 1 NIL NIL NIL NIL)", subtype,
	         encoding, strlen(text));
}

/* UID FETCH and UID STORE, of one message */
static void uid_command(struct conn *c, const char *tag, const char *args)
{
	unsigned long uid;
	char items[256];
	unsigned seq;
	bool store = strncasecmp(args, " STORE ", 7) == 0;
	if (sscanf(args, store ? " STORE %lu %255[^\r\n]" : " FETCH %lu %255[^\r\n]", &uid, items) != 2) {
		reply(c, "%s BAD Invalid arguments\r\n", tag);
		return;
	}
	struct message *m = by_uid(uid, &seq);
	if (!m) {
		reply(c, "%s OK No such message\r\n", tag);
		return;
	}
	if (m->structure && !store) { /* as the test wrote them */
		if (strstr(items, "BODYSTRUCTURE"))
			reply(c, "* %u FETCH (UID %lu BODYSTRUCTURE %s)\r\n", seq, uid, m->structure);
		else
			send_all(c, m->fetched, strlen(m->fetched));
		reply(c, "%s OK Fetch completed\r\n", tag);
		return;
	}
	if (store) {
		if (strstr(items, "+FLAGS") && strstr(items, "\\Seen"))
			m->seen = true;
		if (!strstr(items, ".SILENT"))
			reply(c, "* %u FETCH (UID %lu FLAGS (%s))\r\n", seq, uid, m->seen ? "\\Seen" : "");
		reply(c, "%s OK Store completed\r\n", tag);
		return;
	}
	if (strstr(items, "BODYSTRUCTURE")) {
		char plain[256] = "", html[256] = "";
		if (m->plain)
			part_structure(plain, sizeof(plain), "PLAIN", m->html ? "QUOTED-PRINTABLE" : "8BIT", m->plain);
		if (m->html)
			part_structure(html, sizeof(html), "HTML", "BASE64", m->html);
		if (m->plain && m->html)
			reply(c, "* %u FETCH (UID %lu BODYSTRUCTURE (%s%s \"ALTERNATIVE\" (\"BOUNDARY\" \"b1\") NIL NIL NIL))\r\n",
			      seq, uid, plain, html);
		else if (m->plain || m->html)
			reply(c, "* %u FETCH (UID %lu BODYSTRUCTURE %s)\r\n", seq, uid, m->plain ? plain : html);
		else
			reply(c, "* %u FETCH (UID %lu BODYSTRUCTURE (\"APPLICATION\" \"OCTET-STREAM\" NIL NIL NIL \"BASE64\" 0 NIL NIL NIL NIL))\r\n",
			      seq, uid);
		reply(c, "%s OK Fetch completed\r\n", tag);
		return;
	}
	char section[32];
	if (sscanf(items, "(BODY.PEEK[%31[^]]]", section) == 1 || sscanf(items, "(BODY[%31[^]]]", section) == 1) {
		/* Section 1: the only part, or the plain one of two; 2: the HTML one of two */
		const char *text = NULL, *encoding = "8BIT";
		bool both = m->plain && m->html;
		if (strcmp(section, "1") == 0 && both) {
			text = m->plain;
			encoding = "QUOTED-PRINTABLE";
		} else if (strcmp(section, "2") == 0 && both) {
			text = m->html;
			encoding = "BASE64";
		} else if (strcmp(section, "1") == 0) { /* the only part: as BODYSTRUCTURE declared it */
			text = m->plain ? m->plain : m->html;
			encoding = m->plain ? "8BIT" : "BASE64";
		}
		if (!text) {
			reply(c, "%s BAD No such part\r\n", tag);
			return;
		}
		char *wire = strcmp(encoding, "BASE64") == 0 ? base64(text) : strcmp(encoding, "QUOTED-PRINTABLE") == 0 ? quoted_printable(text) : strdup(text);
		reply(c, "* %u FETCH (UID %lu BODY[%s] {%zu}\r\n", seq, uid, section, strlen(wire));
		send_all(c, wire, strlen(wire));
		reply(c, ")\r\n%s OK Fetch completed\r\n", tag);
		free(wire);
		return;
	}
	reply(c, "%s BAD Unknown items\r\n", tag);
}

/* An astring of the command (an atom, or quoted), into out; *p after it */
static bool astring(const char **p, char *out, size_t size)
{
	const char *s = *p;
	while (*s == ' ')
		s++;
	size_t n = 0;
	if (*s == '"') {
		for (s++; *s && *s != '"'; s++) {
			char ch = *s;
			if (ch == '\\' && s[1])
				ch = *++s;
			if (n < size - 1)
				out[n++] = ch;
		}
		if (*s != '"')
			return false;
		s++;
	} else {
		for (; *s && *s != ' '; s++)
			if (n < size - 1)
				out[n++] = *s;
		if (!n)
			return false;
	}
	out[n] = '\0';
	*p = s;
	return true;
}

static void line(struct conn *c, const char *text)
{
	char tag[32], command[32];
	int n = 0;
	if (sscanf(text, "%31s %31s%n", tag, command, &n) < 2) {
		reply(c, "* BAD Missing command\r\n");
		return;
	}
	const char *args = text + n;
	/* Logged without the tag: the test checks what was asked */
	pthread_mutex_lock(&srv.lock);
	size_t len = strlen(text + strlen(tag) + 1);
	if (srv.commands_len + len + 1 < sizeof(srv.commands)) {
		memcpy(srv.commands + srv.commands_len, text + strlen(tag) + 1, len);
		srv.commands_len += len;
		srv.commands[srv.commands_len++] = '\n';
		srv.commands[srv.commands_len] = '\0';
	}

	if (srv.refused[0] && strncasecmp(text + strlen(tag) + 1, srv.refused, strlen(srv.refused)) == 0) {
		if (srv.refusal[0])
			reply(c, "%s %s\r\n", tag, srv.refusal);
		else
			shutdown(c->fd, SHUT_RDWR); /* hung up on */
	} else if (strcasecmp(command, "CAPABILITY") == 0) {
		reply(c, "* CAPABILITY IMAP4rev1 LITERAL+\r\n%s OK Capability completed\r\n", tag);
	} else if (strcasecmp(command, "LOGOUT") == 0) {
		reply(c, "* BYE Logging out\r\n%s OK Logout completed\r\n", tag);
		shutdown(c->fd, SHUT_RDWR);
	} else if (strcasecmp(command, "NOOP") == 0) {
		reply(c, "%s OK NOOP completed\r\n", tag);
	} else if (strcasecmp(command, "LOGIN") == 0) {
		char user[128], password[128];
		const char *p = args;
		if (!astring(&p, user, sizeof(user)) || !astring(&p, password, sizeof(password))) {
			reply(c, "%s BAD Invalid arguments\r\n", tag);
		} else if (strcmp(user, srv.user) == 0 && strcmp(password, srv.password) == 0) {
			c->logged_in = true;
			srv.logins++;
			reply(c, "%s OK [CAPABILITY IMAP4rev1] Logged in\r\n", tag);
		} else {
			reply(c, "%s NO [AUTHENTICATIONFAILED] Authentication failed.\r\n", tag);
		}
	} else if (!c->logged_in) {
		reply(c, "%s NO Please login first.\r\n", tag);
	} else if (strcasecmp(command, "SELECT") == 0 || strcasecmp(command, "EXAMINE") == 0) {
		if (strcasecmp(args, " INBOX") != 0 && strcasecmp(args, " \"INBOX\"") != 0) {
			reply(c, "%s NO Mailbox doesn't exist\r\n", tag);
		} else {
			c->selected = true;
			reply(c, "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
			         "* OK [PERMANENTFLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft \\*)] Flags permitted.\r\n"
			         "* %u EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 1703602304] UIDs valid\r\n"
			         "* OK [UIDNEXT %u] Predicted next UID\r\n%s OK [READ-WRITE] Select completed\r\n",
			      srv.count, 101 + srv.count, tag);
		}
	} else if (!c->selected && (strcasecmp(command, "UID") == 0 || strcasecmp(command, "FETCH") == 0 ||
	                            strcasecmp(command, "STORE") == 0)) {
		reply(c, "%s BAD Error in IMAP command %s: Mailbox doesn't exist or isn't selected\r\n", tag, command);
	} else if (strcasecmp(command, "UID") == 0) {
		uid_command(c, tag, args);
	} else if (strcasecmp(command, "STATUS") == 0) {
		reply(c, "* STATUS INBOX (MESSAGES %u)\r\n%s OK Status completed\r\n", srv.count, tag);
	} else if (strcasecmp(command, "FETCH") == 0) {
		unsigned from, to;
		char star;
		if (sscanf(args, " %u:%u", &from, &to) == 2) {
		} else if (sscanf(args, " %u:%c", &from, &star) == 2 && star == '*') {
			to = srv.count;
		} else if (sscanf(args, " %u", &from) == 1) {
			to = from;
		} else {
			reply(c, "%s BAD Invalid sequence set\r\n", tag);
			pthread_mutex_unlock(&srv.lock);
			return;
		}
		if (from > to) {
			unsigned t = from;
			from = to;
			to = t;
		}
		if (from < 1 || to > srv.count) {
			reply(c, "%s BAD Error in IMAP command FETCH: Invalid messageset\r\n", tag);
		} else {
			for (unsigned i = from; i <= to; i++)
				fetch_one(c, i);
			if (srv.extra)
				send_all(c, srv.extra, strlen(srv.extra));
			reply(c, "%s OK Fetch completed\r\n", tag);
		}
	} else {
		reply(c, "%s BAD Unknown command\r\n", tag);
	}
	pthread_mutex_unlock(&srv.lock);
}

/* One connection: lines, with the literals the client sends taken in as quoted strings */
static void *connection(void *arg)
{
	struct conn *c = arg;
	pthread_mutex_lock(&srv.lock);
	if (srv.greeting[0]) { /* PREAUTH: logged in by the connection itself */
		c->logged_in = strncmp(srv.greeting, "* PREAUTH", 9) == 0;
		reply(c, "%s\r\n", srv.greeting);
	} else {
		reply(c, "* OK [CAPABILITY IMAP4rev1 LITERAL+ AUTH=PLAIN] Fake IMAP ready.\r\n");
	}
	pthread_mutex_unlock(&srv.lock);
	char *buf = malloc(LINE_MAX_LEN);
	size_t len = 0;
	for (;;) {
		ssize_t n = recv(c->fd, buf + len, LINE_MAX_LEN - 1 - len, 0);
		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = '\0';
		char *nl;
		while ((nl = memchr(buf, '\n', len))) {
			size_t line_len = (size_t)(nl - buf);
			size_t end = line_len && buf[line_len - 1] == '\r' ? line_len - 1 : line_len;
			/* A literal: {n} ending the line, its bytes after our go-ahead */
			if (end > 2 && buf[end - 1] == '}') {
				char *open = memrchr(buf, '{', end);
				unsigned long lit = open ? strtoul(open + 1, NULL, 10) : 0;
				if (open && lit && lit < LINE_MAX_LEN / 2) {
					if (!strchr(open, '+')) /* not LITERAL+: asked to go ahead */
						reply(c, "+ OK\r\n");
					/* The literal's bytes, then the rest of the line, as a quoted string */
					while (len - line_len - 1 < lit) {
						ssize_t more = recv(c->fd, buf + len, LINE_MAX_LEN - 1 - len, 0);
						if (more <= 0)
							goto done;
						len += (size_t)more;
						buf[len] = '\0';
					}
					char *quoted = malloc(lit * 2 + 3), *q = quoted;
					*q++ = '"';
					for (size_t i = 0; i < lit; i++) {
						char ch = buf[line_len + 1 + i];
						if (ch == '"' || ch == '\\')
							*q++ = '\\';
						*q++ = ch;
					}
					*q++ = '"';
					*q = '\0';
					size_t head = (size_t)(open - buf), after = line_len + 1 + lit, qlen = strlen(quoted);
					memmove(buf + head + qlen, buf + after, len - after);
					memcpy(buf + head, quoted, qlen);
					len = head + qlen + (len - after);
					buf[len] = '\0';
					free(quoted);
					continue; /* the line goes on: looked at again */
				}
			}
			char *text = strndup(buf, end);
			memmove(buf, nl + 1, len - line_len - 1);
			len -= line_len + 1;
			buf[len] = '\0';
			line(c, text);
			free(text);
		}
	}
done:
	free(buf);
	close(c->fd);
	free(c);
	return NULL;
}

static void *listener(void *unused)
{
	(void)unused;
	for (;;) {
		int fd = accept(srv.listener, NULL, NULL);
		if (fd < 0)
			break;
		struct conn *c = calloc(1, sizeof(*c));
		c->fd = fd;
		pthread_t t;
		if (pthread_create(&t, NULL, connection, c) == 0)
			pthread_detach(t);
	}
	return NULL;
}

void ui_test_imap_server(const char *url, const char *user, const char *password)
{
	pthread_mutex_lock(&srv.lock);
	for (unsigned i = 0; i < srv.count; i++) {
		free(srv.messages[i].plain);
		free(srv.messages[i].html);
		free(srv.messages[i].structure);
		free(srv.messages[i].fetched);
	}
	free(srv.messages);
	free(srv.extra);
	srv.extra = NULL;
	srv.messages = calloc(MAX_MESSAGES, sizeof(*srv.messages));
	srv.count = 0;
	srv.literals = false;
	srv.greeting[0] = srv.refused[0] = srv.refusal[0] = '\0';
	srv.logins = 0;
	srv.commands_len = 0;
	srv.commands[0] = '\0';
	snprintf(srv.user, sizeof(srv.user), "%s", user);
	snprintf(srv.password, sizeof(srv.password), "%s", password);
	pthread_mutex_unlock(&srv.lock);
	if (srv.listener >= 0)
		return; /* listening already, the port the same */
	const char *colon = strrchr(url, ':');
	uint16_t port = colon ? (uint16_t)strtoul(colon + 1, NULL, 10) : 143;
	srv.listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(srv.listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(srv.listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv.listener, 8) != 0) {
		perror("ui_test: the IMAP server cannot listen");
		return;
	}
	pthread_create(&srv.thread, NULL, listener, NULL);
}

void ui_test_imap_message(const char *from_name, const char *from_address, const char *subject,
                          const char *internaldate, bool seen)
{
	pthread_mutex_lock(&srv.lock);
	if (srv.count < MAX_MESSAGES) {
		struct message *m = &srv.messages[srv.count++];
		snprintf(m->from_name, sizeof(m->from_name), "%s", from_name ? from_name : "");
		snprintf(m->from_address, sizeof(m->from_address), "%s", from_address);
		snprintf(m->subject, sizeof(m->subject), "%s", subject);
		snprintf(m->date, sizeof(m->date), "%s", internaldate);
		m->seen = seen;
	}
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_body(const char *plain, const char *html)
{
	pthread_mutex_lock(&srv.lock);
	if (srv.count) {
		struct message *m = &srv.messages[srv.count - 1];
		free(m->plain);
		free(m->html);
		m->plain = plain ? strdup(plain) : NULL;
		m->html = html ? strdup(html) : NULL;
	}
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_raw_body(const char *structure, const char *fetched)
{
	pthread_mutex_lock(&srv.lock);
	if (srv.count) {
		struct message *m = &srv.messages[srv.count - 1];
		free(m->structure);
		free(m->fetched);
		m->structure = strdup(structure);
		m->fetched = strdup(fetched);
	}
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_fetch_extra(const char *lines)
{
	pthread_mutex_lock(&srv.lock);
	free(srv.extra);
	srv.extra = lines ? strdup(lines) : NULL;
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_refuse(const char *command, const char *answer)
{
	pthread_mutex_lock(&srv.lock);
	snprintf(srv.refused, sizeof(srv.refused), "%s", command);
	snprintf(srv.refusal, sizeof(srv.refusal), "%s", answer ? answer : "");
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_greeting_text(const char *line)
{
	pthread_mutex_lock(&srv.lock);
	snprintf(srv.greeting, sizeof(srv.greeting), "%s", line);
	pthread_mutex_unlock(&srv.lock);
}

void ui_test_imap_literals(bool on)
{
	pthread_mutex_lock(&srv.lock);
	srv.literals = on;
	pthread_mutex_unlock(&srv.lock);
}

const char *ui_test_imap_commands(void)
{
	static char copy[sizeof(srv.commands)];
	pthread_mutex_lock(&srv.lock);
	memcpy(copy, srv.commands, srv.commands_len + 1);
	pthread_mutex_unlock(&srv.lock);
	return copy;
}

unsigned ui_test_imap_logins(void)
{
	pthread_mutex_lock(&srv.lock);
	unsigned n = srv.logins;
	pthread_mutex_unlock(&srv.lock);
	return n;
}
