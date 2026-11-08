/*
 * Labels: one copy of each distinct text, never freed, which labels whose text
 * changes (clock, battery, names) share instead of each keeping its own. There are
 * only so many of them (times of day, battery levels...). The GPU draw unit caches
 * a label's texture by its text (LVGL patch 0019), static or not. Also the case of a
 * letter (ui_char_upper()) and UTF-8 in and out of code points, for the browser's
 * text-transform and the keyboard's autocorrection: the phone has no locales.
 */

#include <ctype.h>
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

/* Case (ui.h): the letters the fonts have */

static uint32_t other_case(uint32_t c, bool upper)
{
	if (c < 0x80)
		return (uint32_t)(upper ? toupper((int)c) : tolower((int)c));
	if (c == 0xFF || c == 0x178) /* ÿ Ÿ */
		return upper ? 0x178 : 0xFF;
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) /* À..Þ, not × */
		return upper ? c : c + 0x20;
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7) /* à..þ, not ÷; ß has no capital here */
		return upper ? c - 0x20 : c;
	/* Pairs: the upper case one first, even then odd (Ā ā), odd then even from Ĺ to ň
	 * and from Ź to ž; İ ı ĸ ſ have no pair here */
	bool pair = (c >= 0x100 && c <= 0x137) || (c >= 0x139 && c <= 0x148) || (c >= 0x14A && c <= 0x177) ||
	            (c >= 0x179 && c <= 0x17E) || (c >= 0x218 && c <= 0x21B);
	if (!pair || c == 0x130 || c == 0x131)
		return c;
	bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
	bool is_upper = ((c & 1) == 1) == odd_upper;
	return upper == is_upper ? c : upper ? c - 1 : c + 1;
}

uint32_t ui_char_upper(uint32_t c)
{
	return other_case(c, true);
}

uint32_t ui_char_lower(uint32_t c)
{
	return other_case(c, false);
}

uint32_t ui_utf8_decode(const char *s, size_t *bytes)
{
	unsigned char b = (unsigned char)s[0];
	size_t len = b == 0 ? 0 : b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
	uint32_t c = len == 1 ? b : len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : len == 4 ? b & 0x07 : 0;
	for (size_t i = 1; i < len; i++) {
		if (((unsigned char)s[i] & 0xC0) != 0x80) { /* cut short: that byte alone */
			*bytes = 1;
			return b;
		}
		c = (c << 6) | ((unsigned char)s[i] & 0x3F);
	}
	*bytes = len;
	return c;
}

size_t ui_utf8_encode(uint32_t c, char *out)
{
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xC0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xE0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		out[2] = (char)(0x80 | (c & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (c >> 18));
	out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
	out[3] = (char)(0x80 | (c & 0x3F));
	return 4;
}
