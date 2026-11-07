/*
 * Doom's sound on the PinePhone: the sound effects module of doomgeneric
 * (DG_sound_module), and the output that Chocolate Doom's OPL music (i_oplmusic.c,
 * opl/, through SDL_mixer.h) plays into.
 *
 * The output is phone-ui's (ui_sound_write(), handed over in doom_host: the phone's, or
 * a Bluetooth speaker): 48 kHz, 16-bit stereo, periods of PERIOD frames. A thread
 * fills each period: the music hook first (it writes the OPL emulator's output),
 * then the sound effects added on top. If the device is busy (Music playing) or
 * missing, the game runs silent, the thread keeping time. Paused (the app not
 * shown, engine.c), the device is let go; Doom quitting stops the thread.
 *
 * Sound effects are DMX lumps (8-bit unsigned, 11 kHz mostly), played at their
 * rate by stepping through them (nearest sample, as DOS did), panned and scaled as
 * Chocolate Doom does with SDL_mixer.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "doom.h"
#include "SDL_mixer.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define RATE 48000
#define PERIOD 512 /* frames: 10.7 ms */
#define CHANNELS 16 /* Doom uses snd_channels of them (8) */

/* Bound by i_sound.c (resampling options of the SDL module) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static struct doom_host *host; /* its sound output */
static pthread_t thread;

struct sample {
	const uint8_t *data;
	uint32_t length;
	uint32_t step; /* 16.16 source samples per output frame */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	const struct sample *sample; /* NULL: free */
	uint32_t pos;                /* 16.16 */
	int left, right;             /* 0 to 255 */
} channels[CHANNELS];

static void (*music_hook)(void *, Uint8 *, int);
static void *music_arg;
static boolean use_prefix;

static bool audio_paused, audio_quit; /* atomic */
static bool thread_started;

#define PAUSE_POLL_MS 200

static void *audio_main(void *arg)
{
	static int16_t out[PERIOD * 2];
	static int32_t mix[PERIOD * 2];
	(void)arg;

	while (!__atomic_load_n(&audio_quit, __ATOMIC_ACQUIRE)) {
		/* Paused (the app not shown): the device let go */
		if (__atomic_load_n(&audio_paused, __ATOMIC_ACQUIRE)) {
			host->sound_release(host->sound);
			usleep(PAUSE_POLL_MS * 1000);
			continue;
		}

		memset(out, 0, sizeof(out));
		void (*hook)(void *, Uint8 *, int) = __atomic_load_n(&music_hook, __ATOMIC_ACQUIRE);
		if (hook)
			hook(music_arg, (Uint8 *)out, sizeof(out));
		for (int i = 0; i < PERIOD * 2; i++)
			mix[i] = out[i];

		pthread_mutex_lock(&lock);
		for (int c = 0; c < CHANNELS; c++) {
			const struct sample *s = channels[c].sample;
			if (!s)
				continue;
			uint32_t pos = channels[c].pos;
			for (int f = 0; f < PERIOD && pos >> 16 < s->length; f++, pos += s->step) {
				int v = (s->data[pos >> 16] - 128) << 8;
				mix[f * 2] += v * channels[c].left / 255;
				mix[f * 2 + 1] += v * channels[c].right / 255;
			}
			channels[c].pos = pos;
			if (pos >> 16 >= s->length)
				channels[c].sample = NULL;
		}
		pthread_mutex_unlock(&lock);

		for (int i = 0; i < PERIOD * 2; i++)
			out[i] = mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i];
		/* No device (busy, missing): silent, keeping time */
		if (!host->sound_write(host->sound, out, PERIOD))
			usleep(PERIOD * 1000000LL / RATE);
	}
	host->sound_release(host->sound);
	return NULL;
}

/* The output's thread, once (phone-ui's output opens the device, and again whenever it
 * was let go, busy or moved) */
static bool audio_start(void)
{
	if (thread_started)
		return true;
	if (pthread_create(&thread, NULL, audio_main, NULL) != 0)
		return false;
	thread_started = true;
	printf("doom: sound at %d Hz\n", RATE);
	return true;
}

/* engine.c: the output, before Doom starts */
void doom_sound_host(struct doom_host *h)
{
	host = h;
}

/* engine.c: the app not shown, or shown again */
void doom_sound_pause(bool paused)
{
	__atomic_store_n(&audio_paused, paused, __ATOMIC_RELEASE);
}

