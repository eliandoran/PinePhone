/*
 * The video engine for the UI tests (src/system/video.h), in place of video.c's
 * GStreamer: a "video" is a text file ui_test_video() writes, its size, length and
 * color; playing it runs on the tests' clock (clock_gettime() wrapped), a frame of that
 * color in NV12 (BT.601, 16-235), its bottom eighth black, each 1/30 s. Anything else
 * fails as a file GStreamer cannot play would.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "system/video.h"
#include "ui_test.h"

#define MAGIC "FAKEVIDEO"
#define FPS 30

struct video {
	enum video_state state;
	bool playing, started;
	int width, height, stride;
	uint32_t duration, pos; /* ms; pos where it was last paused or sought */
	uint64_t since;         /* ms when it last started playing */
	long shown;             /* the frame taken last, -1: none */
	uint8_t *planes;
	char error[64];
};

static int open_count;
static char audio[64];
static struct video *last; /* opened last, NULL once closed */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts); /* the tests' time (ui_test.c's) */
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

bool ui_test_video(const char *path, int width, int height, uint32_t ms, uint32_t rgb)
{
	char text[96];
	snprintf(text, sizeof(text), MAGIC " %d %d %u %06x\n", width, height, ms, rgb);
	return ui_test_write(path, text);
}

int ui_test_videos_open(void)
{
	return open_count;
}

const char *ui_test_video_audio(void)
{
	return audio;
}

bool ui_test_video_playing(void)
{
	return last && last->playing;
}

struct video *video_open(const char *path, const char *audio_device)
{
	struct video *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	open_count++;
	last = v;
	snprintf(audio, sizeof(audio), "%s", audio_device ? audio_device : "");
	v->shown = -1;
	v->playing = true;

	unsigned rgb = 0;
	FILE *f = fopen(path, "r");
	if (!f || fscanf(f, MAGIC " %d %d %u %x", &v->width, &v->height, &v->duration, &rgb) != 4 ||
	    v->width <= 0 || v->height <= 0 || v->width % 2 || v->height % 2) {
		snprintf(v->error, sizeof(v->error), "not a video");
		v->state = VIDEO_FAILED;
		v->width = v->height = 0;
		if (f)
			fclose(f);
		return v;
	}
	fclose(f);

	/* The color in BT.601's 16-235 */
	double r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	double y = 16 + (65.481 * r + 128.553 * g + 24.966 * b) / 255;
	double u = 128 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255;
	double cr = 128 + (112.0 * r - 93.786 * g - 18.214 * b) / 255;
	v->stride = (v->width + 31) & ~31; /* wider than the picture, as a decoder's */
	size_t luma = (size_t)v->stride * (size_t)v->height;
	v->planes = malloc(luma * 3 / 2);
	memset(v->planes, (int)(y + 0.5), luma);
	for (size_t i = luma; i < luma * 3 / 2; i += 2) {
		v->planes[i] = (uint8_t)(u + 0.5);
		v->planes[i + 1] = (uint8_t)(cr + 0.5);
	}
	/* Its bottom eighth black, for which way up it is shown */
	int band = v->height / 8 / 2 * 2;
	memset(v->planes + (size_t)v->stride * (size_t)(v->height - band), 16, (size_t)v->stride * (size_t)band);
	memset(v->planes + luma + (size_t)v->stride * (size_t)(v->height - band) / 2, 128,
	       (size_t)v->stride * (size_t)band / 2);
	v->state = VIDEO_LOADING;
	return v;
}

void video_close(struct video *v)
{
	if (!v)
		return;
	open_count--;
	if (v == last)
		last = NULL;
	free(v->planes);
	free(v);
}

uint32_t video_position(const struct video *v)
{
	if (!v->playing || !v->started)
		return v->pos;
	uint64_t p = v->pos + (now_ms() - v->since);
	return p < v->duration ? (uint32_t)p : v->duration;
}

enum video_state video_poll(struct video *v)
{
	if (v->state == VIDEO_FAILED)
		return v->state;
	if (!v->started) {
		/* Prerolled: playing from now */
		v->started = true;
		v->since = now_ms();
	}
	if (v->playing && video_position(v) >= v->duration) {
		v->pos = v->duration;
		v->playing = false;
		v->state = VIDEO_ENDED;
	} else if (v->state != VIDEO_ENDED) {
		v->state = v->shown < 0 ? VIDEO_LOADING : v->playing ? VIDEO_PLAYING : VIDEO_PAUSED;
	}
	return v->state;
}

const char *video_error(const struct video *v)
{
	return v->error;
}

bool video_frame(struct video *v, struct video_frame *f)
{
	if (v->state == VIDEO_FAILED || !v->started)
		return false;
	long n = (long)video_position(v) * FPS / 1000;
	if (n == v->shown)
		return false;
	v->shown = n;
	*f = (struct video_frame){ .y = v->planes, .uv = v->planes + (size_t)v->stride * (size_t)v->height,
	                           .width = v->width, .height = v->height, .stride = v->stride };
	return true;
}

void video_frame_done(struct video *v)
{
	(void)v;
}

void video_play(struct video *v, bool play)
{
	if (v->state == VIDEO_FAILED || play == v->playing)
		return;
	if (play && v->state == VIDEO_ENDED) {
		v->pos = 0;
		v->state = VIDEO_PLAYING;
	}
	if (!play)
		v->pos = video_position(v);
	v->playing = play;
	v->since = now_ms();
}

void video_seek(struct video *v, uint32_t ms)
{
	if (v->state == VIDEO_FAILED)
		return;
	v->pos = ms < v->duration ? ms : v->duration;
	v->since = now_ms();
	v->shown = -1; /* the frame there */
	if (v->state == VIDEO_ENDED && ms < v->duration)
		v->state = VIDEO_PAUSED;
}

uint32_t video_duration(const struct video *v)
{
	return v->duration;
}

void video_size(const struct video *v, int *width, int *height)
{
	*width = v->width;
	*height = v->height;
}
