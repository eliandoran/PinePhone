/*
 * JSON parser, see json.h. Recursive descent; each value is one allocation.
 */

#include <stdlib.h>
#include <string.h>

#include "json.h"

#define MAX_DEPTH 64

static void skip_ws(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')
		(*p)++;
}

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
		else
			return -1;
	}
	return v;
}

static size_t utf8_put(char *out, unsigned c)
{
	if (c < 0x80) {
		out[0] = c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = 0xc0 | c >> 6;
		out[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0x10000) {
		out[0] = 0xe0 | c >> 12;
		out[1] = 0x80 | (c >> 6 & 0x3f);
		out[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | c >> 18;
	out[1] = 0x80 | (c >> 12 & 0x3f);
	out[2] = 0x80 | (c >> 6 & 0x3f);
	out[3] = 0x80 | (c & 0x3f);
	return 4;
}

/* At the opening quote; the unescaped text (never longer than the escaped one) */
static char *parse_string(const char **p)
{
	const char *s = *p + 1, *end = s;
	while (*end && *end != '"')
		end += *end == '\\' && end[1] ? 2 : 1;
	if (*end != '"')
		return NULL;

	char *out = malloc(end - s + 1), *o = out;
	if (!out)
		return NULL;
	while (s < end) {
		if (*s != '\\') {
			*o++ = *s++;
			continue;
		}
		s++;
		switch (*s++) {
		case 'n': *o++ = '\n'; break;
		case 't': *o++ = '\t'; break;
		case 'r': *o++ = '\r'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'u': {
			int c = end - s >= 4 ? hex4(s) : -1;
			if (c < 0)
				goto fail;
			s += 4;
			/* A surrogate pair: one character */
			if (c >= 0xd800 && c < 0xdc00 && end - s >= 6 && s[0] == '\\' && s[1] == 'u') {
				int low = hex4(s + 2);
				if (low >= 0xdc00 && low < 0xe000) {
					c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
					s += 6;
				}
			}
			/* 6 escaped bytes give at most 4 */
			o += utf8_put(o, c);
			break;
		}
		default: *o++ = s[-1]; break; /* \" \\ \/ */
		}
	}
	*o = '\0';
	*p = end + 1;
	return out;
fail:
	free(out);
	return NULL;
}

static struct json *parse_value(const char **p, int depth)
{
	if (depth > MAX_DEPTH)
		return NULL;
	skip_ws(p);
	struct json *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;

	char c = **p;
	if (c == '{' || c == '[') {
		v->kind = c == '{' ? JSON_OBJECT : JSON_ARRAY;
		char close = c == '{' ? '}' : ']';
		struct json **tail = &v->child;
		(*p)++;
		skip_ws(p);
		if (**p == close) {
			(*p)++;
			return v;
		}
		for (;;) {
			char *key = NULL;
			if (v->kind == JSON_OBJECT) {
				skip_ws(p);
				if (**p != '"' || !(key = parse_string(p)))
					goto fail;
				skip_ws(p);
				if (**p != ':') {
					free(key);
					goto fail;
				}
				(*p)++;
			}
			struct json *item = parse_value(p, depth + 1);
			if (!item) {
				free(key);
				goto fail;
			}
			item->key = key;
			*tail = item;
			tail = &item->next;
			skip_ws(p);
			if (**p == ',') {
				(*p)++;
			} else if (**p == close) {
				(*p)++;
				return v;
			} else {
				goto fail;
			}
		}
	} else if (c == '"') {
		v->kind = JSON_STRING;
		if (!(v->str = parse_string(p)))
			goto fail;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		char *end;
		v->kind = JSON_NUMBER;
		v->num = strtoll(*p, &end, 10);
		/* Fractions and exponents: truncated */
		while (*end == '.' || *end == 'e' || *end == 'E' || *end == '+' || *end == '-' ||
		       (*end >= '0' && *end <= '9'))
			end++;
		*p = end;
	} else if (!strncmp(*p, "true", 4) || !strncmp(*p, "false", 5)) {
		v->kind = JSON_BOOL;
		v->num = c == 't';
		*p += v->num ? 4 : 5;
	} else if (!strncmp(*p, "null", 4)) {
		*p += 4;
	} else {
		goto fail;
	}
	return v;
fail:
	json_free(v);
	return NULL;
}

struct json *json_parse(const char *text)
{
	return text ? parse_value(&text, 0) : NULL;
}

void json_free(struct json *v)
{
	while (v) {
		struct json *next = v->next;
		json_free(v->child);
		free(v->key);
		free(v->str);
		free(v);
		v = next;
	}
}

struct json *json_get(const struct json *obj, const char *key)
{
	if (!obj || obj->kind != JSON_OBJECT)
		return NULL;
	for (struct json *v = obj->child; v; v = v->next)
		if (!strcmp(v->key, key))
			return v;
	return NULL;
}

const char *json_str(const struct json *v, const char *fallback)
{
	return v && v->kind == JSON_STRING ? v->str : fallback;
}

long long json_int(const struct json *v, long long fallback)
{
	if (v && v->kind == JSON_NUMBER)
		return v->num;
	if (v && v->kind == JSON_STRING && v->str[0])
		return strtoll(v->str, NULL, 10);
	return fallback;
}

bool json_bool(const struct json *v)
{
	return v && v->kind == JSON_BOOL && v->num;
}

const char *json_type(const struct json *obj)
{
	return json_str(json_get(obj, "@type"), "");
}
