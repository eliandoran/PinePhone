/*
 * Rich text: a document of blocks, each with its text and the formatting of its runs
 * (rich_text.c), which the editor (rich_edit.c) shows and changes. Only paragraphs and
 * inline marks for now; containers (quotes, lists, tables) are to come as blocks with
 * children.
 *
 * Positions are byte offsets into the document's text as one string, its blocks joined by
 * '\n': what a text area holds, so that the editor's edits map onto the document's.
 */

#ifndef RICH_TEXT_H
#define RICH_TEXT_H

#include <stdbool.h>
#include <stdint.h>

/* Inline formatting */
enum {
	RICH_BOLD = 1 << 0,
	RICH_ITALIC = 1 << 1,
	RICH_UNDERLINE = 1 << 2,
};

/* A bullet list is its items one after another: each a block (a line of the text) of its
 * own, as Quill keeps lists, rather than a list block holding them (which nested lists
 * will want) */
enum rich_block_type {
	RICH_PARAGRAPH,
	RICH_BULLET,   /* an item of a bullet list */
	RICH_NUMBERED, /* of a numbered list: those in a row are one list (rich_number()) */
	RICH_TODO,     /* of a to-do list: done or not (`checked`) */
};

/* A stretch of a block's text with the same marks */
struct rich_run {
	uint32_t len; /* bytes */
	unsigned marks;
};

struct rich_block {
	enum rich_block_type type;
	bool checked; /* a to-do item done */
	char *text; /* UTF-8, without '\n', 0-terminated */
	uint32_t len, cap;
	/* Cover the text in order: none empty, neighbors' marks differ */
	struct rich_run *runs;
	uint32_t run_count, run_cap;
};

struct rich_doc {
	struct rich_block *blocks; /* at least one */
	uint32_t count, cap;
};

/* Plain text, a paragraph a line; rich_doc_free() it */
struct rich_doc *rich_doc_new(const char *text);
void rich_doc_free(struct rich_doc *doc);

/* The text as one string, to be freed */
char *rich_doc_text(const struct rich_doc *doc);
uint32_t rich_doc_length(const struct rich_doc *doc);

/* The block at `pos` (past the end: the last's end) and the offset in it */
void rich_locate(const struct rich_doc *doc, uint32_t pos, uint32_t *block, uint32_t *offset);
/* Where block `block` starts */
uint32_t rich_block_pos(const struct rich_doc *doc, uint32_t block);

/* `len` bytes of text at `pos` with `marks`; a '\n' splits the block there (keeping
 * each side's marks, both of its type: the new to-do item not done) */
void rich_insert(struct rich_doc *doc, uint32_t pos, const char *text, uint32_t len, unsigned marks);
/* What is between `start` and `end`; blocks it spans are joined, of the first's type */
void rich_delete(struct rich_doc *doc, uint32_t start, uint32_t end);
/* `marks` added to (`on`) or removed from what is between `start` and `end` */
void rich_set_marks(struct rich_doc *doc, uint32_t start, uint32_t end, unsigned marks, bool on);
/* The marks text typed at `pos` takes: the character's before it, or after it at the
 * start of a block */
unsigned rich_marks_at(const struct rich_doc *doc, uint32_t pos);

/* The blocks from the one at `start` to the one at `end` made `type` (a to-do item not
 * done, if made one) */
void rich_set_type(struct rich_doc *doc, uint32_t start, uint32_t end, enum rich_block_type type);
/* The to-do item at `pos` done or not */
void rich_set_checked(struct rich_doc *doc, uint32_t pos, bool checked);
enum rich_block_type rich_type_at(const struct rich_doc *doc, uint32_t pos);
/* A numbered list's item's number: 1 + the numbered blocks right before it; 0 for
 * other blocks */
uint32_t rich_number(const struct rich_doc *doc, uint32_t block);

/* Markdown (rich_markdown.c, which says what of it): a block a line. Both to be freed. */
char *rich_to_markdown(const struct rich_doc *doc);
struct rich_doc *rich_from_markdown(const char *md);

#endif
