/*
 * Recorder: voice memos from the main microphone, listed newest first to play back or
 * delete. Each memo is a WAV file (48 kHz, 16-bit mono) in /var/lib/memos, named after
 * the time it started. Capture and playback go through alsa-lib without blocking, from
 * a timer: ALSA's buffer holds the sound between ticks.
 */

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui.h"

#define MEMOS "/var/lib/memos"
#define DEVICE "plug:default" /* converts to the codec's format */
#define RATE 48000
#define HEADER 44             /* bytes of our WAV header, the samples follow */
#define CHUNK 2048            /* frames per read or write */
#define TICK_MS 50
#define CAPTURE_US 1000000    /* ALSA's buffer while recording */
#define PLAYBACK_US 500000
#define MIN_FRAMES (RATE / 2) /* shorter recordings are dropped (a tap by mistake) */
#define SETTLE (RATE / 50)    /* frames dropped at the start: the ADC's first ms are garbage */
#define ROW_H 112
#define METER_W 400
#define METER_DB 60           /* the meter's range below full scale */

/* The microphone to AIF1 capture, the CPU's side of the codec. Mic1's playback switch
 * (to the output mixers) stays off: it would play the microphone on the speaker. The
 * microphone bias comes on with the stream (DAPM). */
static const struct {
	const char *name, *value;
} capture_route[] = {
	{ "Mic1 Capture Switch", "on" },                  /* Mic1 into the ADC mixers */
	{ "Mic1 Boost Volume", "1" },                     /* 24 dB */
	{ "ADC Gain Capture Volume", "3" },               /* 0 dB */
	{ "ADC Capture Volume", "160" },                  /* 0 dB */
	{ "AIF1 Data Digital ADC Capture Switch", "on" }, /* the ADC into AIF1, not AIF1's playback */
	{ "AIF1 AD0 Capture Volume", "160" },             /* 0 dB */
};

struct memo {
	char name[32];  /* file name in MEMOS */
	char title[48]; /* when it was recorded */
	uint32_t seconds;
	lv_obj_t *icon, *time;
};

static struct {
	lv_obj_t *list, *controls, *confirm;
	lv_obj_t *elapsed, *meter, *level, *button, *dot;
	lv_timer_t *timer;
	struct memo *memos; /* the rows' labels point into it */
	unsigned count;
	int doomed; /* memo the confirm bar would delete */

	/* Recording, pcm NULL if not */
	struct {
		snd_pcm_t *pcm;
		int fd;
		char path[64];
		uint32_t frames, saved; /* recorded, in the header */
		uint32_t settle;        /* frames still to drop */
		int peak;               /* the loudest sample */
	} cap;

	/* Playing memos[memo], pcm NULL if not */
	struct {
		snd_pcm_t *pcm;
		int fd, memo;
		unsigned rate, frame_bytes;
		uint64_t frames; /* written to ALSA */
		bool eof;
		uint32_t shown;  /* seconds in the row */
	} play;
} rec;

static void list_fill(void);

static void time_fmt(lv_obj_t *label, uint32_t seconds)
{
	ui_set_text_fmt(label, "%u:%02u", seconds / 60, seconds % 60);
}

/* Files */

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | get16(p + 2) << 16;
}

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
	memcpy(h, "RIFF", 4);
	put32(h + 4, 36 + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16);
	put16(h + 20, 1); /* PCM */
	put16(h + 22, 1); /* mono */
	put32(h + 24, RATE);
	put32(h + 28, RATE * 2);
	put16(h + 32, 2);
	put16(h + 34, 16);
	memcpy(h + 36, "data", 4);
	put32(h + 40, data_bytes);
}

static int memo_cmp(const void *a, const void *b)
{
	/* The names are times: newest first */
	return -strcmp(((const struct memo *)a)->name, ((const struct memo *)b)->name);
}

