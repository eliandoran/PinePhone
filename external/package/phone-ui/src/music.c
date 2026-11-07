/*
 * Music: MP3 files in /var/lib/music (subdirectories too), listed by album from their
 * tags (libmpg123), read again when pulled down, and a Now Playing screen with the cover embedded in the file.
 * Playing is done by `mpg123 --remote`, a child process driven over a socket:
 * decoding never holds up the UI, and the music goes on after leaving the app.
 * It plays where phone-ui's sound goes (a Bluetooth speaker connected: bt_audio_device()),
 * started again there from where the song was when that changes.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mpg123.h>

#include "bluetooth.h"
#include "i18n.h"
#include "ui.h"

#define MUSIC "/var/lib/music"
#define ROW_H 112
#define TICK_MS 250
#define EQ_BARS 3
#define COVER 520

struct track {
	char *path, *title, *artist, *album; /* album "" if untagged */
	unsigned number;  /* on the album, 0 if unknown */
	long rate;        /* samples per second */
	uint32_t seconds; /* as estimated from the header, then as mpg123 counts while playing */
	uint32_t color;   /* of its album, in place of a cover */
};

static struct {
	struct track *tracks;
	unsigned count;
} library;

/* Playback, kept while the app is closed */
static struct {
	bool started; /* a song was picked */
	bool playing, shuffle, repeat;
	unsigned track;
	uint32_t pos;  /* seconds */
	pid_t pid;     /* mpg123, 0 if not running */
	char output[96]; /* its ALSA device, "" the default */
	bool resume;   /* it ended while playing on Bluetooth: again once the sound moved */
	int fd;        /* its stdin and stdout */
	char in[512];  /* the start of a line from it */
	size_t in_len;
	unsigned samples; /* SAMPLEs asked, not answered yet */
	unsigned stale;   /* of those, asked before a JUMP: their answers are dropped */
	lv_timer_t *timer;
} player = { .fd = -1 };

/* The library screen */
static struct {
	lv_obj_t *scr, *list;
	lv_obj_t **rows; /* per track */
	lv_obj_t *eq, *bars[EQ_BARS]; /* moved to the row playing */
	lv_obj_t *mini, *mini_title, *mini_artist, *mini_play;
} lib;

/* The Now Playing screen */
static struct {
	lv_obj_t *scr;
	lv_obj_t *cover, *record, *hole, *art, *album;
	lv_obj_t *title, *artist;
	lv_obj_t *slider, *elapsed, *remaining;
	lv_obj_t *play, *shuffle, *repeat;
	const char *art_path; /* the file whose cover is shown, or NULL */
	unsigned tex;
} now;

static void now_open(void);
static void player_stop(void);
static void state_update(void);

static void time_set(lv_obj_t *label, const char *sign, uint32_t seconds)
{
	ui_set_text_fmt(label, "%s%u:%02u", sign, seconds / 60, seconds % 60);
}

/* Library */

static const uint32_t album_colors[] = {
	0x0a84ff, 0xbf5af2, 0xff9f0a, 0x30d158, 0xff453a, 0x64d2ff, 0xff375f, 0x5e5ce6,
};

static uint32_t album_color(const char *album)
{
	uint32_t h = 5381;
	while (*album)
		h = h * 33 + (unsigned char)*album++;
	return album_colors[h % (sizeof(album_colors) / sizeof(album_colors[0]))];
}

/* An ID3v2 text (mpg123 makes it UTF-8), NULL if empty */
static char *v2_text(const mpg123_string *s)
{
	return s && s->fill > 1 && s->p[0] ? strdup(s->p) : NULL;
}

/* An ID3v1 field: Latin-1, padded with spaces or zeros */
static char *v1_text(const char *field, size_t size)
{
	size_t n = strnlen(field, size);
	while (n && field[n - 1] == ' ')
		n--;
	if (!n)
		return NULL;
	char *s = malloc(n * 2 + 1), *p = s;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)field[i];
		if (c < 0x80) {
			*p++ = (char)c;
		} else {
			*p++ = (char)(0xc0 | c >> 6);
			*p++ = (char)(0x80 | (c & 0x3f));
		}
	}
	*p = 0;
	return s;
}

