/*
 * Books internals: epub.c reads ePub files (a zip of XHTML chapters, style sheets
 * and images, described by an OPF package file), books.c is the app (the library
 * and the reader, which lays chapters out with the browser's litehtml page).
 */

#ifndef BOOKS_H
#define BOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BOOKS_DIR "/var/lib/books"

/* A link of the table of contents */
struct epub_toc {
	char *label;
	int spine;      /* the chapter it is in */
	char *fragment; /* where in it (an id), NULL: its start */
};

struct epub_entry {
	char *name;
	uint32_t method, csize, usize, offset; /* offset: of its local header */
};

struct epub {
	FILE *f;
	struct epub_entry *entries;
	int entry_count;
	char *title, *author; /* "" if none */
	char **spine;         /* the chapters in reading order, paths in the zip */
	int spine_count;
	struct epub_toc *toc;
	int toc_count;
};

/* NULL if it is not a zip with an OPF package and at least one chapter */
struct epub *epub_open(const char *path);
void epub_close(struct epub *book);
/* A file of the book, NUL-terminated (malloc'd), NULL if it is not there */
char *epub_read(struct epub *book, const char *name, size_t *size);
/* The chapter at `path`, -1 if it is not one */
int epub_spine_find(const struct epub *book, const char *path);

/* `ref` relative to the file `base` (both paths in the zip: "OEBPS/a.xhtml"),
 * percent-decoded, without "." and "..": malloc'd. The fragment (after '#', still
 * encoded) goes to *fragment if it is not NULL (malloc'd, NULL if none). */
char *epub_path(const char *base, const char *ref, char **fragment);

#endif