/* engine.c: Doom quit, the library is about to be unloaded */
void doom_sound_stop(void)
{
	if (!thread_started)
		return;
	__atomic_store_n(&audio_quit, true, __ATOMIC_RELEASE);
	pthread_join(thread, NULL);
	thread_started = false;
}

/* SDL_mixer.h */

int Mix_OpenAudio(int frequency, Uint16 format, int channels_, int chunksize)
{
	(void)frequency;
	(void)format;
	(void)channels_;
	(void)chunksize;
	return audio_start() ? 0 : -1;
}

void Mix_CloseAudio(void)
{
}

int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels_)
{
	*frequency = RATE;
	*format = AUDIO_S16SYS;
	*channels_ = 2;
	return thread_started; /* the device may be opened later */
}

void Mix_HookMusic(void (*mix_func)(void *udata, Uint8 *stream, int len), void *arg)
{
	music_arg = arg;
	__atomic_store_n(&music_hook, mix_func, __ATOMIC_RELEASE);
}

const char *Mix_GetError(void)
{
	return "no sound output"; /* NON-NLS: for the engine's log */
}

/* The sound effects module */

static boolean Sound_Init(boolean use_sfx_prefix)
{
	use_prefix = use_sfx_prefix;
	return audio_start();
}

static void Sound_Shutdown(void)
{
}

static int Sound_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char name[16];
	if (sfx->link)
		sfx = sfx->link;
	snprintf(name, sizeof(name), use_prefix ? "ds%s" : "%s", sfx->name);
	return W_GetNumForName(name);
}

/* The lump's samples, read once (kept), NULL if not a valid DMX sound: header
 * (format 3, rate, length), then 16 bytes that DMX skips at each end */
static const struct sample *sample_get(sfxinfo_t *sfx)
{
	if (sfx->driver_data)
		return sfx->driver_data;
	const uint8_t *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
	unsigned lumplen = W_LumpLength(sfx->lumpnum);
	if (lumplen < 8 || data[0] != 3 || data[1] != 0)
		return NULL;
	unsigned rate = data[3] << 8 | data[2];
	unsigned length = (unsigned)data[7] << 24 | data[6] << 16 | data[5] << 8 | data[4];
	if (length > lumplen - 8 || length <= 48)
		return NULL;
	struct sample *s = Z_Malloc(sizeof(*s), PU_STATIC, NULL);
	s->data = data + 8 + 16;
	s->length = length - 32;
	s->step = ((uint64_t)rate << 16) / RATE;
	sfx->driver_data = s;
	return s;
}

static void Sound_UpdateSoundParams(int channel, int vol, int sep)
{
	if (channel < 0 || channel >= CHANNELS)
		return;
	int left = (254 - sep) * vol / 127, right = sep * vol / 127;
	pthread_mutex_lock(&lock);
	channels[channel].left = left < 0 ? 0 : left > 255 ? 255 : left;
	channels[channel].right = right < 0 ? 0 : right > 255 ? 255 : right;
	pthread_mutex_unlock(&lock);
}

static int Sound_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
	if (channel < 0 || channel >= CHANNELS || !thread_started)
		return -1;
	const struct sample *s = sample_get(sfx);
	if (!s)
		return -1;
	pthread_mutex_lock(&lock);
	channels[channel].sample = s;
	channels[channel].pos = 0;
	pthread_mutex_unlock(&lock);
	Sound_UpdateSoundParams(channel, vol, sep);
	return channel;
}

static void Sound_StopSound(int channel)
{
	if (channel < 0 || channel >= CHANNELS)
		return;
	pthread_mutex_lock(&lock);
	channels[channel].sample = NULL;
	pthread_mutex_unlock(&lock);
}

static boolean Sound_SoundIsPlaying(int channel)
{
	if (channel < 0 || channel >= CHANNELS)
		return false;
	pthread_mutex_lock(&lock);
	boolean playing = channels[channel].sample != NULL;
	pthread_mutex_unlock(&lock);
	return playing;
}

static void Sound_Update(void)
{
}

static void Sound_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
	(void)sounds;
	(void)num_sounds;
}

static snddevice_t sound_devices[] = {
	SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
	SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
	sound_devices,
	sizeof(sound_devices) / sizeof(sound_devices[0]),
	Sound_Init,
	Sound_Shutdown,
	Sound_GetSfxLumpNum,
	Sound_Update,
	Sound_UpdateSoundParams,
	Sound_StartSound,
	Sound_StopSound,
	Sound_SoundIsPlaying,
	Sound_CacheSounds,
};