/* The file name without its directory and extension, for untagged files */
static char *name_title(const char *path)
{
	const char *base = strrchr(path, '/') + 1;
	const char *dot = strrchr(base, '.');
	return strndup(base, dot ? (size_t)(dot - base) : strlen(base));
}

static bool track_read(mpg123_handle *mh, const char *path, struct track *t)
{
	int channels, encoding;
	memset(t, 0, sizeof(*t));
	if (mpg123_open(mh, path) != MPG123_OK)
		return false;
	if (mpg123_getformat(mh, &t->rate, &channels, &encoding) != MPG123_OK || t->rate <= 0) {
		mpg123_close(mh);
		return false;
	}
	off_t samples = mpg123_length(mh);
	t->seconds = samples > 0 ? (uint32_t)(samples / t->rate) : 0;

	mpg123_id3v1 *v1;
	mpg123_id3v2 *v2;
	if (mpg123_id3(mh, &v1, &v2) == MPG123_OK) {
		if (v2) {
			t->title = v2_text(v2->title);
			t->artist = v2_text(v2->artist);
			t->album = v2_text(v2->album);
			for (size_t i = 0; i < v2->texts; i++)
				if (!memcmp(v2->text[i].id, "TRCK", 4) && v2->text[i].text.fill > 1)
					t->number = (unsigned)atoi(v2->text[i].text.p); /* "3" or "3/10" */
		}
		if (v1) {
			if (!t->title)
				t->title = v1_text(v1->title, sizeof(v1->title));
			if (!t->artist)
				t->artist = v1_text(v1->artist, sizeof(v1->artist));
			if (!t->album)
				t->album = v1_text(v1->album, sizeof(v1->album));
		}
	}
	mpg123_close(mh);

	t->path = strdup(path);
	if (!t->title)
		t->title = name_title(path);
	if (!t->artist)
		t->artist = strdup(_("Unknown artist")); /* the library is read at each opening */
	if (!t->album)
		t->album = strdup("");
	t->color = album_color(t->album);
	return true;
}

static void scan(mpg123_handle *mh, const char *dir, int depth, unsigned *cap)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[PATH_MAX];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 4)
				scan(mh, path, depth + 1, cap);
			continue;
		}
		size_t n = strlen(e->d_name);
		if (!S_ISREG(st.st_mode) || n < 5 || strcasecmp(e->d_name + n - 4, ".mp3"))
			continue;
		if (library.count == *cap) {
			*cap = *cap ? *cap * 2 : 64;
			library.tracks = realloc(library.tracks, *cap * sizeof(*library.tracks));
		}
		if (track_read(mh, path, &library.tracks[library.count]))
			library.count++;
		else
			printf("music: cannot read %s\n", path);
	}
	closedir(d);
}

/* By album (untagged last), then track number, then title */
static int track_cmp(const void *a, const void *b)
{
	const struct track *x = a, *y = b;
	if (!x->album[0] != !y->album[0])
		return x->album[0] ? -1 : 1;
	int c = strcasecmp(x->album, y->album);
	if (c)
		return c;
	if (x->number != y->number)
		return x->number < y->number ? -1 : 1;
	return strcasecmp(x->title, y->title);
}

static void library_free(void)
{
	for (unsigned i = 0; i < library.count; i++) {
		struct track *t = &library.tracks[i];
		free(t->path);
		free(t->title);
		free(t->artist);
		free(t->album);
	}
	free(library.tracks);
	memset(&library, 0, sizeof(library));
}

/* Read again each time the app opens or its list is pulled down (its rows made
 * again then). The song playing is found again by its path. */
