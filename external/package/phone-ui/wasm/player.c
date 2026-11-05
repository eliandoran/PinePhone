/*
 * `mpg123 --remote` in the browser: music.c starts it as a child process and drives it
 * over a socket pair (sockets.c makes both, and a pretend process). Its commands are
 * played here with the page's <audio>, the file from Emscripten's memory as a blob, and
 * answered as mpg123 1.33 does, with the lines music.c reads:
 *
 *	LOAD <path>, LOADPAUSED <path>   the song, playing or paused at its start
 *	PAUSE                            paused, or playing again
 *	STOP                             "@P 0"
 *	JUMP <n>s                        at second n
 *	SAMPLE                           "@SAMPLE <done> <total>" in samples, "@P 3" once ended
 *	SILENCE                          nothing to answer (it only stops mpg123's frame lines)
 *
 * The sample rate is the file's, as libmpg123 reads it (the counts are in its samples).
 */

#include <emscripten.h>
#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sockets.h"

static long rate = 44100;

EM_JS_DEPS(player, "$UTF8ToString");

/* The song, a new <audio> for it; false if the file is not there */
EM_JS(bool, js_player_load, (const char *path, bool play), {
	const p = Module.player = Module.player || {};
	if (p.audio) {
		p.audio.pause();
		URL.revokeObjectURL(p.audio.src);
	}
	let data;
	try {
		data = FS.readFile(UTF8ToString(path));
	} catch (e) {
		p.audio = null;
		return false;
	}
	p.audio = new Audio(URL.createObjectURL(new Blob([data], { type: "audio/mpeg" })));
	if (play)
		p.audio.play().catch(e => console.error("music:", e.message));
	return true;
});

EM_JS(void, js_player_pause, (void), {
	const a = Module.player && Module.player.audio;
	if (a && a.paused)
		a.play().catch(e => console.error("music:", e.message));
	else if (a)
		a.pause();
});

EM_JS(void, js_player_stop, (void), {
	const a = Module.player && Module.player.audio;
	if (a) {
		a.pause();
		a.currentTime = 0;
	}
});

EM_JS(void, js_player_jump, (double seconds), {
	const a = Module.player && Module.player.audio;
	if (a)
		a.currentTime = seconds;
});

/* Where it is and how long it is, in seconds; false once it ended */
EM_JS(bool, js_player_time, (double *pos, double *length), {
	const a = Module.player && Module.player.audio;
	if (!a)
		return true;
	HEAPF64[pos >> 3] = a.currentTime;
	HEAPF64[length >> 3] = isFinite(a.duration) ? a.duration : 0;
	return !a.ended;
});

/* The file's sample rate, for the counts music.c divides by it */
static void rate_read(const char *path)
{
	mpg123_handle *mh = mpg123_new(NULL, NULL);
	int channels, encoding;
	if (!mh)
		return;
	mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	if (mpg123_open(mh, path) != MPG123_OK || mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK)
		rate = 44100;
	mpg123_close(mh);
	mpg123_delete(mh);
}

void player_command(struct fake_socket *s, const char *line)
{
	char text[64];
	if (!strncmp(line, "LOAD ", 5) || !strncmp(line, "LOADPAUSED ", 11)) {
		const char *path = strchr(line, ' ') + 1;
		rate_read(path);
		if (!js_player_load(path, line[4] == ' ')) {
			fake_push(s, "@E No such file\n");
			fake_push(s, "@P 0\n");
		}
	} else if (!strcmp(line, "PAUSE")) {
		js_player_pause();
	} else if (!strcmp(line, "STOP")) {
		js_player_stop();
		fake_push(s, "@P 0\n");
	} else if (!strncmp(line, "JUMP ", 5)) {
		js_player_jump(atof(line + 5));
	} else if (!strcmp(line, "SAMPLE")) {
		double pos = 0, length = 0;
		if (!js_player_time(&pos, &length)) {
			fake_push(s, "@P 3\n");
			return;
		}
		snprintf(text, sizeof(text), "@SAMPLE %lld %lld\n", (long long)(pos * rate), (long long)(length * rate));
		fake_push(s, text);
	}
}
