/*
 * ePub files: the zip's central directory (stored and deflated entries, zlib),
 * META-INF/container.xml to the OPF package, its title, author and spine (the
 * chapters in reading order), and the table of contents from the EPUB 3
 * navigation document or else the EPUB 2 NCX. The XML is scanned for the few
 * elements needed, by local name (any namespace prefix), not parsed.
 */

#define _GNU_SOURCE /* strndup, memmem */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "books.h"

#define ENTRY_MAX (64 << 20) /* a file of the book, unpacked */
#define EOCD_SEARCH (65535 + 22)

/* Zip */

static uint32_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool zip_read_directory(struct epub *book)
{
	if (fseek(book->f, 0, SEEK_END))
		return false;
	long size = ftell(book->f);
	long tail = size < EOCD_SEARCH ? size : EOCD_SEARCH;
	if (tail < 22)
		return false;
	uint8_t *buf = malloc(tail);
	if (!buf || fseek(book->f, size - tail, SEEK_SET) || fread(buf, 1, tail, book->f) != (size_t)tail) {
		free(buf);
		return false;
	}
	/* The end of central directory record, the last one */
	long eocd = -1;
	for (long i = tail - 22; i >= 0 && eocd < 0; i--)
		if (le32(buf + i) == 0x06054b50)
			eocd = i;
	uint32_t count = eocd >= 0 ? le16(buf + eocd + 10) : 0;
	uint32_t dir_size = eocd >= 0 ? le32(buf + eocd + 12) : 0;
	uint32_t dir_offset = eocd >= 0 ? le32(buf + eocd + 16) : 0;
	free(buf);
	if (!count || (long)dir_offset + dir_size > size)
		return false;

	uint8_t *dir = malloc(dir_size);
	book->entries = calloc(count, sizeof(*book->entries));
	if (!dir || !book->entries || fseek(book->f, dir_offset, SEEK_SET) ||
	    fread(dir, 1, dir_size, book->f) != dir_size) {
		free(dir);
		return false;
	}
	uint32_t pos = 0;
	for (uint32_t i = 0; i < count && pos + 46 <= dir_size; i++) {
		const uint8_t *e = dir + pos;
		if (le32(e) != 0x02014b50)
			break;
		uint32_t name_len = le16(e + 28);
		uint32_t next = pos + 46 + name_len + le16(e + 30) + le16(e + 32);
		if (next > dir_size)
			break;
		struct epub_entry *entry = &book->entries[book->entry_count++];
		entry->name = strndup((const char *)e + 46, name_len);
		entry->method = le16(e + 10);
		entry->csize = le32(e + 20);
		entry->usize = le32(e + 24);
		entry->offset = le32(e + 42);
		pos = next;
	}
	free(dir);
	return book->entry_count > 0;
}

static const struct epub_entry *zip_find(const struct epub *book, const char *name)
{
	for (int i = 0; i < book->entry_count; i++)
		if (book->entries[i].name && !strcmp(book->entries[i].name, name))
			return &book->entries[i];
	return NULL;
}

char *epub_read(struct epub *book, const char *name, size_t *size)
{
	const struct epub_entry *e = zip_find(book, name);
	uint8_t local[30];
	if (!e || e->usize > ENTRY_MAX || (e->method != 0 && e->method != 8) ||
	    fseek(book->f, e->offset, SEEK_SET) || fread(local, 1, 30, book->f) != 30 ||
	    le32(local) != 0x04034b50 ||
	    fseek(book->f, (long)e->offset + 30 + le16(local + 26) + le16(local + 28), SEEK_SET))
		return NULL;
	char *in = malloc(e->csize ? e->csize : 1);
	char *out = malloc((size_t)e->usize + 1);
	if (!in || !out || fread(in, 1, e->csize, book->f) != e->csize) {
		free(in);
		free(out);
		return NULL;
	}
	bool ok;
	if (e->method == 0) {
		ok = e->csize == e->usize;
		if (ok)
			memcpy(out, in, e->usize);
	} else {
		z_stream z = { .next_in = (Bytef *)in, .avail_in = e->csize,
		               .next_out = (Bytef *)out, .avail_out = e->usize };
		ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
		if (ok) {
			int r = inflate(&z, Z_FINISH);
			ok = (r == Z_STREAM_END || r == Z_BUF_ERROR) && z.total_out == e->usize;
			inflateEnd(&z);
		}
	}
	free(in);
	if (!ok) {
		printf("books: %s: cannot be unpacked\n", name);
		free(out);
		return NULL;
	}
	out[e->usize] = '\0';
	if (size)
		*size = e->usize;
	return out;
}