static void library_load(void)
{
	uint32_t t0 = lv_tick_get();
	char *playing = player.started ? strdup(library.tracks[player.track].path) : NULL;
	library_free();
	mkdir(MUSIC, 0755);

	mpg123_handle *mh = mpg123_new(NULL, NULL);
	if (mh) {
		mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
		unsigned cap = 0;
		scan(mh, MUSIC, 0, &cap);
		mpg123_delete(mh);
	}
	if (library.count)
		qsort(library.tracks, library.count, sizeof(*library.tracks), track_cmp);
	printf("music: %u songs, read in %u ms\n", library.count, lv_tick_elaps(t0));

	if (playing) {
		unsigned i = 0;
		while (i < library.count && strcmp(library.tracks[i].path, playing))
			i++;
		if (i < library.count)
			player.track = i;
		else
			player_stop();
		free(playing);
	}
}

/* The cover embedded in the file (ID3v2 APIC, JPEG), cut square, as a texture */
static unsigned cover_read(const char *path)
{
	unsigned tex = 0;
	mpg123_handle *mh = mpg123_new(NULL, NULL);
	if (!mh)
		return 0;
	mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_PICTURE, 0);
	long rate;
	int channels, encoding;
	mpg123_id3v1 *v1;
	mpg123_id3v2 *v2;
	if (mpg123_open(mh, path) != MPG123_OK ||
	    mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK ||
	    mpg123_id3(mh, &v1, &v2) != MPG123_OK || !v2)
		goto out;
	/* The front cover, else the first picture */
	mpg123_picture *pic = NULL;
	for (size_t i = 0; i < v2->pictures; i++) {
		mpg123_picture *p = &v2->picture[i];
		if (p->size < 4 || p->data[0] != 0xff || p->data[1] != 0xd8) /* not a JPEG */
			continue;
		if (!pic || p->type == mpg123_id3_pic_front_cover)
			pic = p;
	}
	if (!pic)
		goto out;
	FILE *f = fmemopen(pic->data, pic->size, "rb");
	if (!f)
		goto out;
	int w, h;
	uint8_t *pixels = ui_jpeg_read(f, COVER, COVER, true, &w, &h);
	fclose(f);
	if (!pixels)
		goto out;
	/* The middle square, moved to the start of the buffer row by row */
	int s = w < h ? w : h, x0 = (w - s) / 2, y0 = (h - s) / 2;
	for (int y = 0; y < s; y++)
		memmove(pixels + (size_t)y * s * 4, pixels + ((size_t)(y0 + y) * w + x0) * 4, (size_t)s * 4);
	tex = ui_texture_create(pixels, s, s);
	free(pixels);
out:
	mpg123_close(mh);
	mpg123_delete(mh);
	return tex;
}

/* Showing the state */

static void eq_update(void)
{
	if (!lib.eq)
		return;
	for (int b = 0; b < EQ_BARS; b++)
		lv_obj_set_height(lib.bars[b], player.playing ? 8 + rand() % 25 : 8);
}

/* The bars into the row of the song playing, before its length */
static void eq_move(void)
{
	if (!lib.eq)
		return;
	lv_obj_set_hidden(lib.eq, !player.started);
	if (!player.started)
		return;
	lv_obj_t *row = lib.rows[player.track];
	if (lv_obj_get_parent(lib.eq) != row) {
		lv_obj_set_parent(lib.eq, row);
		lv_obj_move_to_index(lib.eq, (int32_t)lv_obj_get_child_count(row) - 2);
	}
	eq_update();
}

static void position_update(void)
{
	if (!now.scr || !player.started)
		return;
	const struct track *t = &library.tracks[player.track];
	uint32_t s = player.pos < t->seconds ? player.pos : t->seconds;
	/* Not while it is dragged: the labels follow the finger */
	if (!lv_obj_has_state(now.slider, LV_STATE_PRESSED)) {
		lv_slider_set_range(now.slider, 0, t->seconds ? (int32_t)t->seconds : 1);
		lv_slider_set_value(now.slider, (int32_t)s, LV_ANIM_OFF);
		time_set(now.elapsed, "", s);
		time_set(now.remaining, "-", t->seconds - s);
	}
}

