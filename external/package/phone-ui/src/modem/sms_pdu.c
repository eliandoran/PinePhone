/*
 * SMS PDUs, see sms_pdu.h. Sent with a relative validity of 4 days (as phones do),
 * a message reference of 0 (the modem sets its own), the service centre the SIM's.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sms_pdu.h"

#define GSM_ESC 0x1b
#define SEPTETS 160      /* GSM 7-bit characters in one message */
#define SEPTETS_PART 153 /* in a part, after its header (7 septets) */
#define UNITS 70         /* UCS-2 */
#define UNITS_PART 67
#define VALIDITY 0xaa    /* 4 days */

/* GSM 7-bit's default alphabet, in Unicode */
static const uint16_t gsm[128] = {
	'@', 0xa3, '$', 0xa5, 0xe8, 0xe9, 0xf9, 0xec, 0xf2, 0xc7, '\n', 0xd8, 0xf8, '\r', 0xc5, 0xe5,
	0x394, '_', 0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398, 0x39e, 0xa0, 0xc6, 0xe6, 0xdf, 0xc9,
	' ', '!', '"', '#', 0xa4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xa1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
	0xbf, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};

/* Its extension table: the septet after an escape */
static const struct {
	uint8_t septet;
	uint16_t code;
} gsm_ext[] = {
	{ 0x0a, 0x0c }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2f, '\\' },
	{ 0x3c, '[' }, { 0x3d, '~' }, { 0x3e, ']' }, { 0x40, '|' }, { 0x65, 0x20ac },
};