/* Paths */

static int hex(int c)
{
	return isdigit(c) ? c - '0' : isxdigit(c) ? tolower(c) - 'a' + 10 : -1;
}

char *epub_path(const char *base, const char *ref, char **fragment)
{
	const char *hash = strchr(ref, '#');
	size_t ref_len = hash ? (size_t)(hash - ref) : strlen(ref);
	if (fragment)
		*fragment = hash && hash[1] ? strdup(hash + 1) : NULL;
	const char *slash = ref_len && ref[0] == '/' ? NULL : strrchr(base, '/');
	size_t dir_len = slash ? (size_t)(slash - base + 1) : 0;
	char *path = malloc(dir_len + ref_len + 1);
	if (!path)
		return NULL;
	memcpy(path, base, dir_len);
	/* An empty reference is the base itself */
	if (!ref_len)
		strcpy(path, base);
	char *o = path + (ref_len ? dir_len : strlen(base));
	for (size_t i = 0; i < ref_len; i++) {
		if (ref[i] == '%' && i + 2 < ref_len && hex(ref[i + 1]) >= 0 && hex(ref[i + 2]) >= 0) {
			*o++ = (char)(hex(ref[i + 1]) << 4 | hex(ref[i + 2]));
			i += 2;
		} else {
			*o++ = ref[i];
		}
	}
	*o = '\0';

	/* Segments, without "" and ".", ".." taking the one before */
	char *out = path;
	o = path;
	for (char *s = path; *s;) {
		char *end = strchrnul(s, '/');
		size_t n = end - s;
		if (n == 2 && !memcmp(s, "..", 2)) {
			if (o > out) {
				o--;
				while (o > out && o[-1] != '/')
					o--;
			}
		} else if (n && !(n == 1 && *s == '.')) {
			memmove(o, s, n);
			o += n;
			if (*end)
				*o++ = '/';
		}
		s = *end ? end + 1 : end;
	}
	if (o > out && o[-1] == '/')
		o--;
	*o = '\0';
	return out;
}

/* XML */

/* The next start tag named `name` (local name, any prefix) at or after s, NULL if
 * none before `end` (NULL: the end of the text) */
static const char *xml_find(const char *s, const char *end, const char *name)
{
	size_t n = strlen(name);
	while ((s = strchr(s, '<')) && (!end || s < end)) {
		const char *t = ++s;
		if (*t == '/' || *t == '!' || *t == '?')
			continue;
		while (*t && !isspace((unsigned char)*t) && *t != '>' && *t != '/') {
			const char *colon = t;
			while (*colon && *colon != ':' && !isspace((unsigned char)*colon) && *colon != '>' &&
			       *colon != '/')
				colon++;
			if (*colon != ':')
				break;
			t = colon + 1;
		}
		if (!strncmp(t, name, n) && (isspace((unsigned char)t[n]) || t[n] == '>' || t[n] == '/'))
			return s - 1;
	}
	return NULL;
}

/* Where the element whose start tag is at `tag` ends: its end tag (or the end of
 * its start tag if it is empty), the end of the text if there is none */
