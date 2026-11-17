/*
 * Ringtones (ringer.h): each a list of notes, made into sound as it plays: no files, no
 * decoder. A note is its pitch and a quieter octave above it, struck and fading as a bell
 * or held as a beep, a few ms of fade at both ends against clicks.
 *
 * Played by a thread of its own (started with the first tone, then waiting for the next):
 * a write to the sound device (sound.c) blocks while its buffer is full, which the UI must
 * not. It asks between pieces of TONE_CHUNK frames whether it is still wanted, so another
 * tone or a stop takes effect within one, and lets go of the device after each tone: Music
 * and the games need it free. The silence after a tone is what its end plays out in, as
 * closing the device drops what it has not played. Ringing (a call coming in) is the
 * chosen tone again and again with a pause between, the device kept until stopped; with
 * none free as it starts, it stays silent.
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "system/ringer.h"
#include "ui.h"

#define RATE 48000 /* the codec's own, in stereo: nothing to convert */
#define CHANNELS 2
#define LATENCY_US 100000
#define TONE_CHUNK (RATE / 50)
#define TAIL_MS 150   /* silence: more than the device's buffer */
#define PEAK 24000    /* of 32767 */
#define ATTACK_S 0.004
#define RELEASE_S 0.010
#define BELL_DECAY_S 0.25
#define OCTAVE_DECAY_S 0.08
#define OCTAVE 0.35   /* its share of the pitch's level */

enum voice { VOICE_BELL, VOICE_BEEP };

struct note {
	uint16_t hz; /* 0: a rest */
	uint16_t ms;
};

struct tone {
	const char *id; /* in the settings */
	const char *name;
	enum voice voice;
	const struct note *notes;
	unsigned count;
};

/* A telephone's bell: two notes in turn, twice */
#define TRILL { 1397, 45 }, { 1109, 45 }
#define RING TRILL, TRILL, TRILL, TRILL, TRILL, TRILL, TRILL, TRILL
static const struct note classic[] = { RING, { 0, 280 }, RING, { 1397, 200 } };

static const struct note chime[] = {
	{ 1319, 220 }, { 1047, 220 }, { 784, 220 }, { 1047, 520 }, { 0, 160 },
	{ 1319, 220 }, { 1047, 220 }, { 1175, 220 }, { 1568, 700 },
};

static const struct note rise[] = {
	{ 523, 150 }, { 659, 150 }, { 784, 150 }, { 1047, 150 }, { 1319, 420 }, { 0, 180 },
	{ 587, 150 }, { 740, 150 }, { 880, 150 }, { 1175, 150 }, { 1480, 600 },
};

#define BEEPS { 1976, 90 }, { 0, 70 }, { 1976, 90 }, { 0, 70 }, { 1976, 90 }, { 0, 70 }, { 1976, 90 }
static const struct note pulse[] = { BEEPS, { 0, 420 }, BEEPS, { 0, 420 }, BEEPS };

#define NOTES(n) n, sizeof(n) / sizeof(n[0])
static const struct tone tones[] = {
	{ "classic", N_("Classic"), VOICE_BELL, NOTES(classic) },
	{ "chime",   N_("Chime"),   VOICE_BELL, NOTES(chime) },
	{ "rise",    N_("Rise"),    VOICE_BELL, NOTES(rise) },
	{ "pulse",   N_("Pulse"),   VOICE_BEEP, NOTES(pulse) },
};
#define TONE_COUNT (sizeof(tones) / sizeof(tones[0]))

static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	bool started;     /* the thread */
	int tone;         /* asked for and not played to its end yet, -1: none */
	bool loop;        /* again and again (ringing), until another is asked for or none */
	unsigned request; /* counts what was asked: the thread stops playing an earlier one */
	bool busy;        /* the thread plays, or still holds the device */
} ringer = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .tone = -1 };

unsigned ui_ringtone_count(void)
{
	return TONE_COUNT;
}

const char *ui_ringtone_name(unsigned i)
{
	return tones[i < TONE_COUNT ? i : 0].name;
}

uint32_t ui_ringtone_ms(unsigned i)
{
	const struct tone *t = &tones[i < TONE_COUNT ? i : 0];
	uint32_t ms = 0;
	for (unsigned n = 0; n < t->count; n++)
		ms += t->notes[n].ms;
	return ms;
}

unsigned ui_ringtone(void)
{
	const char *id = ui_setting_get("ringtone", "");
	for (unsigned i = 0; i < TONE_COUNT; i++)
		if (strcmp(tones[i].id, id) == 0)
			return i;
	return 0;
}

void ui_ringtone_set(unsigned i)
{
	if (i < TONE_COUNT)
		ui_setting_set("ringtone", tones[i].id);
}

