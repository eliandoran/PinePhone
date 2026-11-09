/* The rich text document (rich_text.c): blocks, runs of marks, edits */

#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "ui/rich_text.h"

/* The document with its marks: a marked run as [biu:text], blocks on lines. Checks the
 * runs' rules on the way: they cover the text, none empty, neighbors' marks differ. */
static const char *dump(const struct rich_doc *doc)
{
	static char buf[1024];
	size_t n = 0;
	buf[0] = '\0';
	for (uint32_t i = 0; i < doc->count; i++) {
		const struct rich_block *b = &doc->blocks[i];
		if (i)
			n += snprintf(buf + n, sizeof(buf) - n, "\n");
		uint32_t at = 0;
		for (uint32_t r = 0; r < b->run_count; r++) {
			const struct rich_run *run = &b->runs[r];
			CHECK(run->len > 0);
			CHECK(r == 0 || run->marks != b->runs[r - 1].marks);
			if (!run->marks) {
				n += snprintf(buf + n, sizeof(buf) - n, "%.*s", (int)run->len, b->text + at);
			} else {
				n += snprintf(buf + n, sizeof(buf) - n, "[%s%s%s:%.*s]", run->marks & RICH_BOLD ? "b" : "",
				              run->marks & RICH_ITALIC ? "i" : "", run->marks & RICH_UNDERLINE ? "u" : "",
				              (int)run->len, b->text + at);
			}
			at += run->len;
		}
		CHECK_INT(at, b->len);
		CHECK_INT(strlen(b->text), b->len);
	}
	return buf;
}

static void check_text(const struct rich_doc *doc, const char *text)
{
	char *t = rich_doc_text(doc);
	CHECK_STR(t, text);
	CHECK_INT(rich_doc_length(doc), strlen(text));
	free(t);
}

static void insert(struct rich_doc *doc, uint32_t pos, const char *text, unsigned marks)
{
	rich_insert(doc, pos, text, (uint32_t)strlen(text), marks);
}

TEST(plain_text_a_paragraph_a_line)
{
	struct rich_doc *doc = rich_doc_new("one\ntwo\n");
	CHECK_INT(doc->count, 3);
	CHECK_STR(dump(doc), "one\ntwo\n");
	check_text(doc, "one\ntwo\n");
	CHECK_INT(doc->blocks[2].len, 0);
	CHECK_INT(doc->blocks[2].run_count, 0);
	rich_doc_free(doc);

	doc = rich_doc_new("");
	CHECK_INT(doc->count, 1);
	check_text(doc, "");
	rich_doc_free(doc);
}

TEST(insert_with_marks)
{
	struct rich_doc *doc = rich_doc_new("Hello world");
	insert(doc, 6, "big ", RICH_BOLD);
	CHECK_STR(dump(doc), "Hello [b:big ]world");
	insert(doc, 0, "Oh ", RICH_ITALIC | RICH_UNDERLINE);
	CHECK_STR(dump(doc), "[iu:Oh ]Hello [b:big ]world");
	insert(doc, rich_doc_length(doc), "!", 0);
	CHECK_STR(dump(doc), "[iu:Oh ]Hello [b:big ]world!");
	rich_doc_free(doc);
}

/* Typed a character at a time: one run */
TEST(typing_extends_the_run)
{
	struct rich_doc *doc = rich_doc_new("");
	insert(doc, 0, "a", RICH_BOLD);
	insert(doc, 1, "b", RICH_BOLD);
	insert(doc, 2, "c", 0);
	insert(doc, 1, "x", RICH_BOLD);
	CHECK_STR(dump(doc), "[b:axb]c");
	CHECK_INT(doc->blocks[0].run_count, 2);
	rich_doc_free(doc);
}

TEST(marks_set_and_cleared)
{
	struct rich_doc *doc = rich_doc_new("abcdef");
	rich_set_marks(doc, 1, 3, RICH_BOLD, true);
	CHECK_STR(dump(doc), "a[b:bc]def");
	rich_set_marks(doc, 2, 5, RICH_BOLD, true);
	CHECK_STR(dump(doc), "a[b:bcde]f");
	rich_set_marks(doc, 3, 6, RICH_ITALIC, true);
	CHECK_STR(dump(doc), "a[b:bc][bi:de][i:f]");
	rich_set_marks(doc, 0, 6, RICH_BOLD, false);
	CHECK_STR(dump(doc), "abc[i:def]");
	rich_set_marks(doc, 0, 6, RICH_ITALIC, false);
	CHECK_STR(dump(doc), "abcdef");
	CHECK_INT(doc->blocks[0].run_count, 1);
	rich_doc_free(doc);
}