/* Memos newest first; returns how many, *memos is to be freed */
static unsigned memos_load(struct memo **memos)
{
	unsigned count = 0, size = 0;
	*memos = NULL;
	DIR *dir = opendir(MEMOS);
	if (!dir)
		return 0;

	struct dirent *d;
	while ((d = readdir(dir))) {
		size_t len = strlen(d->d_name);
		if (len < 5 || len >= sizeof((*memos)->name) || strcmp(d->d_name + len - 4, ".wav") != 0)
			continue;
		char path[sizeof(MEMOS) + sizeof((*memos)->name)];
		snprintf(path, sizeof(path), MEMOS "/%s", d->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		if (count == size) {
			size = size ? size * 2 : 16;
			*memos = realloc(*memos, size * sizeof(**memos));
		}
		struct memo *m = &(*memos)[count++];
		memset(m, 0, sizeof(*m));
		memcpy(m->name, d->d_name, len + 1);
		m->seconds = st.st_size > HEADER ? (uint32_t)((st.st_size - HEADER) / (RATE * 2)) : 0;

		struct tm tm = { 0 };
		if (sscanf(m->name, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		           &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
			tm.tm_year -= 1900;
			tm.tm_mon--;
			strftime(m->title, sizeof(m->title), "%-d %B %Y, %H:%M", &tm);
		} else {
			snprintf(m->title, sizeof(m->title), "%.*s", (int)(len - 4), m->name);
		}
	}
	closedir(dir);
	if (count) /* none: no array */
		qsort(*memos, count, sizeof(**memos), memo_cmp);
	return count;
}

/* ALSA */

static snd_pcm_t *pcm_open(snd_pcm_stream_t stream, unsigned rate, unsigned channels, unsigned latency_us)
{
	snd_pcm_t *pcm;
	int err = snd_pcm_open(&pcm, DEVICE, stream, SND_PCM_NONBLOCK);
	if (err < 0) {
		printf("recorder: %s: %s\n", DEVICE, snd_strerror(err));
		return NULL;
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
	                         channels, rate, 1, latency_us);
	if (err < 0) {
		printf("recorder: %s: %s\n", DEVICE, snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

static void route_capture(void)
{
	snd_ctl_t *ctl;
	if (snd_ctl_open(&ctl, "hw:0", 0) < 0) {
		printf("recorder: no mixer\n");
		return;
	}
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&value);
	for (unsigned i = 0; i < sizeof(capture_route) / sizeof(capture_route[0]); i++) {
		snd_ctl_elem_id_clear(id);
		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, capture_route[i].name);
		snd_ctl_elem_info_set_id(info, id);
		snd_ctl_elem_value_set_id(value, id);
		/* A single value is set on every channel */
		if (snd_ctl_elem_info(ctl, info) < 0 ||
		    snd_ctl_ascii_value_parse(ctl, value, info, capture_route[i].value) < 0 ||
		    snd_ctl_elem_write(ctl, value) < 0)
			printf("recorder: cannot set '%s'\n", capture_route[i].name);
	}
	snd_ctl_close(ctl);
}

/* Playback */

static void play_stop(void)
{
	if (!rec.play.pcm)
		return;
	snd_pcm_drop(rec.play.pcm);
	snd_pcm_close(rec.play.pcm);
	close(rec.play.fd);
	rec.play.pcm = NULL;
	struct memo *m = &rec.memos[rec.play.memo];
	lv_label_set_text_static(m->icon, LV_SYMBOL_PLAY);
	time_fmt(m->time, m->seconds);
}

static void play_start(int memo)
{
	struct memo *m = &rec.memos[memo];
	char path[sizeof(MEMOS) + sizeof(m->name)];
	snprintf(path, sizeof(path), MEMOS "/%s", m->name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("recorder");
		return;
	}
	/* Our own files: the samples right after the format */
	uint8_t h[HEADER];
	if (read(fd, h, sizeof(h)) != sizeof(h) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) ||
	    get16(h + 20) != 1 || get16(h + 34) != 16 || memcmp(h + 36, "data", 4) ||
	    !get16(h + 22) || get16(h + 22) > 2 || !get32(h + 24)) {
		printf("recorder: %s: not a 16-bit mono or stereo PCM WAV file\n", m->name);
		close(fd);
		return;
	}
	unsigned channels = get16(h + 22);
	rec.play.rate = get32(h + 24);
	/* Stereo: mono through the codec is broken, as for capture */
	rec.play.pcm = pcm_open(SND_PCM_STREAM_PLAYBACK, rec.play.rate, 2, PLAYBACK_US);
	if (!rec.play.pcm) {
		close(fd);
		return;
	}
	rec.play.fd = fd;
	rec.play.memo = memo;
	rec.play.frame_bytes = channels * 2;
	rec.play.frames = 0;
	rec.play.eof = false;
	rec.play.shown = UINT32_MAX;
	lv_label_set_text_static(m->icon, LV_SYMBOL_STOP);
}

static void play_tick(void)
{
	int16_t buf[CHUNK * 2];
	snd_pcm_t *pcm = rec.play.pcm;
	while (!rec.play.eof) {
		snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
		if (avail < 0) {
			/* An underrun: the UI held the loop longer than the buffer */
			printf("recorder: playback: %s\n", snd_strerror((int)avail));
			if (snd_pcm_recover(pcm, (int)avail, 1) < 0) {
				play_stop();
				return;
			}
			continue;
		}
		size_t frames = LV_MIN((size_t)avail, CHUNK);
		if (!frames)
			break;
		ssize_t n = read(rec.play.fd, buf, frames * rec.play.frame_bytes);
		if (n < (ssize_t)rec.play.frame_bytes) {
			rec.play.eof = true;
			break;
		}
		frames = (size_t)n / rec.play.frame_bytes;
		/* Mono on both channels, from the end: in place */
		if (rec.play.frame_bytes == 2)
			for (size_t j = frames; j-- > 0;)
				buf[2 * j] = buf[2 * j + 1] = buf[j];
		snd_pcm_sframes_t written = snd_pcm_writei(pcm, buf, frames);
		if (written < 0) {
			lseek(rec.play.fd, -n, SEEK_CUR);
			if (written != -EAGAIN && snd_pcm_recover(pcm, (int)written, 1) < 0) {
				play_stop();
				return;
			}
			break;
		}
		/* What did not fit, again next time */
		lseek(rec.play.fd, (off_t)written * rec.play.frame_bytes - n, SEEK_CUR);
		rec.play.frames += (uint64_t)written;
	}

	snd_pcm_state_t state = snd_pcm_state(pcm);
	if (rec.play.eof) {
		/* Shorter than the buffer: not started yet */
		if (state == SND_PCM_STATE_PREPARED && snd_pcm_start(pcm) == 0)
			state = SND_PCM_STATE_RUNNING;
		/* Played to the end: it ran out (an underrun stops it) */
		if (state != SND_PCM_STATE_RUNNING) {
			play_stop();
			return;
		}
	}

	snd_pcm_sframes_t delay = 0;
	if (state != SND_PCM_STATE_RUNNING || snd_pcm_delay(pcm, &delay) < 0)
		delay = 0;
	uint64_t played = rec.play.frames > (uint64_t)delay ? rec.play.frames - (uint64_t)delay : 0;
	uint32_t s = (uint32_t)(played / rec.play.rate);
	if (s != rec.play.shown) {
		rec.play.shown = s;
		struct memo *m = &rec.memos[rec.play.memo];
		ui_set_text_fmt(m->time, "%u:%02u / %u:%02u", s / 60, s % 60, m->seconds / 60, m->seconds % 60);
	}
}

/* Recording */

static void header_save(void)
{
	uint8_t h[HEADER];
	wav_header(h, rec.cap.frames * 2);
	if (pwrite(rec.cap.fd, h, sizeof(h), 0) != sizeof(h))
		perror("recorder");
	rec.cap.saved = rec.cap.frames;
}

static void controls_update(void)
{
	bool on = rec.cap.pcm;
	lv_obj_set_hidden(rec.meter, !on);
	lv_obj_set_style_text_color(rec.elapsed, lv_color_hex(on ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);
	if (on)
		time_fmt(rec.elapsed, 0);
	else
		lv_label_set_text_static(rec.elapsed, "Tap to record");
	lv_obj_set_style_text_font(rec.elapsed, on ? &ui_font_48 : &ui_font_26, 0);
	/* A dot to start, a square to stop */
	lv_obj_set_size(rec.dot, on ? 48 : 64, on ? 48 : 64);
	lv_obj_set_style_radius(rec.dot, on ? 10 : LV_RADIUS_CIRCLE, 0);
}

static void record_stop(void)
{
	if (!rec.cap.pcm)
		return;
	snd_pcm_close(rec.cap.pcm);
	rec.cap.pcm = NULL;
	header_save();
	if (fsync(rec.cap.fd) != 0)
		perror("recorder");
	close(rec.cap.fd);
	printf("recorder: %s: %u s, peak %.1f dBFS\n", rec.cap.path, rec.cap.frames / RATE,
	       rec.cap.peak ? 20 * log10(rec.cap.peak / 32768.0) : -INFINITY);
	if (rec.cap.frames < MIN_FRAMES)
		unlink(rec.cap.path);
	controls_update();
	list_fill();
}

static void record_start(void)
{
	play_stop();
	route_capture();
	/* Stereo: the codec gives mono capture with only 8 bits (the low byte 0) */
	rec.cap.pcm = pcm_open(SND_PCM_STREAM_CAPTURE, RATE, 2, CAPTURE_US);
	if (!rec.cap.pcm) {
		lv_label_set_text_static(rec.elapsed, "No microphone");
		return;
	}

	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	mkdir(MEMOS, 0755);
	snprintf(rec.cap.path, sizeof(rec.cap.path), MEMOS "/%s.wav", stamp);
	for (int i = 2; access(rec.cap.path, F_OK) == 0; i++)
		snprintf(rec.cap.path, sizeof(rec.cap.path), MEMOS "/%s-%d.wav", stamp, i);
	rec.cap.fd = open(rec.cap.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (rec.cap.fd < 0) {
		perror("recorder");
		snd_pcm_close(rec.cap.pcm);
		rec.cap.pcm = NULL;
		lv_label_set_text_static(rec.elapsed, "Cannot save");
		return;
	}
	rec.cap.frames = 0;
	rec.cap.peak = 0;
	rec.cap.settle = SETTLE;
	header_save();
	lseek(rec.cap.fd, HEADER, SEEK_SET);
	snd_pcm_start(rec.cap.pcm);
	controls_update();
}

static void record_tick(void)
{
	int16_t buf[CHUNK * 2];
	int peak = 0;
	/* Bounded: an error that recovers every time must not hold the UI */
	for (int i = 0; i < 64; i++) {
		snd_pcm_sframes_t n = snd_pcm_readi(rec.cap.pcm, buf, CHUNK);
		if (n == -EAGAIN)
			break;
		if (n < 0) {
			/* An overrun: the UI held the loop longer than the buffer */
			printf("recorder: capture: %s\n", snd_strerror((int)n));
			if (snd_pcm_recover(rec.cap.pcm, (int)n, 1) < 0) {
				record_stop();
				return;
			}
			if (snd_pcm_state(rec.cap.pcm) == SND_PCM_STATE_PREPARED)
				snd_pcm_start(rec.cap.pcm);
			continue;
		}
		/* Both ADCs have Mic1: their mean, a little less noise */
		for (snd_pcm_sframes_t j = 0; j < n; j++)
			buf[j] = (int16_t)((buf[2 * j] + buf[2 * j + 1]) / 2);
		/* The start, garbage while the ADC settles */
		snd_pcm_sframes_t skip = LV_MIN(n, (snd_pcm_sframes_t)rec.cap.settle);
		rec.cap.settle -= (uint32_t)skip;
		n -= skip;
		memmove(buf, buf + skip, (size_t)n * 2);
		for (snd_pcm_sframes_t j = 0; j < n; j++)
			peak = LV_MAX(peak, abs(buf[j]));
		ssize_t bytes = n * 2;
		if (write(rec.cap.fd, buf, (size_t)bytes) != bytes) {
			perror("recorder");
			record_stop();
			return;
		}
		rec.cap.frames += (uint32_t)n;
	}
	rec.cap.peak = LV_MAX(rec.cap.peak, peak);

	/* Once a second: a power cut keeps a playable file */
	if (rec.cap.frames - rec.cap.saved >= RATE) {
		header_save();
		time_fmt(rec.elapsed, rec.cap.frames / RATE);
	}

	double db = peak ? 20 * log10(peak / 32768.0) : -METER_DB;
	int32_t w = (int32_t)((db + METER_DB) * METER_W / METER_DB);
	lv_obj_set_width(rec.level, LV_CLAMP(12, w, METER_W));
}

/* Screen */

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (rec.cap.pcm)
		record_tick();
	if (rec.play.pcm)
		play_tick();
}

static void record_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
	if (rec.cap.pcm)
		record_stop();
	else
		record_start();
}

static void row_cb(lv_event_t *e)
{
	int memo = (int)(intptr_t)lv_event_get_user_data(e);
	if (rec.cap.pcm)
		return;
	bool same = rec.play.pcm && rec.play.memo == memo;
	play_stop();
	if (!same)
		play_start(memo);
}

static void confirm_show(bool show)
{
	lv_obj_set_hidden(rec.confirm, !show);
	lv_obj_set_hidden(rec.controls, show);
}

static void trash_cb(lv_event_t *e)
{
	if (rec.cap.pcm)
		return;
	rec.doomed = (int)(intptr_t)lv_event_get_user_data(e);
	confirm_show(true);
}

static void cancel_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	confirm_show(false);
}

static void delete_memo_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (rec.play.pcm && rec.play.memo == rec.doomed)
		play_stop();
	char path[sizeof(MEMOS) + sizeof(rec.memos->name)];
	snprintf(path, sizeof(path), MEMOS "/%s", rec.memos[rec.doomed].name);
	if (unlink(path) != 0)
		perror("recorder");
	ui_vibrate(UI_VIBRATE_TAP);
	confirm_show(false);
	list_fill();
}

