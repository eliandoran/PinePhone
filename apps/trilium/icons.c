/*
 * Trilium: note icons, see trilium.h. As Trilium picks a note's icon (getNoteIcon() in
 * TriliumNext's packages/commons/src/lib/notes.ts): its #iconClass, "bx bx-star", else
 * one for what it is (a text note with children a folder; a code note by its language,
 * mime_type.ts' icons; a file or an image by its MIME type; else its type's). Drawn with
 * Boxicons' font (gen-icons.py), as Trilium does, through LVGL's Tiny TTF at any size.
 * Only Boxicons' classes for now: another pack's (a custom icon pack, a note of its own
 * in the document) is drawn as the note's default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trilium.h"

#define ICON_PX 32 /* as the rows' symbols, ui_font_32 */

/* NON-NLS-BEGIN: Trilium's types, MIME types and icons */

/* NOTE_TYPE_ICONS */
static const char *const type_icons[][2] = {
	{"file", "bx-file"},           {"image", "bx-image"},     {"code", "bx-code"},
	{"render", "bx-extension"},    {"search", "bx-file-find"}, {"relationMap", "bx-shape-polygon"},
	{"book", "bx-book"},           {"noteMap", "bxs-network-chart"}, {"mermaid", "bx-selection"},
	{"canvas", "bx-pen"},          {"webView", "bx-globe-alt"}, {"launcher", "bx-link"},
	{"doc", "bxs-file-doc"},       {"contentWidget", "bxs-widget"}, {"mindMap", "bx-sitemap"},
	{"spreadsheet", "bx-table"},   {"llmChat", "bx-message-square-dots"},
};

/* MIME_TYPES_DICT's icons: a code note's language */
static const char *const code_icons[][2] = {
	{"text/plain", "bx-file"},
	{"application/x-bat", "bx-terminal"},
	{"text/x-c++src", "bxl-c-plus-plus"},
	{"text/coffeescript", "bx-coffee"},
	{"text/css", "bxs-file-css"},
	{"text/x-django", "bxl-django"},
	{"text/x-dockerfile", "bxl-docker"},
	{"application/xml-dtd", "bx-code"},
	{"text/x-gfm", "bxl-markdown"},
	{"text/x-go", "bxl-go-lang"},
	{"text/html", "bxl-html5"},
	{"application/x-jsp", "bxl-java"},
	{"text/x-java", "bxl-java"},
	{"text/javascript", "bxl-javascript"},
	{"application/javascript;env=backend", "bxl-javascript"},
	{"application/javascript;env=frontend", "bxl-javascript"},
	{"application/ld+json", "bxs-file-json"},
	{"application/json", "bxs-file-json"},
	{"text/x-less", "bxl-less"},
	{"text/x-markdown", "bxl-markdown"},
	{"text/x-mssql", "bx-data"},
	{"text/x-mysql", "bx-data"},
	{"text/x-php", "bxl-php"},
	{"text/x-pgsql", "bxl-postgresql"},
	{"application/x-powershell", "bxs-terminal"},
	{"text/x-python", "bxl-python"},
	{"text/x-sass", "bxl-sass"},
	{"text/x-sh", "bx-terminal"},
	{"text/x-sql", "bx-data"},
	{"text/x-sqlite;schema=trilium", "bx-data"},
	{"text/x-sqlite", "bx-data"},
	{"text/x-toml", "bx-bracket"},
	{"text/x-trilium-log", "bx-list-ul"},
	{"text/x-trilium-search", "bx-search"},
	{"application/typescript", "bxl-typescript"},
	{"text/xml", "bx-code-alt"},
};