TEST(delete_joins_what_is_left)
{
	struct rich_doc *doc = rich_doc_new("abcd");
	rich_set_marks(doc, 1, 3, RICH_BOLD, true);
	rich_delete(doc, 1, 3);
	CHECK_STR(dump(doc), "ad");
	CHECK_INT(doc->blocks[0].run_count, 1);
	rich_delete(doc, 0, 2);
	CHECK_STR(dump(doc), "");
	CHECK_INT(doc->blocks[0].run_count, 0);
	rich_doc_free(doc);
}

TEST(newline_splits_the_block)
{
	struct rich_doc *doc = rich_doc_new("abcd");
	rich_set_marks(doc, 0, 4, RICH_BOLD, true);
	insert(doc, 2, "\n", 0);
	CHECK_STR(dump(doc), "[b:ab]\n[b:cd]");
	insert(doc, 1, "x\ny\n", RICH_ITALIC);
	CHECK_STR(dump(doc), "[b:a][i:x]\n[i:y]\n[b:b]\n[b:cd]");
	check_text(doc, "ax\ny\nb\ncd");
	/* At the start and end of blocks: empty ones */
	insert(doc, 0, "\n", 0);
	insert(doc, rich_doc_length(doc), "\n", 0);
	CHECK_STR(dump(doc), "\n[b:a][i:x]\n[i:y]\n[b:b]\n[b:cd]\n");
	rich_doc_free(doc);
}

TEST(delete_across_blocks)
{
	struct rich_doc *doc = rich_doc_new("ab\ncd\nef");
	rich_set_marks(doc, 3, 5, RICH_BOLD, true);
	/* Backspace at the start of a block: joined to the one before */
	rich_delete(doc, 2, 3);
	CHECK_STR(dump(doc), "ab[b:cd]\nef");
	rich_delete(doc, 1, 6);
	CHECK_STR(dump(doc), "af");
	CHECK_INT(doc->count, 1);
	rich_doc_free(doc);

	doc = rich_doc_new("a\n\n\nb");
	rich_delete(doc, 1, 4);
	CHECK_STR(dump(doc), "ab");
	rich_doc_free(doc);
}

TEST(marks_across_blocks)
{
	struct rich_doc *doc = rich_doc_new("ab\ncd");
	rich_set_marks(doc, 1, 4, RICH_UNDERLINE, true);
	CHECK_STR(dump(doc), "a[u:b]\n[u:c]d");
	rich_doc_free(doc);
}

TEST(marks_typed_text_takes)
{
	struct rich_doc *doc = rich_doc_new("abcd\nxy\n");
	rich_set_marks(doc, 1, 3, RICH_BOLD, true);
	rich_set_marks(doc, 5, 6, RICH_ITALIC, true);
	CHECK_INT(rich_marks_at(doc, 0), 0);
	CHECK_INT(rich_marks_at(doc, 1), 0);         /* after a */
	CHECK_INT(rich_marks_at(doc, 2), RICH_BOLD); /* after b */
	CHECK_INT(rich_marks_at(doc, 3), RICH_BOLD);
	CHECK_INT(rich_marks_at(doc, 4), 0);
	CHECK_INT(rich_marks_at(doc, 5), RICH_ITALIC); /* a block's start: the x after */
	CHECK_INT(rich_marks_at(doc, 8), 0);           /* the empty last block */
	CHECK_INT(rich_marks_at(doc, 100), 0);
	rich_doc_free(doc);
}

TEST(positions)
{
	struct rich_doc *doc = rich_doc_new("ab\n\ncde");
	uint32_t block, offset;
	rich_locate(doc, 2, &block, &offset);
	CHECK_INT(block, 0);
	CHECK_INT(offset, 2);
	rich_locate(doc, 3, &block, &offset);
	CHECK_INT(block, 1);
	CHECK_INT(offset, 0);
	rich_locate(doc, 5, &block, &offset);
	CHECK_INT(block, 2);
	CHECK_INT(offset, 1);
	rich_locate(doc, 100, &block, &offset);
	CHECK_INT(block, 2);
	CHECK_INT(offset, 3);
	CHECK_INT(rich_block_pos(doc, 2), 4);
	rich_doc_free(doc);
}

/* Characters of several bytes: offsets in bytes */
TEST(utf8)
{
	struct rich_doc *doc = rich_doc_new("\xc8\x99i");
	insert(doc, 2, "\xc4\x83", RICH_BOLD);
	CHECK_STR(dump(doc), "\xc8\x99[b:\xc4\x83]i");
	CHECK_INT(rich_doc_length(doc), 5);
	rich_doc_free(doc);
}
