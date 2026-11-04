/* json.c */

#include "test.h"

#include "json.h"

TEST(scalars)
{
	struct json *v;

	CHECK((v = json_parse("null")) && v->kind == JSON_NULL);
	json_free(v);
	CHECK((v = json_parse("true")) && json_bool(v));
	json_free(v);
	CHECK((v = json_parse(" false ")) && v->kind == JSON_BOOL && !json_bool(v));
	json_free(v);
	CHECK((v = json_parse("\"text\"")));
	CHECK_STR(json_str(v, NULL), "text");
	json_free(v);
}

TEST(numbers)
{
	struct json *v = json_parse("[42, -7, 3.75, -0.5, 1e3, 9007199254740993]");
	CHECK(v && v->kind == JSON_ARRAY);
	struct json *n = v->child;
	CHECK_INT(json_int(n, 0), 42);
	CHECK_INT(json_int(n = n->next, 0), -7);
	CHECK_INT(json_int(n = n->next, 0), 3); /* truncated */
	CHECK_DBL(json_num(n, 0), 3.75);
	CHECK_DBL(json_num(n = n->next, 0), -0.5);
	CHECK_DBL(json_num(n = n->next, 0), 1000);
	/* Above 2^53: exact as an integer, not as a double */
	CHECK_INT(json_int(n = n->next, 0), 9007199254740993LL);
	CHECK(!n->next);
	json_free(v);
}

TEST(int64_as_string)
{
	/* TDLib sends int64 values as strings */
	struct json *v = json_parse("{\"id\": \"-1001234567890123\", \"empty\": \"\"}");
	CHECK_INT(json_int(json_get(v, "id"), 0), -1001234567890123LL);
	CHECK_INT(json_int(json_get(v, "empty"), 5), 5);
	json_free(v);
}

TEST(objects)
{
	struct json *v = json_parse(
		"{ \"@type\": \"message\", \"chat\": { \"id\": 12, \"title\": \"Ben\" },\n"
		"  \"list\": [], \"none\": {}, \"ok\": true }");
	CHECK(v && v->kind == JSON_OBJECT);
	CHECK_STR(json_type(v), "message");
	struct json *chat = json_get(v, "chat");
	CHECK_INT(json_int(json_get(chat, "id"), 0), 12);
	CHECK_STR(json_str(json_get(chat, "title"), NULL), "Ben");
	CHECK(json_get(v, "list")->kind == JSON_ARRAY && !json_get(v, "list")->child);
	CHECK(json_get(v, "none")->kind == JSON_OBJECT && !json_get(v, "none")->child);
	CHECK(json_bool(json_get(v, "ok")));
	CHECK(!json_get(v, "missing"));
	CHECK_STR(json_type(chat), "");
	json_free(v);
}

TEST(missing_values_fall_back)
{
	CHECK(!json_get(NULL, "x"));
	CHECK_STR(json_str(NULL, "fb"), "fb");
	CHECK_INT(json_int(NULL, -1), -1);
	CHECK_DBL(json_num(NULL, 2.5), 2.5);
	CHECK(!json_bool(NULL));
	CHECK_STR(json_type(NULL), "");

	/* A value of another kind is missing too */
	struct json *v = json_parse("{\"n\": 1, \"s\": \"x\"}");
	CHECK_STR(json_str(json_get(v, "n"), "fb"), "fb");
	CHECK_DBL(json_num(json_get(v, "s"), 2.5), 2.5);
	CHECK(!json_get(json_get(v, "n"), "x"));
	json_free(v);
}

TEST(string_escapes)
{
	struct json *v = json_parse("\"a\\\"b\\\\c\\/d\\n\\t\\r\\b\\f\"");
	CHECK_STR(json_str(v, NULL), "a\"b\\c/d\n\t\r\b\f");
	json_free(v);
}