static void toggle_style(lv_obj_t *button, bool on)
{
	lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
	                            lv_color_hex(on ? UI_COLOR_ACCENT : UI_COLOR_DIM), 0);
}

static void cover_update(const struct track *t)
{
	if (now.art_path == t->path)
		return;
	now.art_path = t->path;
	ui_texture_delete(now.tex);
	now.tex = cover_read(t->path);
#if LV_USE_3DTEXTURE
	if (now.tex)
		lv_3dtexture_set_src(now.art, now.tex);
	lv_obj_set_hidden(now.art, !now.tex);
#endif
	/* The record in the album's color without one */
	lv_obj_set_hidden(now.record, now.tex);
	lv_obj_set_style_bg_color(now.cover, lv_color_hex(t->color), 0);
	lv_obj_set_style_bg_color(now.hole, lv_color_hex(t->color), 0);
}

static void state_update(void)
{
	const char *play = player.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;

	if (lib.scr) {
		lv_obj_set_hidden(lib.mini, !player.started);
		if (player.started) {
			const struct track *t = &library.tracks[player.track];
			lv_label_set_text_static(lib.mini_title, t->title);
			lv_label_set_text_static(lib.mini_artist, t->artist);
		}
		lv_label_set_text_static(lv_obj_get_child(lib.mini_play, 0), play);
		eq_move();
	}
	if (now.scr && player.started) {
		const struct track *t = &library.tracks[player.track];
		cover_update(t);
		lv_label_set_text_static(now.album, t->album);
		lv_label_set_text_static(now.title, t->title);
		lv_label_set_text_static(now.artist, t->artist);
		lv_label_set_text_static(lv_obj_get_child(now.play, 0), play);
		toggle_style(now.shuffle, player.shuffle);
		toggle_style(now.repeat, player.repeat);
		position_update();
	}
}

/* mpg123 */

static void tick_cb(lv_timer_t *timer);

static void send_cmd(const char *fmt, ...) LV_FORMAT_ATTRIBUTE(1, 2);
static void send_cmd(const char *fmt, ...)
{
	char buf[PATH_MAX + 32];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (player.fd < 0 || n < 0 || n >= (int)sizeof(buf) - 1)
		return;
	buf[n++] = '\n';
	/* A socket, not a pipe: no SIGPIPE if it ended */
	if (send(player.fd, buf, (size_t)n, MSG_NOSIGNAL) < 0)
		printf("music: mpg123: %s\n", strerror(errno));
}

/* Where the sound goes now, "" the default */
static const char *output(void)
{
	const char *device = bt_audio_device();
	return device ? device : "";
}

