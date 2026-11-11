/*
 * Playing a video file (video.c): GStreamer's playbin, the picture decoded by the video
 * engine where it can (v4l2codecs' stateless decoders, the A64's Cedrus) and handed over
 * as NV12 frames, the sound played on an ALSA device. Everything is called from LVGL's
 * thread: video_poll() each frame reads what the pipeline said, video_frame() takes the
 * newest picture. The tests link tests/fake_video.c instead, the WebAssembly build a stub.
 */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdbool.h>
#include <stdint.h>

enum video_state {
	VIDEO_LOADING, /* opened, the first frame not shown yet */
	VIDEO_PLAYING,
	VIDEO_PAUSED,
	VIDEO_ENDED,   /* played to the end, paused on its last frame */
	VIDEO_FAILED,  /* cannot be played: video_error() says why, for the log */
};

/* A picture: NV12 (a plane of luma, then one of interleaved U and V at half the size
 * both ways), `width` x `height` shown; rows `stride` bytes apart in both planes (the
 * decoder's, wider than the picture) */
struct video_frame {
	const uint8_t *y, *uv;
	int width, height;
	int stride;
	/* The planes as dma-bufs (the decoder's), for the GPU to read without copying them:
	 * -1 when not one */
	int fd[2];
	size_t offset[2]; /* bytes into fd[i] */
	bool bt709;      /* else BT.601 */
	bool full_range; /* else 16-235 */
};

struct video;

/* Opens `path` and starts playing it on `audio_device` (an ALSA device, NULL: none);
 * NULL only without memory: a file that cannot be played comes to VIDEO_FAILED */
struct video *video_open(const char *path, const char *audio_device);
void video_close(struct video *v); /* NULL: nothing */
/* What the pipeline said since: call it each frame */
enum video_state video_poll(struct video *v);
const char *video_error(const struct video *v); /* "" unless failed */
/* The newest picture not taken yet: false without one. It stays valid, its buffer held,
 * until the next video_frame() or video_close(): the GPU may read it after the call
 * that drew it returned. */
bool video_frame(struct video *v, struct video_frame *f);
void video_play(struct video *v, bool play); /* after the end: from the start */
void video_seek(struct video *v, uint32_t ms);
uint32_t video_position(const struct video *v); /* ms; during a seek, the place sought */
uint32_t video_duration(const struct video *v); /* ms, 0 while unknown */
/* The picture's size once known (0 before), for the screen's layout */
void video_size(const struct video *v, int *width, int *height);

#endif
