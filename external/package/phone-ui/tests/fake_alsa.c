/*
 * ALSA's PCM streams for the UI tests (ui_test.h, "Sound"): the snd_pcm_ functions
 * phone-ui calls, wrapped at link time (the Makefile's -Wl,--wrap=), on a sound card
 * that runs on the tests' clock. Capture hears what the test plays into the
 * microphone (a tone, or silence), at the stream's rate since it started;
 * playback takes what is written at its rate and keeps it for the test. The mixer
 * and controls stay alsa-lib's (no card here: phone-ui logs it).
 * Plugin apps (Genesis) call alsa-lib by its names, not wrapped: the program defines those
 * it needs too, which its exported symbols (-rdynamic) put ahead of alsa-lib's.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

#include "ui_test.h"

#define PLAYED_MAX (48000 * 2 * 60) /* samples kept: a minute of 48 kHz stereo */

struct pcm {
	snd_pcm_stream_t stream;
	snd_pcm_state_t state;
	unsigned rate, channels;
	snd_pcm_uframes_t buffer;
	uint64_t start_ms;  /* when it started */
	uint64_t base;      /* frames moved before the last start */
	uint64_t app;       /* frames read or written by phone-ui */
	bool xrun;          /* forced by the test */
};

static bool no_card;
static int mic_peak;
static int16_t *played;
static size_t played_samples;
static unsigned played_channels, played_rate;
static struct pcm *open_pcms[4];
static char played_device[128]; /* the device the last playback stream was opened on */

void ui_test_no_sound_card(bool none)
{
	no_card = none;
}

void ui_test_microphone(int peak)
{
	mic_peak = peak;
}

const char *ui_test_played_device(void)
{
	return played_device;
}

void ui_test_sound_xrun(void)
{
	for (unsigned i = 0; i < sizeof(open_pcms) / sizeof(open_pcms[0]); i++)
		if (open_pcms[i])
			open_pcms[i]->xrun = true;
}

const int16_t *ui_test_played(size_t *frames, unsigned *channels, unsigned *rate)
{
	*frames = played_channels ? played_samples / played_channels : 0;
	*channels = played_channels;
	*rate = played_rate;
	return played;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts); /* the tests' (--wrap=clock_gettime) */
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Frames the card moved: captured, or played, since the stream was prepared */
static uint64_t hw_frames(const struct pcm *p)
{
	if (p->state != SND_PCM_STATE_RUNNING)
		return p->base;
	return p->base + (now_ms() - p->start_ms) * p->rate / 1000;
}

/* An overrun (capture fell behind) or an underrun (playback ran out) */
static bool xrun_check(struct pcm *p)
{
	if (p->state != SND_PCM_STATE_RUNNING)
		return p->state == SND_PCM_STATE_XRUN;
	uint64_t hw = hw_frames(p);
	bool over = p->stream == SND_PCM_STREAM_CAPTURE ? hw - p->app > p->buffer : hw >= p->app;
	if (over || p->xrun) {
		p->state = SND_PCM_STATE_XRUN;
		p->xrun = false;
		if (p->stream == SND_PCM_STREAM_PLAYBACK)
			p->base = p->app;
	}
	return p->state == SND_PCM_STATE_XRUN;
}

int __wrap_snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
	(void)mode;
	if (no_card)
		return -ENOENT;
	struct pcm *p = calloc(1, sizeof(*p));
	if (stream == SND_PCM_STREAM_PLAYBACK)
		snprintf(played_device, sizeof(played_device), "%s", name);
	p->stream = stream;
	p->state = SND_PCM_STATE_OPEN;
	for (unsigned i = 0; i < sizeof(open_pcms) / sizeof(open_pcms[0]); i++)
		if (!open_pcms[i]) {
			open_pcms[i] = p;
			break;
		}
	*pcm = (snd_pcm_t *)p;
	return 0;
}

int __wrap_snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access,
                              unsigned channels, unsigned rate, int resample, unsigned latency_us)
{
	(void)resample;
	struct pcm *p = (struct pcm *)pcm;
	if (format != SND_PCM_FORMAT_S16_LE || access != SND_PCM_ACCESS_RW_INTERLEAVED || !channels || !rate)
		return -EINVAL;
	p->channels = channels;
	p->rate = rate;
	p->buffer = (snd_pcm_uframes_t)((uint64_t)rate * latency_us / 1000000);
	p->state = SND_PCM_STATE_PREPARED;
	if (p->stream == SND_PCM_STREAM_PLAYBACK) {
		free(played);
		played = NULL;
		played_samples = 0;
		played_channels = channels;
		played_rate = rate;
	}
	return 0;
}

int __wrap_snd_pcm_close(snd_pcm_t *pcm)
{
	for (unsigned i = 0; i < sizeof(open_pcms) / sizeof(open_pcms[0]); i++)
		if (open_pcms[i] == (struct pcm *)pcm)
			open_pcms[i] = NULL;
	free(pcm);
	return 0;
}

