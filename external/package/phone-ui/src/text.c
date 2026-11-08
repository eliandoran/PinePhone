/*
 * Labels: one copy of each distinct text, never freed, which labels whose text
 * changes (clock, battery, names) share instead of each keeping its own. There are
 * only so many of them (times of day, battery levels...). The GPU draw unit caches
 * a label's texture by its text (LVGL patch 0019), static or not.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* Slot of `text` in an open addressing table: where it is, or the free slot for it */
static size_t intern_slot(char **table, size_t size, const char *text)
{
	size_t h = 2166136261u;
	for (const char *c = text; *c; c++)
		h = (h ^ (unsigned char)*c) * 16777619u;
	for (h &= size - 1; table[h] && strcmp(table[h], text) != 0; h = (h + 1) & (size - 1))
		;
	return h;
}

const char *ui_intern(const char *text)
{
	static char **table;
	static size_t size, count;

	if (count * 2 >= size) {
		size_t old_size = size;
		char **old = table;
		size = size ? size * 2 : 256;
		table = calloc(size, sizeof(*table));
		for (size_t i = 0; i < old_size; i++)
			if (old[i])
				table[intern_slot(table, size, old[i])] = old[i];
		free(old);
	}

	size_t i = intern_slot(table, size, text);
	if (!table[i]) {
		table[i] = strdup(text);
		count++;
	}
	return table[i];
}

/* Setting the same text still redraws the label: only set it when it changed */
void ui_set_text(lv_obj_t *label, const char *text)
{
	const char *s = ui_intern(text);
	if (lv_label_get_text(label) != s)
		lv_label_set_text_static(label, s);
}

void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ui_set_text(label, buf);
}