/* FILE_MIME_MAPPINGS */
static const char *const file_icons[][2] = {
	{"application/pdf", "bxs-file-pdf"},
	{"application/vnd.oasis.opendocument.text", "bxs-file-doc"},
	{"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "bxs-file-doc"},
	{"application/gpx+xml", "bx-trip"},
	{"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "bx-spreadsheet"},
	{"application/vnd.oasis.opendocument.spreadsheet", "bx-spreadsheet"},
	{"application/vnd.ms-excel", "bx-spreadsheet"},
	{"text/csv", "bx-spreadsheet"},
};

#define LOOK_UP(table, key) look_up(table, sizeof(table) / sizeof(table[0]), key)

/* NON-NLS-END */

static const char *look_up(const char *const table[][2], size_t n, const char *key)
{
	for (size_t i = 0; i < n; i++)
		if (!strcmp(table[i][0], key))
			return table[i][1];
	return NULL;
}

static int boxicon_cmp(const void *key, const void *icon)
{
	return strcmp(key, ((const struct tr_boxicon *)icon)->name);
}

/* An icon's character by its name, 0 if Boxicons has none */
static uint32_t boxicon(const char *name)
{
	const struct tr_boxicon *b = bsearch(name, tr_boxicons, tr_boxicons_count, sizeof(*tr_boxicons), boxicon_cmp);
	return b ? b->cp : 0;
}

/* The icon's name in a class, "bx bx-star": Boxicons' only, NULL for another pack's */
static const char *class_icon(const char *icon_class, char *name, size_t size)
{
	char prefix[16];
	if (!icon_class || sscanf(icon_class, "%15s", prefix) != 1 || strcmp(prefix, "bx")) /* NON-NLS */
		return NULL;
	const char *rest = icon_class + strspn(icon_class, " ") + strlen(prefix);
	char format[16];
	snprintf(format, sizeof(format), "%%%zus", size - 1);
	return sscanf(rest, format, name) == 1 ? name : NULL;
}

/* For what the note is, getNoteIcon()'s defaults */
static const char *default_icon(const char *type, const char *mime, bool folder)
{
	/* NON-NLS-BEGIN */
	const char *icon = NULL;
	if (!strcmp(type, "text"))
		return folder ? "bx-folder" : "bx-note";
	if (!strcmp(type, "code"))
		icon = LOOK_UP(code_icons, mime);
	else if (!strcmp(type, "file")) {
		if (!strncmp(mime, "video/", 6))
			return "bx-video";
		if (!strncmp(mime, "audio/", 6))
			return "bx-music";
		if (!strncmp(mime, "font/", 5))
			return "bx-font";
		icon = LOOK_UP(file_icons, mime);
	} else if (!strcmp(type, "image") && !strcmp(mime, "image/gif"))
		return "bxs-file-gif";
	return icon ? icon : LOOK_UP(type_icons, type) ? LOOK_UP(type_icons, type) : "bx-note";
	/* NON-NLS-END */
}

static void utf8(char out[8], uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		out[1] = '\0';
	} else if (cp < 0x800) {
		out[0] = (char)(0xc0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3f));
		out[2] = '\0';
	} else {
		out[0] = (char)(0xe0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		out[3] = '\0';
	}
}

void tr_note_icon(char icon[8], const char *icon_class, const char *type, const char *mime, bool folder)
{
	char name[64];
	const char *own = class_icon(icon_class, name, sizeof(name));
	uint32_t cp = own ? boxicon(own) : 0;
	if (!cp)
		cp = boxicon(default_icon(type ? type : "", mime ? mime : "", folder));
	utf8(icon, cp);
}

const lv_font_t *tr_icon_font_px(int px)
{
	/* The few sizes asked (the rows', the sync indicator's mark), made when first asked */
	static struct {
		int px;
		lv_font_t *font;
	} fonts[4];
	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		if (fonts[i].font && fonts[i].px == px)
			return fonts[i].font;
		if (!fonts[i].font) {
			fonts[i].font = lv_tiny_ttf_create_data_ex(tr_boxicons_ttf, tr_boxicons_ttf_size, px,
			                                           LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
			fonts[i].px = px;
			if (fonts[i].font)
				return fonts[i].font;
		}
	}
	return &ui_font_32;
}

const lv_font_t *tr_icon_font(void)
{
	return tr_icon_font_px(ICON_PX);
}

void tr_icon(char icon[8], const char *name)
{
	utf8(icon, boxicon(name));
}
