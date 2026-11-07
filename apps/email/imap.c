/*
 * What the IMAP server says, parsed (RFC 3501's syntax, as much as the inbox needs):
 * a FETCH response's UID, FLAGS, INTERNALDATE and ENVELOPE (date, subject, the first
 * sender), EXISTS, and the headers' encoded words (RFC 2047: base64 or
 * quoted-printable in a charset) as UTF-8. Strings come quoted or as atoms, NIL for
 * none; a literal ({n}CRLF then its bytes) as the session hands it, whole.
 */

#define _GNU_SOURCE /* timegm */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "email.h"

struct cur {
	const char *p, *end;
};

static void ws(struct cur *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\r' || *c->p == '\n'))
		c->p++;
}

static bool is_atom_char(char ch)
{
	return ch > ' ' && ch != '(' && ch != ')' && ch != '{' && ch != '"' && ch != ']' && ch != '[';
}

/* Into out (cut to fit), "" when there is none */
static bool atom(struct cur *c, char *out, size_t size)
{
	ws(c);
	size_t n = 0;
	const char *start = c->p;
	while (c->p < c->end && is_atom_char(*c->p))
		c->p++;
	if (c->p == start)
		return false;
	n = (size_t)(c->p - start);
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), start);
	return true;
}

/* A quoted string, a literal, NIL ("") or an atom */
static bool string(struct cur *c, char *out, size_t size)
{
	ws(c);
	out[0] = '\0';
	if (c->p >= c->end)
		return false;
	if (*c->p == '"') {
		size_t n = 0;
		for (c->p++; c->p < c->end && *c->p != '"'; c->p++) {
			char ch = *c->p;
			if (ch == '\\' && c->p + 1 < c->end)
				ch = *++c->p;
			if (n < size - 1)
				out[n++] = ch;
		}
		out[n] = '\0';
		if (c->p >= c->end)
			return false;
		c->p++;
		return true;
	}
	if (*c->p == '{') {
		char *stop;
		unsigned long len = strtoul(c->p + 1, &stop, 10);
		if (*stop != '}')
			return false;
		const char *data = stop + 1;
		if (data < c->end && *data == '\r')
			data++;
		if (data < c->end && *data == '\n')
			data++;
		if ((size_t)(c->end - data) < len)
			return false; /* cut */
		snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), data);
		c->p = data + len;
		return true;
	}
	if (!atom(c, out, size))
		return false;
	if (strcmp(out, "NIL") == 0)
		out[0] = '\0';
	return true;
}

/* A value of any kind passed over: a string, or a list with its nesting */
static bool skip_value(struct cur *c)
{
	ws(c);
	if (c->p < c->end && *c->p == '(') {
		int depth = 0;
		for (; c->p < c->end; c->p++) {
			if (*c->p == '"') {
				char junk[2];
				if (!string(c, junk, sizeof(junk)))
					return false;
				c->p--;
			} else if (*c->p == '(') {
				depth++;
			} else if (*c->p == ')' && --depth == 0) {
				c->p++;
				return true;
			}
		}
		return false;
	}
	char junk[8];
	return string(c, junk, sizeof(junk));
}

static bool open_list(struct cur *c)
{
	ws(c);
	if (c->p >= c->end || *c->p != '(')
		return false;
	c->p++;
	return true;
}

static bool close_list(struct cur *c)
{
	ws(c);
	if (c->p >= c->end || *c->p != ')')
		return false;
	c->p++;
	return true;
}

/* An address list, ((name adl mailbox host) ...) or NIL: the first one's name and
 * address into m, the rest passed over */
