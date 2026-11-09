/*
 * Rich text: the document the editor changes (rich_text.h). Each block keeps its text
 * and runs covering it; an edit splits the runs it touches where it starts and ends,
 * changes those between, then joins neighbors left with the same marks.
 */

#include <stdlib.h>
#include <string.h>

#include "rich_text.h"

/* Blocks */

static void text_reserve(struct rich_block *b, uint32_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	b->cap = (b->len + extra + 1) * 2;
	b->text = realloc(b->text, b->cap);
}

static void runs_reserve(struct rich_block *b, uint32_t extra)
{
	if (b->run_count + extra <= b->run_cap)
		return;
	b->run_cap = (b->run_count + extra) * 2;
	b->runs = realloc(b->runs, b->run_cap * sizeof(*b->runs));
}

/* Plain text, one run */
static void block_init(struct rich_block *b, const char *text, uint32_t len)
{
	*b = (struct rich_block){ .type = RICH_PARAGRAPH };
	text_reserve(b, len);
	memcpy(b->text, text, len);
	b->text[len] = '\0';
	b->len = len;
	if (len) {
		runs_reserve(b, 1);
		b->runs[b->run_count++] = (struct rich_run){ len, 0 };
	}
}

static void block_free(struct rich_block *b)
{
	free(b->text);
	free(b->runs);
}

/* Empty runs dropped, neighbors with the same marks joined */
static void runs_normalize(struct rich_block *b)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < b->run_count; i++) {
		if (!b->runs[i].len)
			continue;
		if (n && b->runs[n - 1].marks == b->runs[i].marks)
			b->runs[n - 1].len += b->runs[i].len;
		else
			b->runs[n++] = b->runs[i];
	}
	b->run_count = n;
}

/* A run starting at `offset`, the one there split if needed: its index (run_count at the end) */
static uint32_t run_split(struct rich_block *b, uint32_t offset)
{
	uint32_t at = 0;
	for (uint32_t i = 0; i < b->run_count; i++) {
		if (at == offset)
			return i;
		if (offset < at + b->runs[i].len) {
			runs_reserve(b, 1);
			memmove(&b->runs[i + 2], &b->runs[i + 1], (b->run_count - i - 1) * sizeof(*b->runs));
			b->runs[i + 1] = (struct rich_run){ at + b->runs[i].len - offset, b->runs[i].marks };
			b->runs[i].len = offset - at;
			b->run_count++;
			return i + 1;
		}
		at += b->runs[i].len;
	}
	return b->run_count;
}

static void block_insert(struct rich_block *b, uint32_t offset, const char *text, uint32_t len, unsigned marks)
{
	if (!len)
		return;
	text_reserve(b, len);
	memmove(b->text + offset + len, b->text + offset, b->len - offset + 1);
	memcpy(b->text + offset, text, len);
	b->len += len;

	uint32_t i = run_split(b, offset);
	runs_reserve(b, 1);
	memmove(&b->runs[i + 1], &b->runs[i], (b->run_count - i) * sizeof(*b->runs));
	b->runs[i] = (struct rich_run){ len, marks };
	b->run_count++;
	runs_normalize(b);
}

static void block_delete(struct rich_block *b, uint32_t start, uint32_t end)
{
	if (start >= end)
		return;
	uint32_t i = run_split(b, start), j = run_split(b, end);
	memmove(&b->runs[i], &b->runs[j], (b->run_count - j) * sizeof(*b->runs));
	b->run_count -= j - i;
	memmove(b->text + start, b->text + end, b->len - end + 1);
	b->len -= end - start;
	runs_normalize(b);
}

/* The document's blocks */

static void blocks_reserve(struct rich_doc *doc, uint32_t extra)
{
	if (doc->count + extra <= doc->cap)
		return;
	doc->cap = (doc->count + extra) * 2;
	doc->blocks = realloc(doc->blocks, doc->cap * sizeof(*doc->blocks));
}

/* Block `bi` cut at `offset`: what follows becomes the next block */
static void doc_split(struct rich_doc *doc, uint32_t bi, uint32_t offset)
{
	blocks_reserve(doc, 1);
	memmove(&doc->blocks[bi + 2], &doc->blocks[bi + 1], (doc->count - bi - 1) * sizeof(*doc->blocks));
	doc->count++;
	struct rich_block *b = &doc->blocks[bi], *next = &doc->blocks[bi + 1];
	uint32_t i = run_split(b, offset);
	block_init(next, b->text + offset, b->len - offset);
	next->type = b->type;
	/* Its runs: those from there (block_init() made one, or none for no text) */
	next->run_count = 0;
	runs_reserve(next, b->run_count - i);
	if (b->run_count > i)
		memcpy(next->runs, b->runs + i, (b->run_count - i) * sizeof(*b->runs));
	next->run_count = b->run_count - i;
	b->run_count = i;
	b->len = offset;
	b->text[offset] = '\0';
}