static uint32_t note_frames(const struct note *n)
{
	return (uint32_t)n->ms * (RATE / 1000);
}

/* A note's sample `at` frames into it */
static int16_t note_sample(const struct note *n, enum voice voice, uint32_t at)
{
	if (!n->hz)
		return 0;
	double t = (double)at / RATE, left = (double)(note_frames(n) - at) / RATE;
	double angle = 2 * M_PI * n->hz * t;
	double level = fmin(1, t / ATTACK_S) * fmin(1, left / RELEASE_S);
	double octave = OCTAVE;
	if (voice == VOICE_BELL) {
		level *= exp(-t / BELL_DECAY_S);
		octave *= exp(-t / OCTAVE_DECAY_S);
	}
	return (int16_t)lround(PEAK * level * (sin(angle) + octave * sin(2 * angle)) / (1 + OCTAVE));
}

/* `count` frames of the tone from `frame` on, both channels the same: how many it still had */
static unsigned tone_render(const struct tone *t, uint32_t frame, int16_t *out, unsigned count)
{
	unsigned note = 0, done = 0;
	uint32_t start = 0;
	while (done < count && note < t->count) {
		uint32_t frames = note_frames(&t->notes[note]);
		if (frame >= start + frames) {
			start += frames;
			note++;
			continue;
		}
		int16_t s = note_sample(&t->notes[note], t->voice, frame - start);
		for (unsigned c = 0; c < CHANNELS; c++)
			out[done * CHANNELS + c] = s;
		done++;
		frame++;
	}
	return done;
}

static bool wanted(unsigned request)
{
	pthread_mutex_lock(&ringer.lock);
	bool still = ringer.request == request;
	pthread_mutex_unlock(&ringer.lock);
	return still;
}

static void *ringer_thread(void *arg)
{
	(void)arg;
	static int16_t buf[TONE_CHUNK * CHANNELS];
	struct ui_sound *out = ui_sound_open("ringer", RATE, CHANNELS, LATENCY_US);

	pthread_mutex_lock(&ringer.lock);
	for (;;) {
		while (ringer.tone < 0)
			pthread_cond_wait(&ringer.wake, &ringer.lock);
		const struct tone *t = &tones[ringer.tone];
		unsigned request = ringer.request;
		bool loop = ringer.loop;
		ringer.busy = true;
		pthread_mutex_unlock(&ringer.lock);

		/* After the tone, silence: as long as the device takes to play its end, or
		 * ringing, the pause before it starts again */
		uint32_t after = (loop ? UI_RING_PAUSE_MS : TAIL_MS) * (RATE / 1000);
		uint32_t frame = 0, silence = after;
		bool heard = true; /* false: no device (none, or Music's meanwhile) */
		while (heard && wanted(request)) {
			unsigned n = tone_render(t, frame, buf, TONE_CHUNK);
			frame += n;
			if (!n && !silence) {
				if (!loop)
					break;
				frame = 0;
				silence = after;
				continue;
			}
			if (!n) {
				n = silence < TONE_CHUNK ? silence : TONE_CHUNK;
				memset(buf, 0, n * CHANNELS * sizeof(buf[0]));
				silence -= n;
			}
			heard = ui_sound_write(out, buf, n);
		}
		ui_sound_release(out);

		pthread_mutex_lock(&ringer.lock);
		ringer.busy = false;
		if (ringer.request == request)
			ringer.tone = -1; /* played; else another one was asked for meanwhile, or none */
	}
	return NULL;
}

static void ringer_start(unsigned i, bool loop)
{
	if (i >= TONE_COUNT)
		return;
	pthread_mutex_lock(&ringer.lock);
	if (!ringer.started) {
		pthread_t thread;
		int err = pthread_create(&thread, NULL, ringer_thread, NULL);
		if (err)
			printf("phone-ui: no ringer: %s\n", strerror(err));
		else
			pthread_detach(thread);
		ringer.started = !err;
	}
	if (ringer.started) {
		ringer.tone = (int)i;
		ringer.loop = loop;
		ringer.request++;
		pthread_cond_signal(&ringer.wake);
	}
	pthread_mutex_unlock(&ringer.lock);
}

void ui_ringer_play(unsigned i)
{
	ringer_start(i, false);
}

void ui_ringer_ring(void)
{
	ringer_start(ui_ringtone(), true);
}

void ui_ringer_stop(void)
{
	pthread_mutex_lock(&ringer.lock);
	ringer.tone = -1;
	ringer.request++;
	pthread_mutex_unlock(&ringer.lock);
}

bool ui_ringer_playing(void)
{
	pthread_mutex_lock(&ringer.lock);
	bool playing = ringer.tone >= 0 || ringer.busy;
	pthread_mutex_unlock(&ringer.lock);
	return playing;
}