static bool from_parse(struct cur *c, struct mail *m)
{
	ws(c);
	if (c->p < c->end && *c->p != '(')
		return skip_value(c); /* NIL */
	if (!open_list(c) || !open_list(c))
		return false;
	char name[sizeof(m->from)], adl[8], mailbox[64], host[sizeof(m->address) - 66];
	if (!string(c, name, sizeof(name)) || !string(c, adl, sizeof(adl)) || !string(c, mailbox, sizeof(mailbox)) ||
	    !string(c, host, sizeof(host)) || !close_list(c))
		return false;
	snprintf(m->address, sizeof(m->address), "%s@%s", mailbox, host);
	mime_header_decode(name, m->from, sizeof(m->from));
	if (!m->from[0])
		snprintf(m->from, sizeof(m->from), "%s", m->address);
	/* The other senders */
	while (c->p < c->end) {
		ws(c);
		if (*c->p == ')')
			break;
		if (!skip_value(c))
			return false;
	}
	return close_list(c);
}

/* (date subject from sender reply-to to cc bcc in-reply-to message-id) */
static bool envelope_parse(struct cur *c, struct mail *m)
{
	char date[64], subject[sizeof(m->subject)];
	if (!open_list(c) || !string(c, date, sizeof(date)) || !string(c, subject, sizeof(subject)))
		return false;
	mime_header_decode(subject, m->subject, sizeof(m->subject));
	if (!from_parse(c, m))
		return false;
	for (int i = 0; i < 7; i++)
		if (!skip_value(c))
			return false;
	return close_list(c);
}

bool imap_fetch_parse(const char *line, size_t len, struct mail *m)
{
	struct cur c = {line, line + len};
	char word[32];
	memset(m, 0, sizeof(*m));
	if (!atom(&c, word, sizeof(word)) || strcmp(word, "*") != 0 || !atom(&c, word, sizeof(word)) ||
	    !atom(&c, word, sizeof(word)) || strcasecmp(word, "FETCH") != 0 || !open_list(&c))
		return false;
	for (;;) {
		ws(&c);
		if (c.p < c.end && *c.p == ')')
			return true;
		if (!atom(&c, word, sizeof(word)))
			return false;
		if (strcasecmp(word, "UID") == 0) {
			char n[24];
			if (!atom(&c, n, sizeof(n)))
				return false;
			m->uid = strtoul(n, NULL, 10);
		} else if (strcasecmp(word, "FLAGS") == 0) {
			if (!open_list(&c))
				return false;
			char flag[32];
			while (atom(&c, flag, sizeof(flag)))
				if (strcasecmp(flag, "\\Seen") == 0)
					m->seen = true;
			if (!close_list(&c))
				return false;
		} else if (strcasecmp(word, "INTERNALDATE") == 0) {
			char date[48];
			if (!string(&c, date, sizeof(date)))
				return false;
			m->date = imap_date_parse(date);
		} else if (strcasecmp(word, "ENVELOPE") == 0) {
			if (!envelope_parse(&c, m))
				return false;
		} else if (!skip_value(&c)) {
			return false;
		}
	}
}

long imap_exists(const char *response, size_t len)
{
	struct cur c = {response, response + len};
	char word[32], n[24];
	if (!atom(&c, word, sizeof(word)) || strcmp(word, "*") != 0 || !atom(&c, n, sizeof(n)) ||
	    !atom(&c, word, sizeof(word)) || strcasecmp(word, "EXISTS") != 0)
		return -1;
	return strtol(n, NULL, 10);
}

time_t imap_date_parse(const char *s)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec"; /* NON-NLS */
	int day, year, h, mi, sec, zh, zm;
	char mon[4], sign;
	if (sscanf(s, "%d-%3s-%d %d:%d:%d %c%2d%2d", &day, mon, &year, &h, &mi, &sec, &sign, &zh, &zm) != 9)
		return 0;
	const char *m = strstr(months, mon);
	if (!m || (m - months) % 3)
		return 0;
	struct tm tm = {.tm_mday = day, .tm_mon = (int)(m - months) / 3, .tm_year = year - 1900,
	                .tm_hour = h, .tm_min = mi, .tm_sec = sec};
	time_t t = timegm(&tm);
	long offset = (zh * 60 + zm) * 60;
	return t - (sign == '-' ? -offset : offset);
}

