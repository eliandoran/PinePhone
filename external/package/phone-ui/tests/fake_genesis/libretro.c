/*
 * Genesis Plus GX in the tests (fake_genesis.h): the libretro functions genesis.so
 * looks up, a frame of one color each run, its save state the frames run so far.
 */

#include <stdio.h>
#include <string.h>

#include "fake_genesis.h"
#include "libretro.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define STATE_SIZE 32
#define EXPORT __attribute__((visibility("default")))

static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio_batch;
static retro_input_poll_t input_poll;
static retro_input_state_t input_state;
static uint16_t frame[720 * 576];
static int16_t sound[800 * 2];

EXPORT void retro_set_environment(retro_environment_t cb)
{
	environment = cb;
}

EXPORT void retro_init(void)
{
	struct fake_genesis *f = &fake_genesis;
	const char *dir = NULL;
	if (environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
		snprintf(f->system_dir, sizeof(f->system_dir), "%s", dir);
	if (environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
		snprintf(f->save_dir, sizeof(f->save_dir), "%s", dir);
	struct retro_log_callback log = { 0 };
	if (environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		log.log(RETRO_LOG_INFO, "not shown\n");
		log.log(RETRO_LOG_WARN, "fake core ready");
	}
	struct retro_variable var = { "genesis_plus_gx_region_detect", NULL };
	environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
	bool updated;
	environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated);
	environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
	environment(RETRO_ENVIRONMENT_SET_ROTATION, &(unsigned){ 0 }); /* not one it takes */
}

EXPORT void retro_deinit(void)
{
}

EXPORT void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video = cb;
}

EXPORT void retro_set_audio_sample(retro_audio_sample_t cb)
{
	(void)cb;
}

EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch = cb;
}

EXPORT void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll = cb;
}

EXPORT void retro_set_input_state(retro_input_state_t cb)
{
	input_state = cb;
}

EXPORT void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port;
	(void)device;
}

EXPORT bool retro_load_game(const struct retro_game_info *info)
{
	struct fake_genesis *f = &fake_genesis;
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	f->rgb565 = environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
	snprintf(f->path, sizeof(f->path), "%s", info->path);
	f->state[0] = '\0';
	if (f->load_fails)
		return false;
	__atomic_add_fetch(&f->loads, 1, __ATOMIC_ACQ_REL);
	return true;
}

EXPORT void retro_unload_game(void)
{
	__atomic_add_fetch(&fake_genesis.unloads, 1, __ATOMIC_ACQ_REL);
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = info->geometry.max_width = 320;
	info->geometry.base_height = info->geometry.max_height = 224;
	info->timing.fps = 59.92;
	info->timing.sample_rate = 48000;
}

EXPORT void retro_run(void)
{
	struct fake_genesis *f = &fake_genesis;
	input_poll();
	__atomic_store_n(&f->pad, (unsigned)input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) & 0xffff,
	                 __ATOMIC_RELEASE);
	input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
	input_state(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
	unsigned w = LOAD(f->w) ? LOAD(f->w) : 320, h = LOAD(f->h) ? LOAD(f->h) : 224;
	uint32_t c = LOAD(f->color);
	uint16_t px = (c >> 19 & 0x1f) << 11 | (c >> 10 & 0x3f) << 5 | (c >> 3 & 0x1f);
	for (unsigned i = 0; i < w * h; i++)
		frame[i] = px;
	video(frame, w, h, w * 2);
	video(NULL, w, h, w * 2); /* a frame the same as the last */
	audio_batch(sound, 800);
	__atomic_add_fetch(&f->frames, 1, __ATOMIC_ACQ_REL);
}

EXPORT void retro_reset(void)
{
	__atomic_add_fetch(&fake_genesis.resets, 1, __ATOMIC_ACQ_REL);
}

EXPORT size_t retro_serialize_size(void)
{
	return STATE_SIZE;
}

EXPORT bool retro_serialize(void *data, size_t size)
{
	if (size < STATE_SIZE)
		return false;
	memset(data, 0, size);
	snprintf(data, size, "frames %u", LOAD(fake_genesis.frames));
	return true;
}

EXPORT bool retro_unserialize(const void *data, size_t size)
{
	if (size != STATE_SIZE)
		return false;
	snprintf(fake_genesis.state, sizeof(fake_genesis.state), "%.*s", (int)size, (const char *)data);
	return true;
}

EXPORT void *retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM && fake_genesis.sram_size ? fake_genesis.sram : NULL;
}

EXPORT size_t retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SAVE_RAM ? fake_genesis.sram_size : 0;
}