/* Decoding */

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* The PDU's octets; -1 if it is not hexadecimal */
static int unhex(const char *hex, uint8_t *out, size_t max)
{
	size_t n = strlen(hex);
	if (n % 2 || n / 2 > max)
		return -1;
	for (size_t i = 0; i < n / 2; i++) {
		int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return (int)(n / 2);
}

static size_t utf8_put(char *out, size_t size, size_t at, uint32_t c)
{
	char b[4];
	size_t n;
	if (c < 0x80) {
		b[0] = (char)c;
		n = 1;
	} else if (c < 0x800) {
		b[0] = (char)(0xc0 | c >> 6);
		b[1] = (char)(0x80 | (c & 0x3f));
		n = 2;
	} else if (c < 0x10000) {
		b[0] = (char)(0xe0 | c >> 12);
		b[1] = (char)(0x80 | (c >> 6 & 0x3f));
		b[2] = (char)(0x80 | (c & 0x3f));
		n = 3;
	} else {
		b[0] = (char)(0xf0 | c >> 18);
		b[1] = (char)(0x80 | (c >> 12 & 0x3f));
		b[2] = (char)(0x80 | (c >> 6 & 0x3f));
		b[3] = (char)(0x80 | (c & 0x3f));
		n = 4;
	}
	if (at + n >= size)
		return at; /* full: the rest dropped */
	memcpy(out + at, b, n);
	out[at + n] = '\0';
	return at + n;
}

/* Septet i of packed GSM 7-bit data */
static unsigned septet(const uint8_t *d, size_t size, size_t i)
{
	size_t bit = i * 7, byte = bit / 8;
	unsigned v = d[byte] >> (bit % 8);
	if (bit % 8 > 1 && byte + 1 < size)
		v |= d[byte + 1] << (8 - bit % 8);
	return v & 0x7f;
}

/* `count` septets from `first` as UTF-8 */
static void gsm_decode(const uint8_t *d, size_t size, size_t first, size_t count, char *out, size_t out_size)
{
	size_t at = 0;
	out[0] = '\0';
	for (size_t i = first; i < first + count; i++) {
		unsigned s = septet(d, size, i);
		if (s != GSM_ESC) {
			at = utf8_put(out, out_size, at, gsm[s]);
			continue;
		}
		if (++i == first + count)
			break;
		s = septet(d, size, i);
		uint16_t c = ' '; /* an escape to nothing known: a space, as the specification asks */
		for (size_t k = 0; k < sizeof(gsm_ext) / sizeof(gsm_ext[0]); k++)
			if (gsm_ext[k].septet == s)
				c = gsm_ext[k].code;
		at = utf8_put(out, out_size, at, c);
	}
}

static void ucs2_decode(const uint8_t *d, size_t size, char *out, size_t out_size)
{
	size_t at = 0;
	out[0] = '\0';
	for (size_t i = 0; i + 1 < size; i += 2) {
		uint32_t c = (uint32_t)d[i] << 8 | d[i + 1];
		if (c >= 0xd800 && c < 0xdc00 && i + 3 < size) {
			uint32_t lo = (uint32_t)d[i + 2] << 8 | d[i + 3];
			if (lo >= 0xdc00 && lo < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		if (c >= 0xd800 && c < 0xe000)
			c = 0xfffd; /* a lone surrogate */
		at = utf8_put(out, out_size, at, c);
	}
}

static int bcd(uint8_t b)
{
	return (b & 0x0f) * 10 + (b >> 4);
}

/* The service centre's time stamp: semi-octets swapped, the zone in quarters of an
 * hour (its sign bit 3) */
static time_t timestamp(const uint8_t *p)
{
	struct tm tm = {
		.tm_year = bcd(p[0]) < 70 ? bcd(p[0]) + 100 : bcd(p[0]), .tm_mon = bcd(p[1]) - 1, .tm_mday = bcd(p[2]),
		.tm_hour = bcd(p[3]), .tm_min = bcd(p[4]), .tm_sec = bcd(p[5]),
	};
	int quarters = (p[6] & 0x07) * 10 + (p[6] >> 4);
	if (p[6] & 0x08)
		quarters = -quarters;
	return timegm(&tm) - quarters * 15 * 60;
}

/* An address: `digits` semi-octets, its type, the digits */
static void address(const uint8_t *p, unsigned digits, char *out, size_t size)
{
	uint8_t type = p[0];
	p++;
	if ((type & 0x70) == 0x50) { /* alphanumeric: GSM 7-bit */
		gsm_decode(p, (digits + 1) / 2, 0, digits * 4 / 7, out, size);
		return;
	}
	size_t at = 0;
	if ((type & 0x70) == 0x10)
		out[at++] = '+';
	static const char semi[] = "0123456789*#abc";
	for (unsigned i = 0; i < digits && at + 1 < size; i++) {
		unsigned v = i % 2 ? p[i / 2] >> 4 : p[i / 2] & 0x0f;
		if (v < 15)
			out[at++] = semi[v];
	}
	out[at] = '\0';
}

enum alphabet { GSM7, DATA8, UCS2 };

/* From the data coding scheme; DATA8 also for what cannot be read (compressed) */
static enum alphabet alphabet_of(uint8_t dcs)
{
	if ((dcs & 0xc0) == 0x00 || (dcs & 0xc0) == 0x40) /* general, automatic deletion */
		return dcs & 0x20 ? DATA8 : (dcs >> 2 & 3) == 2 ? UCS2 : (dcs >> 2 & 3) == 1 ? DATA8 : GSM7;
	if ((dcs & 0xf0) == 0xf0)
		return dcs & 0x04 ? DATA8 : GSM7;
	if ((dcs & 0xf0) == 0xe0)
		return UCS2; /* message waiting, stored */
	if ((dcs & 0xe0) == 0xc0)
		return GSM7;
	return DATA8; /* reserved */
}

bool sms_deliver_decode(const char *hex, struct sms_deliver *out)
{
	uint8_t d[200];
	int n = unhex(hex, d, sizeof(d));
	memset(out, 0, sizeof(*out));
	out->total = out->seq = 1;
	if (n < 1)
		return false;

	size_t i = 1 + d[0]; /* past the service centre's address */
	if (i + 2 > (size_t)n)
		return false;
	uint8_t first = d[i++];
	if ((first & 0x03) != 0) /* not SMS-DELIVER */
		return false;
	unsigned digits = d[i++];
	if (digits > 20 || i + 1 + (digits + 1) / 2 + 10 > (size_t)n)
		return false;
	address(&d[i], digits, out->sender, sizeof(out->sender));
	i += 1 + (digits + 1) / 2;
	i++; /* protocol identifier */
	enum alphabet alphabet = alphabet_of(d[i++]);
	out->time = timestamp(&d[i]);
	i += 7;
	unsigned length = d[i++]; /* septets, or octets */
	const uint8_t *ud = &d[i];
	size_t size = (size_t)n - i;
	size_t octets = alphabet == GSM7 ? (length * 7 + 7) / 8 : length;
	if (alphabet == DATA8 || octets > size)
		return false;

	size_t header = 0; /* its octets, with their length */
	if (first & 0x40) {
		if (!size || (size_t)ud[0] + 1 > octets)
			return false;
		header = ud[0] + 1;
		for (size_t h = 1; h + 1 < header;) {
			uint8_t id = ud[h], len = ud[h + 1];
			if (h + 2 + len > header)
				break;
			const uint8_t *v = &ud[h + 2];
			if (id == 0x00 && len == 3 && v[1] && v[2] && v[2] <= v[1]) {
				out->ref = v[0];
				out->total = v[1];
				out->seq = v[2];
			} else if (id == 0x08 && len == 4 && v[2] && v[3] && v[3] <= v[2]) {
				out->ref = (unsigned)v[0] << 8 | v[1];
				out->total = v[2];
				out->seq = v[3];
			}
			h += 2 + len;
		}
	}

	if (alphabet == UCS2) {
		ucs2_decode(ud + header, octets - header, out->text, sizeof(out->text));
	} else {
		size_t skip = (header * 8 + 6) / 7; /* the header and its fill bits */
		if (skip > length)
			return false;
		gsm_decode(ud, octets, skip, length - skip, out->text, sizeof(out->text));
	}
	return true;
}

/* Encoding */

/* The next character of UTF-8 text; U+FFFD for a broken one */
static uint32_t utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t c = p[0];
	unsigned n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
	if (n == 4) {
		(*s)++;
		return 0xfffd;
	}
	c &= n ? 0x3f >> n : 0x7f;
	for (unsigned k = 1; k <= n; k++) {
		if ((p[k] & 0xc0) != 0x80) {
			*s += k;
			return 0xfffd;
		}
		c = c << 6 | (p[k] & 0x3f);
	}
	*s += n + 1;
	return c;
}

/* Its septets: 1, 2 (escaped), 0 not in GSM 7-bit */
static unsigned gsm_encode(uint32_t c, uint8_t *out)
{
	if (c == 0xa0) /* the table's 0x1b entry is the escape, not a space */
		return 0;
	for (unsigned s = 0; s < 128; s++)
		if (gsm[s] == c && s != GSM_ESC) {
			out[0] = (uint8_t)s;
			return 1;
		}
	for (size_t k = 0; k < sizeof(gsm_ext) / sizeof(gsm_ext[0]); k++)
		if (gsm_ext[k].code == c) {
			out[0] = GSM_ESC;
			out[1] = gsm_ext[k].septet;
			return 2;
		}
	return 0;
}

static void hex_put(char **at, const uint8_t *d, size_t n)
{
	for (size_t i = 0; i < n; i++)
		*at += sprintf(*at, "%02X", d[i]);
}

/* The PDU of one part: `ud` is its user data (with the header), `length` its length
 * in septets or octets */
static void part_put(struct sms_submit *out, const uint8_t *addr, size_t addr_size, bool ucs2, bool header,
                     const uint8_t *ud, size_t ud_size, unsigned length)
{
	char *at = out->pdu[out->count];
	uint8_t head[] = { 0x00, header ? 0x51 : 0x11, 0x00 };
	hex_put(&at, head, sizeof(head));
	hex_put(&at, addr, addr_size);
	uint8_t tail[] = { 0x00, ucs2 ? 0x08 : 0x00, VALIDITY, (uint8_t)length };
	hex_put(&at, tail, sizeof(tail));
	hex_put(&at, ud, ud_size);
	out->length[out->count] = (unsigned)(strlen(out->pdu[out->count]) / 2 - 1);
	out->count++;
}

bool sms_submit_encode(const char *number, const char *text, unsigned ref, struct sms_submit *out)
{
	memset(out, 0, sizeof(*out));

	/* The destination: its length in digits, its type, the digits in semi-octets */
	uint8_t addr[2 + 10] = { 0 };
	static const char dialed[] = "0123456789*#"; /* a semi-octet's value: its index */
	unsigned digits = 0;
	bool international = false;
	for (const char *s = number; *s; s++) {
		const char *semi = strchr(dialed, *s);
		if (*s == '+' && !digits)
			international = true;
		if (!semi)
			continue;
		if (digits == 20)
			return false;
		unsigned v = (unsigned)(semi - dialed);
		addr[2 + digits / 2] |= digits % 2 ? v << 4 : v;
		digits++;
	}
	if (!digits || !*text)
		return false;
	if (digits % 2)
		addr[2 + digits / 2] |= 0xf0;
	addr[0] = (uint8_t)digits;
	addr[1] = international ? 0x91 : 0x81;
	size_t addr_size = 2 + (digits + 1) / 2;

	/* GSM 7-bit if every character has septets, UCS-2 (UTF-16) otherwise */
	static uint8_t septets[SEPTETS_PART * SMS_PARTS_MAX + 2];
	static uint16_t units[SEPTETS_PART * SMS_PARTS_MAX + 2]; /* as many: GSM 7-bit until a character is not */
	size_t ns = 0, nu = 0;
	bool ucs2 = false;
	for (const char *s = text; *s;) {
		uint32_t c = utf8_next(&s);
		uint8_t sep[2];
		unsigned k = ucs2 ? 0 : gsm_encode(c, sep);
		if (!k)
			ucs2 = true;
		else if (ns + k <= sizeof(septets))
			memcpy(&septets[ns], sep, k), ns += k;
		else
			return false;
		if (nu + 2 > sizeof(units) / sizeof(units[0]))
			return false;
		if (c >= 0x10000) {
			units[nu++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
			units[nu++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
		} else {
			units[nu++] = (uint16_t)c;
		}
	}

	size_t total = ucs2 ? nu : ns, single = ucs2 ? UNITS : SEPTETS, per = ucs2 ? UNITS_PART : SEPTETS_PART;
	bool header = total > single;
	/* The parts' bounds, not splitting an escape or a surrogate pair */
	size_t bounds[SMS_PARTS_MAX + 1] = { 0 };
	unsigned parts = 0;
	for (size_t at = 0; at < total;) {
		if (parts == SMS_PARTS_MAX)
			return false;
		size_t end = at + (header ? per : single);
		if (end >= total) {
			end = total;
		} else if (ucs2 ? units[end - 1] >= 0xd800 && units[end - 1] < 0xdc00 : septets[end - 1] == GSM_ESC) {
			end--;
		}
		bounds[++parts] = at = end;
	}

	for (unsigned p = 0; p < parts; p++) {
		uint8_t ud[140] = { 0 };
		size_t used = 0;
		if (header) {
			uint8_t udh[] = { 5, 0x00, 3, (uint8_t)ref, (uint8_t)parts, (uint8_t)(p + 1) };
			memcpy(ud, udh, sizeof(udh));
			used = sizeof(udh);
		}
		size_t from = bounds[p], n = bounds[p + 1] - from;
		if (ucs2) {
			for (size_t k = 0; k < n; k++) {
				ud[used++] = (uint8_t)(units[from + k] >> 8);
				ud[used++] = (uint8_t)units[from + k];
			}
			part_put(out, addr, addr_size, true, header, ud, used, (unsigned)used);
		} else {
			/* Packed after the header and its fill bits */
			size_t first = (used * 8 + 6) / 7, bit = first * 7;
			for (size_t k = 0; k < n; k++, bit += 7) {
				unsigned s = septets[from + k];
				ud[bit / 8] |= (uint8_t)(s << (bit % 8));
				if (bit % 8 > 1)
					ud[bit / 8 + 1] |= (uint8_t)(s >> (8 - bit % 8));
			}
			part_put(out, addr, addr_size, false, header, ud, (bit + 7) / 8, (unsigned)(first + n));
		}
	}
	return true;
}

/* USSD strings */

bool ussd_encode(const char *text, char *hex, size_t size)
{
	size_t at = 0;
	hex[0] = '\0';
	while (*text) {
		uint32_t c = utf8_next(&text);
		uint16_t units[2] = { (uint16_t)c, 0 };
		unsigned n = 1;
		if (c >= 0x10000) {
			units[0] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
			units[1] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
			n = 2;
		}
		for (unsigned k = 0; k < n; k++) {
			if (at + 4 >= size)
				return false;
			at += (size_t)snprintf(hex + at, size - at, "%04X", units[k]);
		}
	}
	return true;
}

/* UCS-2 by the cell broadcast data coding scheme (3GPP TS 23.038 5): general coding
 * (01xx) with that alphabet, or 0x11, UCS-2 after a language (its first unit) */
static bool ussd_dcs_ucs2(int dcs)
{
	return dcs == 0x11 || ((dcs & 0xc0) == 0x40 && (dcs >> 2 & 3) == 2);
}

void ussd_decode(const char *str, int dcs, bool ucs2, char *out, size_t size)
{
	uint8_t d[600];
	int n = ucs2 || ussd_dcs_ucs2(dcs) ? unhex(str, d, sizeof(d)) : -1;
	if (n >= 0 && n % 2 == 0) {
		size_t skip = !ucs2 && dcs == 0x11 && n >= 2 ? 2 : 0;
		ucs2_decode(d + skip, (size_t)n - skip, out, size);
	} else {
		/* Text: its characters as they came, what is not UTF-8 replaced */
		size_t at = 0;
		out[0] = '\0';
		while (*str) {
			uint32_t c = utf8_next(&str);
			at = utf8_put(out, size, at, c == 0xfffd ? '?' : c);
		}
	}
	/* Line ends: "\n" ("\r\n" in the modem's text) */
	char *w = out;
	for (const char *r = out; *r; r++) {
		if (*r == '\r' && r[1] == '\n')
			continue;
		*w++ = *r == '\r' ? '\n' : *r;
	}
	*w = '\0';
}