/* RFC 2047 */

static int b64_value(char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A';
	if (ch >= 'a' && ch <= 'z')
		return ch - 'a' + 26;
	if (ch >= '0' && ch <= '9')
		return ch - '0' + 52;
	return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

static size_t b64_decode(const char *in, size_t len, unsigned char *out, size_t size)
{
	size_t n = 0;
	unsigned bits = 0, have = 0;
	for (size_t i = 0; i < len; i++) {
		int v = b64_value(in[i]);
		if (v < 0)
			continue;
		bits = bits << 6 | (unsigned)v;
		have += 6;
		if (have >= 8) {
			have -= 8;
			if (n < size)
				out[n++] = (unsigned char)(bits >> have);
		}
	}
	return n;
}

static int hex_value(char ch)
{
	return isdigit((unsigned char)ch) ? ch - '0' : isxdigit((unsigned char)ch) ? (tolower(ch) - 'a' + 10) : -1;
}

/* Q: = hex hex, _ a space */
static size_t q_decode(const char *in, size_t len, unsigned char *out, size_t size)
{
	size_t n = 0;
	for (size_t i = 0; i < len && n < size; i++) {
		if (in[i] == '_') {
			out[n++] = ' ';
		} else if (in[i] == '=' && i + 2 < len && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
			out[n++] = (unsigned char)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
			i += 2;
		} else {
			out[n++] = (unsigned char)in[i];
		}
	}
	return n;
}

/* Windows-1252's own characters (0x80-0x9F), as Unicode; Latin-1 otherwise */
static unsigned cp1252(unsigned char ch)
{
	static const unsigned short high[32] = {
		0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
		0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
	if (ch >= 0x80 && ch < 0xA0 && high[ch - 0x80])
		return high[ch - 0x80];
	return ch;
}

static size_t utf8_put(unsigned cp, char *out, size_t size)
{
	if (cp < 0x80) {
		if (size < 1)
			return 0;
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (size < 2)
			return 0;
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (size < 3)
		return 0;
	out[0] = (char)(0xE0 | cp >> 12);
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(0x80 | (cp & 0x3F));
	return 3;
}

/* The decoded bytes of one word, in `charset`, appended as UTF-8 */
static size_t to_utf8(const char *charset, const unsigned char *bytes, size_t len, char *out, size_t size)
{
	size_t n = 0;
	bool latin = strncasecmp(charset, "iso-8859-", 9) == 0 || strcasecmp(charset, "us-ascii") == 0 ||
	             strcasecmp(charset, "windows-1252") == 0 || strcasecmp(charset, "cp1252") == 0;
	if (!latin) { /* UTF-8, or taken as it is */
		n = len < size ? len : size;
		memcpy(out, bytes, n);
		return n;
	}
	bool windows = strncasecmp(charset, "iso-8859-", 9) != 0 && strcasecmp(charset, "us-ascii") != 0;
	for (size_t i = 0; i < len; i++) {
		unsigned cp = windows ? cp1252(bytes[i]) : bytes[i];
		size_t put = utf8_put(cp, out + n, size - n);
		if (!put)
			break;
		n += put;
	}
	return n;
}

/* An encoded word at `p`: appended, *p after it; false if not one */
static bool encoded_word(const char **p, char *out, size_t size, size_t *n)
{
	const char *s = *p;
	if (strncmp(s, "=?", 2) != 0)
		return false;
	const char *q1 = strchr(s + 2, '?');
	if (!q1 || q1[1] == '\0' || q1[2] != '?')
		return false;
	char enc = (char)toupper((unsigned char)q1[1]);
	if (enc != 'B' && enc != 'Q')
		return false;
	const char *text = q1 + 3;
	const char *stop = strstr(text, "?=");
	if (!stop)
		return false;
	char charset[32];
	snprintf(charset, sizeof(charset), "%.*s", (int)(q1 - (s + 2) < 31 ? q1 - (s + 2) : 31), s + 2);
	char *star = strchr(charset, '*'); /* a language tag after it */
	if (star)
		*star = '\0';
	unsigned char bytes[256];
	size_t len = enc == 'B' ? b64_decode(text, (size_t)(stop - text), bytes, sizeof(bytes))
	                        : q_decode(text, (size_t)(stop - text), bytes, sizeof(bytes));
	*n += to_utf8(charset, bytes, len, out + *n, size - 1 - *n);
	*p = stop + 2;
	return true;
}

void mime_header_decode(const char *in, char *out, size_t size)
{
	size_t n = 0;
	const char *p = in;
	bool after_word = false; /* the space between two encoded words is not shown */
	while (*p && n < size - 1) {
		if (p[0] == '=' && p[1] == '?') {
			size_t before = n;
			if (encoded_word(&p, out, size, &n)) {
				after_word = true;
				continue;
			}
			n = before;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			const char *q = p;
			while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
				q++;
			if (!(after_word && q[0] == '=' && q[1] == '?'))
				out[n++] = ' ';
			p = q;
			continue;
		}
		out[n++] = *p++;
		after_word = false;
	}
	/* Not cut inside a UTF-8 sequence */
	while (n && (out[n - 1] & 0xC0) == 0x80 && n >= size - 1)
		n--;
	out[n] = '\0';
}

/* Bodies */

/* "* N FETCH (" passed: at the first item */
static bool fetch_open(struct cur *c)
{
	char word[32];
	return atom(c, word, sizeof(word)) && strcmp(word, "*") == 0 && atom(c, word, sizeof(word)) &&
	       atom(c, word, sizeof(word)) && strcasecmp(word, "FETCH") == 0 && open_list(c);
}

/* A body part's type, subtype, parameters (its charset), id, description, encoding and
 * size, then whatever else there is up to its ')' */
static bool part_parse(struct cur *c, const char *section, struct imap_part *html, struct imap_part *plain)
{
	struct imap_part p = {0};
	char type[32], key[40], value[40];
	snprintf(p.section, sizeof(p.section), "%s", section);
	if (!string(c, type, sizeof(type)) || !string(c, p.subtype, sizeof(p.subtype)))
		return false;
	ws(c);
	if (c->p < c->end && *c->p == '(') { /* parameters */
		c->p++;
		for (;;) {
			ws(c);
			if (c->p < c->end && *c->p == ')')
				break;
			if (!string(c, key, sizeof(key)) || !string(c, value, sizeof(value)))
				return false;
			if (strcasecmp(key, "charset") == 0)
				snprintf(p.charset, sizeof(p.charset), "%s", value);
		}
		c->p++;
	} else if (!skip_value(c)) { /* NIL */
		return false;
	}
	char junk[8];
	if (!skip_value(c) || !skip_value(c) || !string(c, p.encoding, sizeof(p.encoding)) || !atom(c, junk, sizeof(junk)))
		return false;
	while (c->p < c->end) { /* lines, md5, disposition, language, location... */
		ws(c);
		if (*c->p == ')')
			break;
		if (!skip_value(c))
			return false;
	}
	if (strcasecmp(type, "text") == 0) {
		if (strcasecmp(p.subtype, "html") == 0 && !html->section[0])
			*html = p;
		else if (strcasecmp(p.subtype, "plain") == 0 && !plain->section[0])
			*plain = p;
	}
	return close_list(c);
}

/* A body: a part, or a multipart whose parts are numbered prefix.1, prefix.2... */
static bool body_parse(struct cur *c, const char *prefix, int depth, struct imap_part *html, struct imap_part *plain)
{
	if (!open_list(c) || depth > 8)
		return false;
	ws(c);
	if (c->p < c->end && *c->p == '(') { /* multipart */
		for (int i = 1;; i++) {
			ws(c);
			if (c->p >= c->end || *c->p != '(')
				break;
			char section[32];
			if (prefix[0])
				snprintf(section, sizeof(section), "%s.%d", prefix, i);
			else
				snprintf(section, sizeof(section), "%d", i);
			if (!body_parse(c, section, depth + 1, html, plain))
				return false;
		}
		char subtype[32];
		if (!string(c, subtype, sizeof(subtype)))
			return false;
		while (c->p < c->end) {
			ws(c);
			if (*c->p == ')')
				break;
			if (!skip_value(c))
				return false;
		}
		return close_list(c);
	}
	return part_parse(c, prefix[0] ? prefix : "1", html, plain);
}

bool imap_bodystructure_parse(const char *response, size_t len, struct imap_part *html, struct imap_part *plain)
{
	struct cur c = {response, response + len};
	char word[32];
	memset(html, 0, sizeof(*html));
	memset(plain, 0, sizeof(*plain));
	if (!fetch_open(&c))
		return false;
	for (;;) {
		ws(&c);
		if (c.p < c.end && *c.p == ')')
			return html->section[0] || plain->section[0];
		if (!atom(&c, word, sizeof(word)))
			return false;
		if (strcasecmp(word, "BODYSTRUCTURE") == 0) {
			if (!body_parse(&c, "", 0, html, plain))
				return false;
		} else if (!skip_value(&c)) {
			return false;
		}
	}
}

bool imap_body_literal(const char *response, size_t len, const char **data, size_t *size)
{
	struct cur c = {response, response + len};
	char word[32];
	if (!fetch_open(&c))
		return false;
	for (;;) {
		ws(&c);
		if (c.p >= c.end || *c.p == ')')
			return false;
		if (!atom(&c, word, sizeof(word)))
			return false;
		if (strcasecmp(word, "BODY") == 0 && c.p < c.end && *c.p == '[') {
			/* BODY[section]<origin> {n} */
			while (c.p < c.end && *c.p != ' ' && *c.p != '{')
				c.p++;
			ws(&c);
			if (c.p >= c.end || *c.p != '{')
				return false;
			char *stop;
			unsigned long n = strtoul(c.p + 1, &stop, 10);
			if (*stop != '}')
				return false;
			const char *p = stop + 1;
			if (p < c.end && *p == '\r')
				p++;
			if (p < c.end && *p == '\n')
				p++;
			if ((size_t)(c.end - p) < n)
				return false;
			*data = p;
			*size = n;
			return true;
		}
		if (!skip_value(&c))
			return false;
	}
}

/* Quoted-printable, a body's: soft line breaks (= at a line's end) removed */
static size_t qp_body_decode(const char *in, size_t len, unsigned char *out)
{
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '=') {
			if (i + 2 < len && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
				out[n++] = (unsigned char)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
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

char *mime_decode_body(const char *data, size_t len, const char *encoding, const char *charset)
{
	unsigned char *bytes = malloc(len + 1);
	if (!bytes)
		return NULL;
	size_t n;
	if (strcasecmp(encoding, "base64") == 0)
		n = b64_decode(data, len, bytes, len);
	else if (strcasecmp(encoding, "quoted-printable") == 0)
		n = qp_body_decode(data, len, bytes);
	else {
		memcpy(bytes, data, len);
		n = len;
	}
	/* Three bytes of UTF-8 at most per byte of a single-byte charset */
	char *out = malloc(3 * n + 1);
	if (!out) {
		free(bytes);
		return NULL;
	}
	size_t m = to_utf8(charset && charset[0] ? charset : "utf-8", bytes, n, out, 3 * n);
	/* No NULs inside: a label stops there */
	for (size_t i = 0; i < m; i++)
		if (!out[i])
			out[i] = ' ';
	out[m] = '\0';
	free(bytes);
	return out;
}
