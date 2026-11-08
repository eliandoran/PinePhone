/*
 * The emoji images (1906, 6 MB) are fetched from emoji/ beside the page when first drawn,
 * not with phone-ui, with the set's version (emoji_version) for the browser's cache.
 * ../src/ui/emoji.c reads /usr/share/emoji: each name has an empty file there from the start
 * (emoji_names.c, made from the images' directory), so the sequences that have an image are
 * known at once. Reading an empty one (png_image_begin_read_from_file, wrapped) starts its
 * fetch and finds no image yet; once it arrived, emoji_frame() has ui_emoji_reload() draw it.
 */

#include <emscripten.h>
#include <png.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ui.h"

#define EMOJI_DIR "/usr/share/emoji"

extern const char emoji_names[]; /* "1f600\n1f1e6-1f1e8\n..." */
extern const char emoji_version[]; /* the set's, for the browser's cache */

static volatile bool arrived;

EMSCRIPTEN_KEEPALIVE void emoji_arrived(void)
{
	arrived = true;
}

EM_JS_DEPS(emoji, "$UTF8ToString");

/* Once per name: the file replaced by the image when it comes, kept empty if it fails */
EM_JS(void, js_emoji_fetch, (const char *path, const char *name, const char *version), {
	const file = UTF8ToString(path);
	Module.emojiFetched = Module.emojiFetched || new Set();
	if (Module.emojiFetched.has(file))
		return;
	Module.emojiFetched.add(file);
	fetch("emoji/" + UTF8ToString(name) + "?v=" + UTF8ToString(version)).then(res => {
		if (!res.ok)
			throw res.status;
		return res.arrayBuffer();
	}).then(data => {
		FS.writeFile(file, new Uint8Array(data));
		_emoji_arrived();
	}).catch(() => {});
});

int __real_png_image_begin_read_from_file(png_imagep image, const char *path);

int __wrap_png_image_begin_read_from_file(png_imagep image, const char *path)
{
	struct stat st;
	if (!strncmp(path, EMOJI_DIR "/", sizeof(EMOJI_DIR)) && !stat(path, &st) && !st.st_size) {
		js_emoji_fetch(path, path + sizeof(EMOJI_DIR), emoji_version);
		return 0;
	}
	return __real_png_image_begin_read_from_file(image, path);
}

void emoji_start(void)
{
	mkdir("/usr", 0755);
	mkdir("/usr/share", 0755);
	mkdir(EMOJI_DIR, 0755);
	char path[256];
	for (const char *s = emoji_names, *end; *s; s = end + 1) {
		end = strchr(s, '\n');
		snprintf(path, sizeof(path), EMOJI_DIR "/%.*s.png", (int)(end - s), s);
		FILE *f = fopen(path, "w");
		if (f)
			fclose(f);
	}
}

/* Once a frame: those that arrived since, together */
void emoji_frame(void)
{
	if (arrived) {
		arrived = false;
		ui_emoji_reload();
	}
}
