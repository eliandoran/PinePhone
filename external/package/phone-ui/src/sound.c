/*
 * Sound output for plugin apps' threads (ui.h: Doom, Genesis): 16-bit interleaved PCM
 * through alsa-lib on where phone-ui's sound goes (ui_sound_device(): the phone's, or a
 * Bluetooth speaker). Opened at the first write, and again when that device changed
 * (checked each second), after it was busy (Music playing) or let go; a write blocks
 * while the buffer is full, which paces a game, and says so when there is no device,
 * for the game to pace itself.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "ui.h"

#define CHECK_MS 1000 /* the device changed, or a busy one free again */

struct ui_sound {
	char name[32];       /* for the log: the app's */
	unsigned rate, channels, latency_us;
	snd_pcm_t *pcm;      /* NULL: not open */
	const char *device;  /* the ui_sound_device() it was opened on */
	const char *failed;  /* the device that could not be opened, said once */
	uint64_t checked;    /* 0: at the next write */
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

struct ui_sound *ui_sound_open(const char *name, unsigned rate, unsigned channels, unsigned latency_us)
{
	struct ui_sound *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->rate = rate;
	s->channels = channels;
	s->latency_us = latency_us;
	return s;
}

void ui_sound_release(struct ui_sound *s)
{
	if (s && s->pcm)
		snd_pcm_close(s->pcm);
	if (s) {
		s->pcm = NULL;
		s->checked = 0;
	}
}

void ui_sound_close(struct ui_sound *s)
{
	ui_sound_release(s);
	free(s);
}

static void pcm_open(struct ui_sound *s)
{
	const char *device = ui_sound_device();
	s->device = device;
	int err = snd_pcm_open(&s->pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (err == 0)
		err = snd_pcm_set_params(s->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, s->channels,
		                         s->rate, 1, s->latency_us);
	if (err < 0) {
		if (s->failed != device)
			printf("%s: no sound: %s: %s\n", s->name, device, snd_strerror(err));
		s->failed = device;
		if (s->pcm)
			snd_pcm_close(s->pcm);
		s->pcm = NULL;
		return;
	}
	s->failed = NULL;
	printf("%s: sound on %s\n", s->name, device);
}

bool ui_sound_write(struct ui_sound *s, const int16_t *frames, unsigned count)
{
	if (!s)
		return false;
	uint64_t now = now_ms();
	if (!s->checked || now - s->checked >= CHECK_MS) {
		s->checked = now;
		if (s->pcm && ui_sound_device() != s->device) {
			snd_pcm_close(s->pcm);
			s->pcm = NULL;
		}
		if (!s->pcm)
			pcm_open(s);
	}
	while (s->pcm && count) {
		snd_pcm_sframes_t n = snd_pcm_writei(s->pcm, frames, count);
		if (n == -EAGAIN) {
			usleep(1000);
		} else if (n < 0) {
			/* Underrun (the game stalled), or after the phone slept: start again */
			if (snd_pcm_recover(s->pcm, (int)n, 1) < 0) {
				printf("%s: sound stopped: %s\n", s->name, snd_strerror((int)n));
				snd_pcm_close(s->pcm);
				s->pcm = NULL; /* tried again in CHECK_MS */
			}
		} else {
			frames += n * s->channels;
			count -= (unsigned)n;
		}
	}
	return s->pcm != NULL;
}