static bool player_start(void)
{
	if (player.pid)
		return true;
	snprintf(player.output, sizeof(player.output), "%s", output());
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		perror("music: socketpair");
		return false;
	}
	int max_fd = sysconf(_SC_OPEN_MAX); /* phone-ui's files (display, input) closed in it */
	fflush(NULL);
	pid_t parent = getpid(), pid = fork();
	if (pid == 0) {
		/* Ends with phone-ui */
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(1);
		dup2(sv[1], STDIN_FILENO);
		dup2(sv[1], STDOUT_FILENO);
		for (int fd = 3; fd < max_fd; fd++)
			close(fd);
		if (player.output[0])
			execlp("mpg123", "mpg123", "--remote", "-o", "alsa", "-a", player.output, (char *)NULL);
		else
			execlp("mpg123", "mpg123", "--remote", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	if (pid < 0) {
		perror("music: fork");
		close(sv[0]);
		return false;
	}
	player.pid = pid;
	player.fd = sv[0];
	player.in_len = 0;
	player.samples = player.stale = 0;
	fcntl(player.fd, F_SETFL, O_NONBLOCK);
	/* No line for every frame: the position is asked for */
	send_cmd("SILENCE");
	if (!player.timer)
		player.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	return true;
}

/* It ended, or could not start */
static void player_gone(void)
{
	/* A Bluetooth speaker going ends it before bt_audio_device() says so */
	player.resume = player.playing && player.output[0];
	close(player.fd);
	player.fd = -1;
	waitpid(player.pid, NULL, 0);
	player.pid = 0;
	player.playing = false;
	printf("music: mpg123 ended\n");
	state_update();
}

static void player_stop(void)
{
	send_cmd("STOP");
	player.started = player.playing = false;
	player.pos = 0;
}

/* The song from its start, playing or paused */
static void track_load(unsigned track, bool play)
{
	player.resume = false;
	player.started = true;
	player.track = track;
	player.pos = 0;
	player.playing = play;
	if (player_start())
		send_cmd("%s %s", play ? "LOAD" : "LOADPAUSED", library.tracks[track].path);
}

/* The song where it was, playing or paused, on mpg123 started again: where the sound goes
 * changed */
static void player_reroute(void)
{
	uint32_t pos = player.pos;
	if (player.pid) {
		kill(player.pid, SIGTERM);
		close(player.fd);
		player.fd = -1;
		waitpid(player.pid, NULL, 0);
		player.pid = 0;
	}
	printf("music: playing on %s\n", output()[0] ? output() : "the phone");
	if (!player.started)
		return;
	track_load(player.track, player.playing);
	if (pos)
		send_cmd("JUMP %us", pos);
	player.pos = pos;
	state_update();
}

/* The song after this one; false at the end of the list without repeat */
static bool next_track(unsigned *next)
{
	unsigned n = library.count;
	if (player.shuffle && n > 1) {
		*next = (player.track + 1 + (unsigned)rand() % (n - 1)) % n;
		return true;
	}
	*next = player.track + 1 < n ? player.track + 1 : 0;
	return player.track + 1 < n || player.repeat;
}

static void line_cb(const char *line)
{
	long long done, total;
	if (sscanf(line, "@SAMPLE %lld %lld", &done, &total) == 2) {
		if (player.samples)
			player.samples--;
		/* Where it was before a seek: the slider would jump back for a tick */
		if (player.stale) {
			player.stale--;
			return;
		}
		if (!player.started)
			return;
		struct track *t = &library.tracks[player.track];
		uint32_t pos = (uint32_t)(done / t->rate);
		/* Its count, better than the estimate of a file without a VBR header */
		if (total > 0)
			t->seconds = (uint32_t)(total / t->rate);
		if (pos != player.pos) {
			player.pos = pos;
			position_update();
		}
	} else if (!strncmp(line, "@E ", 3)) {
		/* Perhaps a SAMPLE's answer: no stream */
		if (player.samples)
			player.samples--;
		if (player.stale > player.samples)
			player.stale = player.samples;
		printf("music: mpg123: %s\n", line + 3);
	} else if (!strcmp(line, "@P 0")) {
		/* Stopped: by STOP, or the file could not be opened */
		if (player.playing) {
			player.playing = false;
			state_update();
		}
	} else if (!strcmp(line, "@P 3") && player.started) {
		/* The end of the song */
		unsigned next;
		bool go_on = next_track(&next);
		track_load(next, go_on);
		state_update();
	}
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	bool moved = strcmp(output(), player.output) != 0;
	if (!player.pid) {
		if (player.resume && moved) {
			player.playing = true;
			player_reroute();
		}
		return;
	}
	if (moved) {
		player_reroute();
		return;
	}
	for (;;) {
		ssize_t n = read(player.fd, player.in + player.in_len, sizeof(player.in) - 1 - player.in_len);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
			player_gone();
			return;
		}
		if (n < 0)
			break;
		player.in_len += (size_t)n;
		player.in[player.in_len] = 0;
		char *start = player.in, *nl;
		while ((nl = strchr(start, '\n'))) {
			*nl = 0;
			line_cb(start);
			start = nl + 1;
		}
		player.in_len -= (size_t)(start - player.in);
		memmove(player.in, start, player.in_len);
		/* A line longer than the buffer: dropped */
		if (player.in_len == sizeof(player.in) - 1)
			player.in_len = 0;
	}
	if (player.playing) {
		send_cmd("SAMPLE");
		player.samples++;
		eq_update();
	}
}

