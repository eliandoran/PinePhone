/*
 * Small JSON parser for TDLib's updates: a tree of values, freed at once.
 * Integers only (no fractions are needed); TDLib's int64 values come as
 * strings, json_int() reads both.
 */

#ifndef PHONE_UI_JSON_H
#define PHONE_UI_JSON_H

#include <stdbool.h>

enum json_kind { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
	enum json_kind kind;
	char *key;                 /* member of an object */
	char *str;                 /* JSON_STRING */
	long long num;             /* JSON_NUMBER, JSON_BOOL */
	struct json *child, *next; /* JSON_ARRAY, JSON_OBJECT: first element, then siblings */
};

struct json *json_parse(const char *text); /* NULL if invalid */
void json_free(struct json *v);

/* All of these accept NULL (a missing value) and return the fallback */
struct json *json_get(const struct json *obj, const char *key);
const char *json_str(const struct json *v, const char *fallback);
long long json_int(const struct json *v, long long fallback);
bool json_bool(const struct json *v);
const char *json_type(const struct json *obj); /* its "@type", "" if none */

#endif