TEST(unicode_escapes)
{
	struct json *v = json_parse("[\"\\u0041\", \"\\u00e9\", \"\\u20AC\", \"\\ud83d\\ude00\", \"caf\\u00e9!\"]");
	struct json *s = v->child;
	CHECK_STR(json_str(s, NULL), "A");
	CHECK_STR(json_str(s = s->next, NULL), "\xc3\xa9");          /* é */
	CHECK_STR(json_str(s = s->next, NULL), "\xe2\x82\xac");      /* € */
	CHECK_STR(json_str(s = s->next, NULL), "\xf0\x9f\x98\x80");  /* 😀, a surrogate pair */
	CHECK_STR(json_str(s = s->next, NULL), "caf\xc3\xa9!");
	json_free(v);
}

TEST(utf8_kept)
{
	struct json *v = json_parse("\"Gr\xc3\xbc\xc3\x9f \xf0\x9f\x91\x8b\"");
	CHECK_STR(json_str(v, NULL), "Gr\xc3\xbc\xc3\x9f \xf0\x9f\x91\x8b");
	json_free(v);
}

TEST(invalid)
{
	static const char *const texts[] = {
		"", " ", "{", "[", "[1,]", "[1 2]", "{\"a\" 1}", "{\"a\":}", "{a: 1}",
		"\"open", "\"\\u12\"", "\"\\uzzzz\"", "tru", "nul", "-", "+1", "{\"a\": 1,}",
	};
	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		struct json *v = json_parse(texts[i]);
		if (v)
			TEST_FAIL("\"%s\" parsed", texts[i]);
		json_free(v);
	}
	CHECK(!json_parse(NULL));
}

static char *nested(int depth)
{
	char *text = malloc(2 * depth + 1);
	memset(text, '[', depth);
	memset(text + depth, ']', depth);
	text[2 * depth] = '\0';
	return text;
}

TEST(depth_limit)
{
	char *text = nested(64);
	struct json *v = json_parse(text);
	CHECK(v);
	json_free(v);
	free(text);

	/* Too deep: refused, not a stack overflow */
	text = nested(100000);
	CHECK(!json_parse(text));
	free(text);
}

TEST(escape)
{
	char out[64];
	json_escape(out, sizeof(out), "plain text");
	CHECK_STR(out, "plain text");
	json_escape(out, sizeof(out), "say \"hi\" \\o/");
	CHECK_STR(out, "say \\\"hi\\\" \\\\o/");
	json_escape(out, sizeof(out), "a\nb\tc\x01");
	CHECK_STR(out, "a\\u000ab\\u0009c\\u0001");
	json_escape(out, sizeof(out), "caf\xc3\xa9");
	CHECK_STR(out, "caf\xc3\xa9");
}

TEST(escape_round_trip)
{
	const char *text = "\"quoted\" \\ back\nslash\x1f end \xe2\x82\xac";
	char escaped[256], json[300];
	json_escape(escaped, sizeof(escaped), text);
	snprintf(json, sizeof(json), "\"%s\"", escaped);
	struct json *v = json_parse(json);
	CHECK_STR(json_str(v, NULL), text);
	json_free(v);
}

TEST(escape_cuts_to_size)
{
	char out[8];
	memset(out, 'x', sizeof(out));
	json_escape(out, sizeof(out), "abcdefghijkl");
	CHECK(strlen(out) < sizeof(out));
	CHECK(strncmp(out, "abcdefghijkl", strlen(out)) == 0);
}

/* json.h: 6 * strlen(text) + 1 always fits, nothing cut */
TEST(escape_worst_case_fits)
{
	static const char *const texts[] = { "\"", "\n", "\x01\x02\x03", "a\n", "\n\"\\" };
	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		size_t size = 6 * strlen(texts[i]) + 1;
		char out[64], json[80];
		json_escape(out, size, texts[i]);
		snprintf(json, sizeof(json), "\"%s\"", out);
		struct json *v = json_parse(json);
		if (!v || strcmp(json_str(v, ""), texts[i]) != 0)
			TEST_FAIL("text %zu cut: \"%s\"", i, out);
		json_free(v);
	}
}
