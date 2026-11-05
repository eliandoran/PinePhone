/*
 * The repository's demo files (../../../../demo: books, photos, music) in the phone's
 * /var/lib, fetched from demo/ beside the page once phone-ui started, smallest first, one
 * at a time: the apps show them as they arrive. demo_files.c (the Makefile's) lists them,
 * with their version for the browser's cache.
 */

#include <emscripten.h>

extern const char demo_files[]; /* "books/moby-dick.epub\n..." */
extern const char demo_version[];

EM_JS_DEPS(demo, "$UTF8ToString");

EM_JS(void, js_demo_fetch, (const char *files, const char *version), {
	const v = UTF8ToString(version);
	const paths = UTF8ToString(files).split("\n").filter(p => p);
	(async () => {
		for (const path of paths) {
			try {
				const res = await fetch("demo/" + encodeURI(path) + "?v=" + v);
				if (!res.ok)
					throw new Error(res.status);
				const data = new Uint8Array(await res.arrayBuffer());
				const file = "/var/lib/" + path;
				FS.mkdirTree(file.slice(0, file.lastIndexOf("/")));
				FS.writeFile(file, data);
			} catch (e) {
				console.error("demo: " + path + ": " + e.message);
			}
		}
	})();
});

void demo_start(void)
{
	js_demo_fetch(demo_files, demo_version);
}