/* Controls */

static void play_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!player.started)
		return;
	ui_vibrate(UI_VIBRATE_TAP);
	if (!player.pid) {
		/* mpg123 ended (killed, crashed): from where it was */
		uint32_t pos = player.pos;
		track_load(player.track, true);
		if (pos)
			send_cmd("JUMP %us", pos);
		player.pos = pos;
	} else {
		player.playing = !player.playing;
		send_cmd("PAUSE");
	}
	state_update();
}

static void prev_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	/* Into a song: back to its start */
	unsigned track = player.pos >= 3 || player.track == 0 ? player.track : player.track - 1;
	track_load(track, player.playing);
	state_update();
}

static void next_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	/* By hand it always goes on, wrapping around */
	unsigned next;
	next_track(&next);
	track_load(next, player.playing);
	state_update();
}

static void shuffle_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	player.shuffle = !player.shuffle;
	state_update();
}

static void repeat_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	player.repeat = !player.repeat;
	state_update();
}

static void seek_cb(lv_event_t *e)
{
	const struct track *t = &library.tracks[player.track];
	uint32_t s = (uint32_t)lv_slider_get_value(now.slider);
	if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
		player.pos = s;
		send_cmd("JUMP %us", s);
		player.stale = player.samples;
		return;
	}
	time_set(now.elapsed, "", s);
	time_set(now.remaining, "-", t->seconds - s);
}

/* Now Playing */

static void now_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_texture_delete(now.tex);
	memset(&now, 0, sizeof(now));
}

static lv_obj_t *control_create(lv_obj_t *parent, int32_t size, uint32_t color, uint32_t pressed,
                                const char *symbol, const lv_font_t *font, lv_event_cb_t cb)
{
	lv_obj_t *button = ui_button(parent, size, size, color, pressed, symbol, cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(button, 0), font, 0);
	return button;
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t size, uint32_t color)
{
	lv_obj_t *c = ui_box(parent);
	lv_obj_set_size(c, size, size);
	lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
	lv_obj_center(c);
	return c;
}

/* The file's cover, or a record on the album's color: rings are opaque circles,
 * drawn as clears */