int __wrap_snd_pcm_start(snd_pcm_t *pcm)
{
	struct pcm *p = (struct pcm *)pcm;
	if (p->state != SND_PCM_STATE_PREPARED)
		return -EBADFD;
	p->state = SND_PCM_STATE_RUNNING;
	p->start_ms = now_ms();
	return 0;
}

int __wrap_snd_pcm_drop(snd_pcm_t *pcm)
{
	((struct pcm *)pcm)->state = SND_PCM_STATE_SETUP;
	return 0;
}

snd_pcm_state_t __wrap_snd_pcm_state(snd_pcm_t *pcm)
{
	struct pcm *p = (struct pcm *)pcm;
	xrun_check(p);
	return p->state;
}

int __wrap_snd_pcm_recover(snd_pcm_t *pcm, int err, int silent)
{
	(void)silent;
	struct pcm *p = (struct pcm *)pcm;
	if (err != -EPIPE)
		return err;
	/* Prepared again: capture starts over from now, playback refills */
	p->base = p->app = 0;
	p->state = SND_PCM_STATE_PREPARED;
	if (p->stream == SND_PCM_STREAM_CAPTURE)
		return __wrap_snd_pcm_start(pcm);
	return 0;
}

snd_pcm_sframes_t __wrap_snd_pcm_avail_update(snd_pcm_t *pcm)
{
	struct pcm *p = (struct pcm *)pcm;
	if (xrun_check(p))
		return -EPIPE;
	if (p->stream == SND_PCM_STREAM_CAPTURE)
		return (snd_pcm_sframes_t)(hw_frames(p) - p->app);
	return (snd_pcm_sframes_t)(p->buffer - (p->app - hw_frames(p)));
}

int __wrap_snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay)
{
	struct pcm *p = (struct pcm *)pcm;
	if (xrun_check(p))
		return -EPIPE;
	*delay = p->stream == SND_PCM_STREAM_CAPTURE ? (snd_pcm_sframes_t)(hw_frames(p) - p->app) :
	                                                (snd_pcm_sframes_t)(p->app - hw_frames(p));
	return 0;
}

/* The microphone: a 440 Hz tone of mic_peak, on every channel */
static int16_t mic_sample(uint64_t frame, unsigned rate)
{
	return (int16_t)lround(mic_peak * sin(2 * M_PI * 440 * (double)frame / rate));
}

snd_pcm_sframes_t __wrap_snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t size)
{
	struct pcm *p = (struct pcm *)pcm;
	if (p->stream != SND_PCM_STREAM_CAPTURE)
		return -EBADFD;
	if (xrun_check(p))
		return -EPIPE;
	if (p->state != SND_PCM_STATE_RUNNING)
		return -EAGAIN;
	uint64_t avail = hw_frames(p) - p->app;
	if (!avail)
		return -EAGAIN;
	snd_pcm_uframes_t n = size < avail ? size : (snd_pcm_uframes_t)avail;
	int16_t *out = buf;
	for (snd_pcm_uframes_t i = 0; i < n; i++)
		for (unsigned c = 0; c < p->channels; c++)
			out[i * p->channels + c] = mic_sample(p->app + i, p->rate);
	p->app += n;
	return (snd_pcm_sframes_t)n;
}

snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t size)
{
	struct pcm *p = (struct pcm *)pcm;
	if (p->stream != SND_PCM_STREAM_PLAYBACK)
		return -EBADFD;
	if (xrun_check(p))
		return -EPIPE;
	uint64_t space = p->buffer - (p->app - hw_frames(p));
	if (!space)
		return -EAGAIN;
	snd_pcm_uframes_t n = size < space ? size : (snd_pcm_uframes_t)space;
	size_t samples = n * p->channels;
	if (played_samples + samples <= PLAYED_MAX) {
		played = realloc(played, (played_samples + samples) * sizeof(*played));
		memcpy(played + played_samples, buf, samples * sizeof(*played));
		played_samples += samples;
	}
	p->app += n;
	/* As snd_pcm_set_params() sets it: playback starts once the buffer is full */
	if (p->state == SND_PCM_STATE_PREPARED && p->app >= p->buffer)
		__wrap_snd_pcm_start(pcm);
	return (snd_pcm_sframes_t)n;
}

/* For plugin apps: alsa-lib's names, onto the same card */

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
	return __wrap_snd_pcm_open(pcm, name, stream, mode);
}

int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access, unsigned int channels,
                       unsigned int rate, int soft_resample, unsigned int latency)
{
	return __wrap_snd_pcm_set_params(pcm, format, access, channels, rate, soft_resample, latency);
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
	return __wrap_snd_pcm_writei(pcm, buffer, size);
}

int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent)
{
	return __wrap_snd_pcm_recover(pcm, err, silent);
}

int snd_pcm_close(snd_pcm_t *pcm)
{
	return __wrap_snd_pcm_close(pcm);
}