static const char *xml_end(const char *tag, const char *name)
{
	const char *gt = strchr(tag, '>');
	if (!gt)
		return tag + strlen(tag);
	if (gt[-1] == '/')
		return gt + 1;
	size_t n = strlen(name);
	for (const char *s = gt; (s = strstr(s, "</"));) {
		s += 2;
		const char *t = s, *colon = strchr(s, ':'), *close = strchr(s, '>');
		if (colon && close && colon < close)
			t = colon + 1;
		if (!strncmp(t, name, n) && (t[n] == '>' || isspace((unsigned char)t[n])))
			return s - 2;
	}
	return tag + strlen(tag);
}

static void utf8_put(char **o, uint32_t c)
{
	char *p = *o;
	if (c < 0x80) {
		*p++ = (char)c;
	} else if (c < 0x800) {
		*p++ = (char)(0xc0 | c >> 6);
		*p++ = (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		*p++ = (char)(0xe0 | c >> 12);
		*p++ = (char)(0x80 | (c >> 6 & 0x3f));
		*p++ = (char)(0x80 | (c & 0x3f));
	} else {
		*p++ = (char)(0xf0 | c >> 18);
		*p++ = (char)(0x80 | (c >> 12 & 0x3f));
		*p++ = (char)(0x80 | (c >> 6 & 0x3f));
		*p++ = (char)(0x80 | (c & 0x3f));
	}
	*o = p;
}

/* Text from s to end without its tags, entities decoded, white space collapsed
 * and trimmed (malloc'd) */
static char *xml_text(const char *s, const char *end)
{
	char *out = malloc(end - s + 1), *o = out;
	if (!out)
		return NULL;
	bool space = false;
	while (s < end) {
		if (*s == '<') {
			const char *gt = memchr(s, '>', end - s);
			s = gt ? gt + 1 : end;
			continue;
		}
		if (isspace((unsigned char)*s)) {
			space = o > out;
			s++;
			continue;
		}
		if (space)
			*o++ = ' ';
		space = false;
		const char *semi = *s == '&' ? memchr(s, ';', end - s < 12 ? end - s : 12) : NULL;
		if (semi) {
			static const struct { const char *name; char c; } named[] = {
				{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
			};
			size_t n = semi - s - 1;
			bool done = false;
			if (s[1] == '#') {
				uint32_t c = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
				if (c && c < 0x110000) {
					utf8_put(&o, c);
					done = true;
				}
			}
			for (size_t i = 0; i < sizeof(named) / sizeof(named[0]) && !done; i++) {
				if (strlen(named[i].name) == n && !memcmp(s + 1, named[i].name, n)) {
					*o++ = named[i].c;
					done = true;
				}
			}
			if (done) {
				s = semi + 1;
				continue;
			}
		}
		*o++ = *s++;
	}
	*o = '\0';
	return out;
}

/* The value of attribute `name` (local name) in the start tag at `tag`, entities
 * decoded (malloc'd), NULL if it has none */
static char *xml_attr(const char *tag, const char *name)
{
	const char *gt = strchr(tag, '>');
	size_t n = strlen(name);
	for (const char *s = tag + 1; s && (!gt || s < gt); s++) {
		if (!isspace((unsigned char)s[-1]) && s[-1] != ':')
			continue;
		if (strncmp(s, name, n) || (s[n] != '=' && !isspace((unsigned char)s[n])))
			continue;
		if (s[-1] == ':') {
			/* xlink:href and the like, not epub:type for "type" in another attribute */
			const char *p = s - 1;
			while (p > tag && !isspace((unsigned char)p[-1]))
				p--;
			if (memchr(p, '=', s - p))
				continue;
		}
		const char *v = s + n;
		while (isspace((unsigned char)*v))
			v++;
		if (*v++ != '=')
			continue;
		while (isspace((unsigned char)*v))
			v++;
		char quote = *v;
		if (quote != '"' && quote != '\'')
			continue;
		const char *close = strchr(v + 1, quote);
		if (!close)
			return NULL;
		return xml_text(v + 1, close);
	}
	return NULL;
}

/* The text of the first element named `name`, NULL if there is none */
static char *xml_element_text(const char *s, const char *name)
{
	const char *tag = xml_find(s, NULL, name);
	if (!tag)
		return NULL;
	const char *gt = strchr(tag, '>');
	const char *end = xml_end(tag, name);
	return gt && gt < end ? xml_text(gt + 1, end) : NULL;
}

/* Package */

static void toc_add(struct epub *book, char *label, const char *base, const char *href)
{
	char *fragment = NULL;
	char *path = href ? epub_path(base, href, &fragment) : NULL;
	int spine = path ? epub_spine_find(book, path) : -1;
	free(path);
	if (spine < 0 || !label || !*label) {
		free(label);
		free(fragment);
		return;
	}
	struct epub_toc *toc = realloc(book->toc, (book->toc_count + 1) * sizeof(*toc));
	if (!toc) {
		free(label);
		free(fragment);
		return;
	}
	book->toc = toc;
	toc[book->toc_count++] = (struct epub_toc){ label, spine, fragment };
}

/* EPUB 3: the <nav epub:type="toc"> of the navigation document, its links */
static void toc_from_nav(struct epub *book, const char *path)
{
	char *xml = epub_read(book, path, NULL);
	if (!xml)
		return;
	const char *nav = NULL;
	for (const char *s = xml; !nav && (s = xml_find(s, NULL, "nav")); s++) {
		char *type = xml_attr(s, "type");
		if (type && strstr(type, "toc"))
			nav = s;
		free(type);
	}
	if (!nav)
		nav = xml_find(xml, NULL, "nav");
	const char *end = nav ? xml_end(nav, "nav") : NULL;
	for (const char *a = nav; a && (a = xml_find(a + 1, end, "a"));) {
		const char *gt = strchr(a, '>');
		const char *close = xml_end(a, "a");
		char *href = xml_attr(a, "href");
		toc_add(book, gt && gt < close ? xml_text(gt + 1, close) : NULL, path, href);
		free(href);
	}
	free(xml);
}

/* EPUB 2: the NCX's navPoints, nested ones in document order */
static void toc_from_ncx(struct epub *book, const char *path)
{
	char *xml = epub_read(book, path, NULL);
	if (!xml)
		return;
	for (const char *p = xml; (p = xml_find(p + 1, NULL, "navPoint"));) {
		const char *label = xml_find(p, NULL, "navLabel");
		const char *content = xml_find(p, NULL, "content");
		if (!label || !content)
			continue;
		char *src = xml_attr(content, "src");
		toc_add(book, xml_element_text(label, "text"), path, src);
		free(src);
	}
	free(xml);
}

int epub_spine_find(const struct epub *book, const char *path)
{
	for (int i = 0; i < book->spine_count; i++)
		if (!strcmp(book->spine[i], path))
			return i;
	return -1;
}

struct item {
	char *id, *path, *type, *properties;
};

static bool package_read(struct epub *book)
{
	char *container = epub_read(book, "META-INF/container.xml", NULL);
	const char *rootfile = container ? xml_find(container, NULL, "rootfile") : NULL;
	char *opf_path = rootfile ? xml_attr(rootfile, "full-path") : NULL;
	free(container);
	char *opf = opf_path ? epub_read(book, opf_path, NULL) : NULL;
	if (!opf) {
		free(opf_path);
		return false;
	}

	book->title = xml_element_text(opf, "title");
	book->author = xml_element_text(opf, "creator");

	/* EPUB 2's cover: <meta name="cover" content="the image's id"> */
	char *cover_id = NULL;
	const char *metadata = xml_find(opf, NULL, "metadata");
	const char *metadata_end = metadata ? xml_end(metadata, "metadata") : NULL;
	for (const char *s = metadata; s && !cover_id && (s = xml_find(s + 1, metadata_end, "meta"));) {
		char *name = xml_attr(s, "name");
		if (name && !strcmp(name, "cover"))
			cover_id = xml_attr(s, "content");
		free(name);
	}

	struct item *items = NULL;
	int item_count = 0;
	const char *manifest = xml_find(opf, NULL, "manifest");
	const char *manifest_end = manifest ? xml_end(manifest, "manifest") : NULL;
	for (const char *s = manifest; s && (s = xml_find(s + 1, manifest_end, "item"));) {
		struct item *more = realloc(items, (item_count + 1) * sizeof(*items));
		if (!more)
			break;
		items = more;
		char *href = xml_attr(s, "href");
		items[item_count++] = (struct item){
			xml_attr(s, "id"), href ? epub_path(opf_path, href, NULL) : NULL,
			xml_attr(s, "media-type"), xml_attr(s, "properties"),
		};
		free(href);
	}

	const char *spine = xml_find(opf, NULL, "spine");
	const char *spine_end = spine ? xml_end(spine, "spine") : NULL;
	char *ncx_id = spine ? xml_attr(spine, "toc") : NULL;
	for (const char *s = spine; s && (s = xml_find(s + 1, spine_end, "itemref"));) {
		char *idref = xml_attr(s, "idref");
		for (int i = 0; idref && i < item_count; i++) {
			if (!items[i].id || !items[i].path || strcmp(items[i].id, idref))
				continue;
			char **more = realloc(book->spine, (book->spine_count + 1) * sizeof(*more));
			if (more) {
				book->spine = more;
				book->spine[book->spine_count++] = strdup(items[i].path);
			}
			break;
		}
		free(idref);
	}

	/* The navigation document, or else the NCX (the spine's toc, or by its type) */
	const char *nav = NULL, *ncx = NULL;
	for (int i = 0; i < item_count; i++) {
		if (!items[i].path)
			continue;
		if (items[i].properties && strstr(items[i].properties, "nav") && !nav)
			nav = items[i].path;
		if ((ncx_id && items[i].id && !strcmp(items[i].id, ncx_id)) ||
		    (!ncx && items[i].type && !strcmp(items[i].type, "application/x-dtbncx+xml")))
			ncx = items[i].path;
	}
	/* The cover: EPUB 3's cover-image, or else EPUB 2's (an image: not SVG) */
	for (int i = 0; i < item_count; i++) {
		const struct item *it = &items[i];
		if (!it->path || !it->type || strncmp(it->type, "image/", 6) || strstr(it->type, "svg"))
			continue;
		if (it->properties && strstr(it->properties, "cover-image")) {
			free(book->cover);
			book->cover = strdup(it->path);
			break;
		}
		if (cover_id && it->id && !strcmp(it->id, cover_id) && !book->cover)
			book->cover = strdup(it->path);
	}
	free(cover_id);

	if (nav)
		toc_from_nav(book, nav);
	if (!book->toc_count && ncx)
		toc_from_ncx(book, ncx);

	for (int i = 0; i < item_count; i++) {
		free(items[i].id);
		free(items[i].path);
		free(items[i].type);
		free(items[i].properties);
	}
	free(items);
	free(ncx_id);
	free(opf);
	free(opf_path);
	if (!book->title)
		book->title = strdup("");
	if (!book->author)
		book->author = strdup("");
	return book->spine_count > 0 && book->title && book->author;
}

struct epub *epub_open(const char *path)
{
	struct epub *book = calloc(1, sizeof(*book));
	if (!book)
		return NULL;
	book->f = fopen(path, "rb");
	if (!book->f || !zip_read_directory(book) || !package_read(book)) {
		printf("books: %s: not a readable ePub\n", path);
		epub_close(book);
		return NULL;
	}
	return book;
}

void epub_close(struct epub *book)
{
	if (!book)
		return;
	if (book->f)
		fclose(book->f);
	for (int i = 0; i < book->entry_count; i++)
		free(book->entries[i].name);
	free(book->entries);
	for (int i = 0; i < book->spine_count; i++)
		free(book->spine[i]);
	free(book->spine);
	for (int i = 0; i < book->toc_count; i++) {
		free(book->toc[i].label);
		free(book->toc[i].fragment);
	}
	free(book->toc);
	free(book->title);
	free(book->author);
	free(book->cover);
	free(book);
}
