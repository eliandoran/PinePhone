/*
 * Text and formatting shared by the apps: one-line previews, sizes, times.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i18n.h"
#include "ui.h"

char *ui_one_line(const char *prefix, const char *original, size_t max)
{
	char *buf = malloc(max + 1);
	if (!buf)
		return NULL;
	size_t n = snprintf(buf, max + 1, "%s", prefix);
	if (n > max)
		n = max;
	bool space = false;
	/* Emoji sequences first: not cut in the middle */
	char *composed = ui_emoji_compose(original);
	const char *text = composed ? composed : "";
	while (*text) {
		if (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r') {
			space = n > 0;
			text++;
			continue;
		}
		/* Whole characters only */
		unsigned char c = *text;
		size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		if (n + space + len > max || strnlen(text, len) < len)
			break;
		if (space)
			buf[n++] = ' ';
		space = false;
		memcpy(buf + n, text, len);
		n += len;
		text += len;
	}
	buf[n] = '\0';
	free(composed);
	return buf;
}

void ui_size_format(char *buf, size_t size, unsigned long long bytes)
{
	static const char *const units[] = { "kB", "MB", "GB", "TB" };
	if (bytes < 1000) {
		snprintf(buf, size, "%llu B", bytes);
		return;
	}
	double v = bytes / 1000.0;
	unsigned u = 0;
	/* As rounded: 999.6 kB is 1.0 MB, not 1000 kB */
	while (v >= 999.5 && u + 1 < sizeof(units) / sizeof(units[0])) {
		v /= 1000;
		u++;
	}
	snprintf(buf, size, v < 9.95 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

void ui_when_format(char *buf, size_t size, time_t t)
{
	time_t now = time(NULL);
	struct tm tm, today;
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
		strftime(buf, size, "%H:%M", &tm);
	else if (now - t < 6 * 24 * 3600)
		snprintf(buf, size, "%s", ui_day_name(tm.tm_wday, true));
	else if (tm.tm_year == today.tm_year)
		snprintf(buf, size, "%d %s", tm.tm_mday, ui_month_name(tm.tm_mon, true));
	else
		snprintf(buf, size, "%d %s %d", tm.tm_mday, ui_month_name(tm.tm_mon, true), tm.tm_year + 1900);
}

void ui_offset_format(char *buf, size_t size, long seconds)
{
	int off = (int)(seconds / 60 % (24 * 60)); /* minutes */
	char sign = off < 0 ? '-' : '+';
	if (off < 0)
		off = -off;
	if (off == 0)
		snprintf(buf, size, "UTC");
	else if (off % 60)
		snprintf(buf, size, "UTC%c%d:%02d", sign, off / 60, off % 60);
	else
		snprintf(buf, size, "UTC%c%d", sign, off / 60);
}

void ui_tz_localtime(const char *tz, time_t t, struct tm *tm)
{
	const char *cur = getenv("TZ");
	char *saved = cur ? strdup(cur) : NULL;

	setenv("TZ", tz, 1);
	tzset();
	localtime_r(&t, tm);
	if (saved) {
		setenv("TZ", saved, 1);
		free(saved);
	} else {
		unsetenv("TZ");
	}
	tzset();
}