static void cover_create(lv_obj_t *parent)
{
	now.cover = ui_box(parent);
	lv_obj_set_size(now.cover, COVER, COVER);
	lv_obj_set_style_radius(now.cover, 32, 0);
	lv_obj_set_style_bg_opa(now.cover, LV_OPA_COVER, 0);

	now.record = circle(now.cover, 400, 0x1c1c1e);
	lv_obj_t *groove = circle(now.record, 300, 0x2c2c2e);
	lv_obj_t *inner = circle(groove, 260, 0x1c1c1e);
	now.hole = circle(inner, 140, 0);
	circle(now.hole, 20, 0x1c1c1e);

#if LV_USE_3DTEXTURE
	now.art = lv_3dtexture_create(now.cover);
	lv_obj_set_size(now.art, COVER, COVER);
	lv_obj_set_clickable(now.art, false);
	lv_obj_set_hidden(now.art, true);
#endif
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = ui_one_line_label(parent, "", font);
	lv_obj_set_width(label, LV_PCT(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

static void now_open(void)
{
	if (now.scr || !player.started)
		return;
	lv_obj_t *body;
	now.scr = ui_app_screen_create(_("Now Playing"), NULL, &body);
	lv_obj_add_event_cb(now.scr, now_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(body, 48, 0);
	lv_obj_set_style_pad_top(body, 32, 0);
	lv_obj_set_scrollable(body, false);

	cover_create(body);

	now.album = centered_label(body, UI_FONT_CAPTION, UI_COLOR_DIM);
	lv_obj_set_style_margin_top(now.album, 24, 0);
	now.title = centered_label(body, UI_FONT_HEADLINE, UI_COLOR_TEXT);
	lv_obj_set_style_margin_top(now.title, 24, 0);
	now.artist = centered_label(body, UI_FONT_BODY, UI_COLOR_ACCENT);
	lv_obj_set_style_margin_top(now.artist, 8, 0);

	now.slider = lv_slider_create(body);
	lv_obj_set_size(now.slider, LV_PCT(100), 10);
	lv_obj_set_style_margin_top(now.slider, 48, 0);
	lv_obj_set_ext_click_area(now.slider, 36);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(now.slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
	lv_obj_set_style_pad_all(now.slider, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(now.slider, 0, LV_PART_KNOB);
	lv_obj_set_style_outline_width(now.slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
	lv_obj_add_event_cb(now.slider, seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(now.slider, seek_cb, LV_EVENT_RELEASED, NULL);

	lv_obj_t *times = ui_box(body);
	lv_obj_set_size(times, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(times, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(times, 16, 0);
	now.elapsed = ui_dim_label(times, "", UI_FONT_CAPTION);
	now.remaining = ui_dim_label(times, "", UI_FONT_CAPTION);

	lv_obj_t *controls = ui_box(body);
	lv_obj_set_size(controls, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_margin_top(controls, 32, 0);
	now.shuffle = control_create(controls, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                             LV_SYMBOL_SHUFFLE, &ui_font_24, shuffle_cb);
	control_create(controls, 104, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_PREV, &ui_font_32, prev_cb);
	now.play = control_create(controls, 136, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
	                          LV_SYMBOL_PLAY, &ui_font_48, play_cb);
	control_create(controls, 104, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_NEXT, &ui_font_32, next_cb);
	now.repeat = control_create(controls, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	                            LV_SYMBOL_LOOP, &ui_font_24, repeat_cb);

	state_update();
	ui_push(now.scr);
}

/* Library screen */

static void lib_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(lib.rows);
	memset(&lib, 0, sizeof(lib));
}

static void row_cb(lv_event_t *e)
{
	unsigned i = (unsigned)(uintptr_t)lv_event_get_user_data(e);
	if (!player.started || i != player.track || !player.pid)
		track_load(i, true);
	else if (!player.playing)
		play_cb(e);
	state_update();
	now_open();
}

static void mini_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	now_open();
}

/* Small cover: a square in the album's color with a note */
static void thumb_create(lv_obj_t *parent, uint32_t color)
{
	lv_obj_t *thumb = ui_box(parent);
	lv_obj_set_size(thumb, 72, 72);
	lv_obj_set_style_radius(thumb, 12, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(thumb, lv_color_hex(color), 0);
	lv_obj_t *note = lv_label_create(thumb);
	lv_label_set_text_static(note, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_font(note, &ui_font_26, 0);
	lv_obj_set_style_text_color(note, lv_color_white(), 0);
	lv_obj_center(note);
}

static void row_create(lv_obj_t *card, unsigned i, bool divider)
{
	const struct track *t = &library.tracks[i];
	lv_obj_t *row = lib.rows[i] = ui_row_create(card, ROW_H, divider, row_cb, (void *)(uintptr_t)i);
	thumb_create(row, t->color);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *title = ui_one_line_label(text, t->title, UI_FONT_TITLE);
	lv_obj_set_width(title, LV_PCT(100));
	lv_obj_t *artist = ui_one_line_label(text, t->artist, UI_FONT_BODY);
	lv_obj_set_width(artist, LV_PCT(100));
	lv_obj_set_style_text_color(artist, lv_color_hex(UI_COLOR_DIM), 0);

	lv_obj_t *len = lv_label_create(row);
	time_set(len, "", t->seconds);
	lv_obj_set_style_text_font(len, UI_FONT_CAPTION, 0);
	lv_obj_set_style_text_color(len, lv_color_hex(UI_COLOR_DIM), 0);
}

/* Bars bouncing in the row of the song playing */
static void eq_create(lv_obj_t *parent)
{
	lib.eq = ui_box(parent);
	lv_obj_set_size(lib.eq, 32, 32);
	lv_obj_set_flex_flow(lib.eq, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(lib.eq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(lib.eq, 4, 0);
	for (int b = 0; b < EQ_BARS; b++) {
		lv_obj_t *bar = lib.bars[b] = ui_box(lib.eq);
		lv_obj_set_size(bar, 6, 8);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_ACCENT), 0);
	}
	lv_obj_set_hidden(lib.eq, true);
}

/* The song playing, under the list: tapping it opens Now Playing */
static void mini_create(lv_obj_t *parent)
{
	lib.mini = ui_bar_create(parent);
	lv_obj_set_style_pad_hor(lib.mini, 24, 0);
	lv_obj_set_style_pad_column(lib.mini, 20, 0);
	lv_obj_set_style_bg_color(lib.mini, lv_color_hex(UI_COLOR_PRESSED), LV_STATE_PRESSED);
	lv_obj_set_clickable(lib.mini, true);
	lv_obj_add_event_cb(lib.mini, mini_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *text = ui_row_text(lib.mini);
	lib.mini_title = ui_one_line_label(text, "", UI_FONT_BODY);
	lv_obj_set_width(lib.mini_title, LV_PCT(100));
	lib.mini_artist = ui_one_line_label(text, "", UI_FONT_CAPTION);
	lv_obj_set_width(lib.mini_artist, LV_PCT(100));
	lv_obj_set_style_text_color(lib.mini_artist, lv_color_hex(UI_COLOR_DIM), 0);

	lib.mini_play = control_create(lib.mini, 80, UI_COLOR_ACCENT, UI_COLOR_ACCENT_PRESSED,
	                               LV_SYMBOL_PLAY, &ui_font_32, play_cb);
	control_create(lib.mini, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED,
	               LV_SYMBOL_NEXT, &ui_font_26, next_cb);
}

/* The library's rows, by album */
static void list_fill(void)
{
	lv_obj_t *list = lib.list;
	lv_obj_clean(list);
	lib.eq = NULL;
	free(lib.rows);
	lib.rows = NULL;

	if (!library.count) {
		ui_empty_label(list, _("No music"));
		char text[96];
		snprintf(text, sizeof(text), _("Copy MP3 files to %s"), MUSIC);
		lv_obj_t *hint = ui_dim_label(list, ui_intern(text), UI_FONT_CAPTION);
		lv_obj_set_width(hint, LV_PCT(100));
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_margin_top(hint, 16, 0);
	}

	/* A section per album: they are sorted */
	lib.rows = calloc(library.count ? library.count : 1, sizeof(*lib.rows));
	lv_obj_t *card = NULL;
	for (unsigned i = 0; i < library.count; i++) {
		const struct track *t = &library.tracks[i];
		if (!card || strcmp(t->album, library.tracks[i - 1].album) != 0) {
			ui_section_label(list, t->album[0] ? t->album : _("Unknown album"));
			card = ui_card_create(list);
		}
		bool last = i + 1 == library.count || strcmp(library.tracks[i + 1].album, t->album) != 0;
		row_create(card, i, !last);
	}
	eq_create(list);
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	library_load();
	list_fill();
	state_update();
	ui_pull_refresh_done(lib.list);
}

void music_open(void)
{
	library_load();

	lv_obj_t *content;
	lib.scr = ui_app_screen_create(_("Music"), NULL, &content);
	lv_obj_add_event_cb(lib.scr, lib_delete_cb, LV_EVENT_DELETE, NULL);

	/* The list scrolls above the bar of the song playing */
	lv_obj_t *list = lib.list = ui_box(content);
	lv_obj_set_width(list, LV_PCT(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(list, 16, 0);
	lv_obj_set_style_pad_bottom(list, 32, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_scrollable(list, true);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
	ui_pull_refresh(list, pull_cb, NULL);
	list_fill();

	mini_create(content);
	state_update();
	ui_push(lib.scr);
}