static void row_create(lv_obj_t *card, unsigned i)
{
	struct memo *m = &rec.memos[i];
	void *user_data = (void *)(intptr_t)i;
	lv_obj_t *row = ui_row_create(card, ROW_H, i + 1 < rec.count, row_cb, user_data);
	lv_obj_set_style_pad_right(row, 0, 0);
	lv_obj_set_style_pad_column(row, 20, 0);

	m->icon = lv_label_create(row);
	lv_label_set_text_static(m->icon, LV_SYMBOL_PLAY);
	lv_obj_set_style_text_font(m->icon, &ui_font_32, 0);
	lv_obj_set_style_text_color(m->icon, lv_color_hex(UI_COLOR_ACCENT), 0);
	lv_obj_set_width(m->icon, 40);

	lv_obj_t *text = ui_row_text(row);
	lv_obj_t *title = ui_one_line_label(text, m->title, &ui_font_26);
	lv_obj_set_width(title, LV_PCT(100));
	m->time = ui_dim_label(text, "", &ui_font_20);
	time_fmt(m->time, m->seconds);

	lv_obj_t *trash = ui_box(row);
	lv_obj_set_size(trash, 88, ROW_H);
	lv_obj_set_clickable(trash, true);
	lv_obj_add_event_cb(trash, trash_cb, LV_EVENT_CLICKED, user_data);
	lv_obj_set_style_text_color(trash, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_text_color(trash, lv_color_hex(UI_COLOR_RED), LV_STATE_PRESSED);
	lv_obj_t *symbol = lv_label_create(trash);
	lv_label_set_text_static(symbol, LV_SYMBOL_TRASH);
	lv_obj_center(symbol);
}

static void list_fill(void)
{
	play_stop();
	lv_obj_clean(rec.list);
	free(rec.memos);
	rec.count = memos_load(&rec.memos);
	if (!rec.count) {
		ui_empty_label(rec.list, "No recordings");
		return;
	}
	lv_obj_t *card = ui_card_create(rec.list);
	for (unsigned i = 0; i < rec.count; i++)
		row_create(card, i);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(rec.timer);
	rec.timer = NULL;
	/* Leaving keeps what was recorded */
	if (rec.cap.pcm) {
		snd_pcm_close(rec.cap.pcm);
		rec.cap.pcm = NULL;
		header_save();
		fsync(rec.cap.fd);
		close(rec.cap.fd);
		if (rec.cap.frames < MIN_FRAMES)
			unlink(rec.cap.path);
	}
	if (rec.play.pcm) {
		snd_pcm_drop(rec.play.pcm);
		snd_pcm_close(rec.play.pcm);
		close(rec.play.fd);
	}
	free(rec.memos);
	memset(&rec, 0, sizeof(rec));
}

void recorder_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Recorder", NULL, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);

	rec.list = ui_box(body);
	lv_obj_set_width(rec.list, LV_PCT(100));
	lv_obj_set_flex_grow(rec.list, 1);
	lv_obj_set_flex_flow(rec.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(rec.list, 16, 0);
	lv_obj_set_style_pad_top(rec.list, 8, 0);
	lv_obj_set_style_pad_bottom(rec.list, 16, 0);
	lv_obj_set_scrollable(rec.list, true);
	lv_obj_set_scroll_dir(rec.list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(rec.list, LV_SCROLLBAR_MODE_OFF);

	/* Time, level and the record button */
	rec.controls = ui_box(body);
	lv_obj_set_size(rec.controls, LV_PCT(100), 300);
	lv_obj_set_flex_flow(rec.controls, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(rec.controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(rec.controls, 24, 0);
	lv_obj_set_style_pad_bottom(rec.controls, 24, 0);

	rec.elapsed = lv_label_create(rec.controls);

	rec.meter = ui_box(rec.controls);
	lv_obj_set_size(rec.meter, METER_W, 12);
	lv_obj_set_style_radius(rec.meter, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(rec.meter, lv_color_hex(UI_COLOR_TRACK), 0);
	lv_obj_set_style_bg_opa(rec.meter, LV_OPA_COVER, 0);
	rec.level = ui_box(rec.meter);
	lv_obj_set_size(rec.level, 12, 12);
	lv_obj_set_style_radius(rec.level, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(rec.level, lv_color_hex(UI_COLOR_RED), 0);
	lv_obj_set_style_bg_opa(rec.level, LV_OPA_COVER, 0);

	rec.button = ui_button(rec.controls, 136, 136, UI_COLOR_RED, UI_COLOR_RED_PRESSED, "", record_cb, NULL);
	rec.dot = ui_box(rec.button);
	lv_obj_set_style_bg_color(rec.dot, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(rec.dot, LV_OPA_COVER, 0);
	lv_obj_center(rec.dot);

	rec.confirm = ui_confirm_bar(body, LV_SYMBOL_TRASH "  Delete", delete_memo_cb, cancel_cb, NULL);

	controls_update();
	list_fill();
	rec.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	ui_push(scr);
}
