/*
 * Doom: WAD files (plugin.h), read without Doom nor LVGL. A WAD is a header
 * ("IWAD" or "PWAD", the number of lumps, where their directory is), the lumps,
 * and the directory: 16 bytes each (offset, size, 8-byte name).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plugin.h"

/* Names of the games doomgeneric knows; `pwad`: its file says PWAD but it is the
 * whole game (Chex Quest's), Doom takes it by its name (d_iwad.c) */
static const struct {
	const char *file, *name;
	bool pwad;
} known[] = {
	{ "doom1.wad", "Doom (shareware)", false },
	{ "doom.wad", "Doom", false },
	{ "doomu.wad", "The Ultimate Doom", false },
	{ "doom2.wad", "Doom II", false },
	{ "doom2f.wad", "Doom II (French)", false },
	{ "plutonia.wad", "Final Doom: Plutonia", false },
	{ "tnt.wad", "Final Doom: TNT", false },
	{ "freedoom1.wad", "Freedoom: Phase 1", false },
	{ "freedoom2.wad", "Freedoom: Phase 2", false },
	{ "freedm.wad", "FreeDM", false },
	{ "chex.wad", "Chex Quest", true },
	{ "hacx.wad", "Hacx", true },
};

/* IWADs of the other games on Doom's engine, which doomgeneric does not run */
static const char *const other_games[] = { "heretic", "hexen", "hexdd", "strife", "voices" };

const char *wad_known_name(const char *file)
{
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (strcasecmp(file, known[i].file) == 0)
			return known[i].name;
	return NULL;
}

/* A game Doom can run: an IWAD not of another game, or one known by its name */
bool wad_is_game(const char *path, const char *file)
{
	for (unsigned i = 0; i < sizeof(other_games) / sizeof(other_games[0]); i++)
		if (strncasecmp(file, other_games[i], strlen(other_games[i])) == 0)
			return false;
	char magic[4];
	FILE *f = fopen(path, "rb");
	bool read = f && fread(magic, 1, 4, f) == 4;
	if (f)
		fclose(f);
	if (read && memcmp(magic, "IWAD", 4) == 0)
		return true;
	for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
		if (known[i].pwad && strcasecmp(file, known[i].file) == 0)
			return read && memcmp(magic, "PWAD", 4) == 0;
	return false;
}

/* Title screens: TITLEPIC, in the WAD's palette (PLAYPAL's first), as BGRA */


static void *lump_read(FILE *f, int32_t pos, int32_t size)
{
	void *data = size > 0 && size < (1 << 24) ? malloc(size) : NULL;
	if (data && (fseek(f, pos, SEEK_SET) != 0 || fread(data, 1, size, f) != (size_t)size)) {
		free(data);
		data = NULL;
	}
	return data;
}

static int16_t le16(const uint8_t *p)
{
	return (int16_t)(p[0] | p[1] << 8);
}

static int32_t le32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* Malloc'd WAD_PIC_W x WAD_PIC_H BGRA, NULL if the WAD has none. TITLEPIC is a
 * patch (columns of runs of palette indices), or a raw 320 x 200 block in some
 * old WADs. */
uint8_t *wad_title(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t head[12], *dir = NULL, *pal = NULL, *pic = NULL, *out = NULL;
	int32_t pic_size = 0;
	if (fread(head, 1, 12, f) != 12)
		goto done;
	int32_t count = le32(head + 4), table = le32(head + 8);
	if (count <= 0 || count > 65536 || !(dir = lump_read(f, table, count * 16)))
		goto done;
	for (int32_t i = 0; i < count; i++) {
		const uint8_t *e = dir + i * 16;
		if (!pal && strncmp((const char *)e + 8, "PLAYPAL", 8) == 0 && le32(e + 4) >= 768)
			pal = lump_read(f, le32(e), 768);
		else if (!pic && strncmp((const char *)e + 8, "TITLEPIC", 8) == 0)
			pic = lump_read(f, le32(e), pic_size = le32(e + 4));
	}
	if (!pal || !pic)
		goto done;

	out = calloc(WAD_PIC_W * WAD_PIC_H, 4);
	for (int i = 0; i < WAD_PIC_W * WAD_PIC_H; i++)
		out[i * 4 + 3] = 0xff;
	if (pic_size == WAD_PIC_W * WAD_PIC_H) {
		for (int i = 0; i < WAD_PIC_W * WAD_PIC_H; i++)
			memcpy(out + i * 4, (uint8_t[]){ pal[pic[i] * 3 + 2], pal[pic[i] * 3 + 1], pal[pic[i] * 3] }, 3);
		goto done;
	}
	int w = pic_size >= 8 ? le16(pic) : 0, h = pic_size >= 8 ? le16(pic + 2) : 0;
	if (w <= 0 || h <= 0 || 8 + 4 * w > pic_size)
		goto done;
	for (int x = 0; x < w && x < WAD_PIC_W; x++) {
		int32_t p = le32(pic + 8 + 4 * x);
		/* Posts: top, length, a padding byte, the pixels, a padding byte; 0xff ends */
		while (p >= 0 && p + 3 <= pic_size && pic[p] != 0xff) {
			int top = pic[p], len = pic[p + 1];
			const uint8_t *src = pic + p + 3;
			if (p + 3 + len > pic_size)
				break;
			for (int y = 0; y < len; y++) {
				int row = top + y;
				if (row >= WAD_PIC_H)
					break;
				uint8_t *d = out + (row * WAD_PIC_W + x) * 4;
				d[0] = pal[src[y] * 3 + 2];
				d[1] = pal[src[y] * 3 + 1];
				d[2] = pal[src[y] * 3];
			}
			p += len + 4;
		}
	}
done:
	free(dir);
	free(pal);
	free(pic);
	fclose(f);
	return out;
}