/* Block `bi + 1` appended to block `bi` */
static void doc_join(struct rich_doc *doc, uint32_t bi)
{
	struct rich_block *b = &doc->blocks[bi], *next = &doc->blocks[bi + 1];
	text_reserve(b, next->len);
	memcpy(b->text + b->len, next->text, next->len + 1);
	b->len += next->len;
	runs_reserve(b, next->run_count);
	if (next->run_count)
		memcpy(b->runs + b->run_count, next->runs, next->run_count * sizeof(*b->runs));
	b->run_count += next->run_count;
	runs_normalize(b);
	block_free(next);
	memmove(&doc->blocks[bi + 1], &doc->blocks[bi + 2], (doc->count - bi - 2) * sizeof(*doc->blocks));
	doc->count--;
}

struct rich_doc *rich_doc_new(const char *text)
{
	struct rich_doc *doc = calloc(1, sizeof(*doc));
	for (;;) {
		size_t len = strcspn(text, "\n");
		blocks_reserve(doc, 1);
		block_init(&doc->blocks[doc->count++], text, (uint32_t)len);
		if (!text[len])
			break;
		text += len + 1;
	}
	return doc;
}

void rich_doc_free(struct rich_doc *doc)
{
	if (!doc)
		return;
	for (uint32_t i = 0; i < doc->count; i++)
		block_free(&doc->blocks[i]);
	free(doc->blocks);
	free(doc);
}

uint32_t rich_doc_length(const struct rich_doc *doc)
{
	uint32_t len = doc->count - 1; /* the '\n' between blocks */
	for (uint32_t i = 0; i < doc->count; i++)
		len += doc->blocks[i].len;
	return len;
}

char *rich_doc_text(const struct rich_doc *doc)
{
	char *text = malloc(rich_doc_length(doc) + 1), *o = text;
	for (uint32_t i = 0; i < doc->count; i++) {
		if (i)
			*o++ = '\n';
		memcpy(o, doc->blocks[i].text, doc->blocks[i].len);
		o += doc->blocks[i].len;
	}
	*o = '\0';
	return text;
}

void rich_locate(const struct rich_doc *doc, uint32_t pos, uint32_t *block, uint32_t *offset)
{
	uint32_t i = 0;
	while (i + 1 < doc->count && pos > doc->blocks[i].len) {
		pos -= doc->blocks[i].len + 1;
		i++;
	}
	*block = i;
	*offset = pos < doc->blocks[i].len ? pos : doc->blocks[i].len;
}

uint32_t rich_block_pos(const struct rich_doc *doc, uint32_t block)
{
	uint32_t pos = 0;
	for (uint32_t i = 0; i < block && i < doc->count; i++)
		pos += doc->blocks[i].len + 1;
	return pos;
}

/* Edits */

void rich_insert(struct rich_doc *doc, uint32_t pos, const char *text, uint32_t len, unsigned marks)
{
	uint32_t bi, offset;
	rich_locate(doc, pos, &bi, &offset);
	for (;;) {
		const char *nl = memchr(text, '\n', len);
		uint32_t n = nl ? (uint32_t)(nl - text) : len;
		block_insert(&doc->blocks[bi], offset, text, n, marks);
		if (!nl)
			break;
		doc_split(doc, bi, offset + n);
		bi++;
		offset = 0;
		text += n + 1;
		len -= n + 1;
	}
}

void rich_delete(struct rich_doc *doc, uint32_t start, uint32_t end)
{
	if (start >= end)
		return;
	uint32_t first, first_off, last, last_off;
	rich_locate(doc, start, &first, &first_off);
	rich_locate(doc, end, &last, &last_off);
	if (first == last) {
		block_delete(&doc->blocks[first], first_off, last_off);
		return;
	}
	block_delete(&doc->blocks[first], first_off, doc->blocks[first].len);
	block_delete(&doc->blocks[last], 0, last_off);
	/* Those between go, then the last joins the first */
	for (uint32_t i = first + 1; i < last; i++)
		block_free(&doc->blocks[i]);
	memmove(&doc->blocks[first + 1], &doc->blocks[last], (doc->count - last) * sizeof(*doc->blocks));
	doc->count -= last - first - 1;
	doc_join(doc, first);
}

void rich_set_marks(struct rich_doc *doc, uint32_t start, uint32_t end, unsigned marks, bool on)
{
	uint32_t first, first_off, last, last_off;
	rich_locate(doc, start, &first, &first_off);
	rich_locate(doc, end, &last, &last_off);
	for (uint32_t bi = first; bi <= last; bi++) {
		struct rich_block *b = &doc->blocks[bi];
		uint32_t i = run_split(b, bi == first ? first_off : 0);
		uint32_t j = run_split(b, bi == last ? last_off : b->len);
		for (uint32_t k = i; k < j; k++)
			b->runs[k].marks = on ? b->runs[k].marks | marks : b->runs[k].marks & ~marks;
		runs_normalize(b);
	}
}

unsigned rich_marks_at(const struct rich_doc *doc, uint32_t pos)
{
	uint32_t bi, offset;
	rich_locate(doc, pos, &bi, &offset);
	const struct rich_block *b = &doc->blocks[bi];
	/* The run the character before is in: the first if there is none */
	uint32_t at = 0;
	for (uint32_t i = 0; i < b->run_count; i++) {
		at += b->runs[i].len;
		if (offset <= at)
			return b->runs[i].marks;
	}
	return 0;
}
