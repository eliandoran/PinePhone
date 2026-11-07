/*
 * What phone-ui uses that a browser does not have, missing as phone-ui expects it to be
 * on a board without it: no sound card (ALSA), no camera, no external apps, no USB
 * network to pair a computer over, no screenshots.
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <string.h>
#include <sys/sysinfo.h>

#include "pairing.h"
#include "ui.h"

/* Camera, as camera.c's screen without the GPU renderer */
void camera_open(void)
{
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Camera", NULL, &body);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *label = lv_label_create(body);
	lv_label_set_text_static(label, "No camera in the browser");
	ui_push(scr);
}

bool ui_screenshot_save(const char *path)
{
	LV_UNUSED(path);
	return false;
}

void ui_screenshot(void)
{
}

void pairing_init(void)
{
}

/* Settings › Developer mode: no computer allowed */
struct pairing_computer *pairing_computers(int *count)
{
	*count = 0;
	return NULL;
}

bool pairing_revoke(const char *b64)
{
	LV_UNUSED(b64);
	return false;
}

/* No external apps here; the built-in ones start as on the phone */
void external_scan(void)
{
	tasks_start();
}

void external_open(const struct ui_app *app)
{
	LV_UNUSED(app);
}

/* ui.c's for evdev's touchscreens; the page's pointer is not one */
void lv_evdev_set_rollover(lv_indev_t *indev, bool rollover)
{
	LV_UNUSED(indev);
	LV_UNUSED(rollover);
}

/* Settings › About: the page's uptime, the memory WebAssembly has */
int sysinfo(struct sysinfo *info)
{
	memset(info, 0, sizeof(*info));
	info->uptime = (long)(lv_tick_get() / 1000);
	info->totalram = (unsigned long)__builtin_wasm_memory_size(0) * 65536;
	info->mem_unit = 1;
	return 0;
}

/* ALSA: no sound card (opening one fails, as on a board without) */

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
	(void)name, (void)stream, (void)mode;
	*pcm = NULL;
	return -ENODEV;
}

int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access,
                       unsigned int channels, unsigned int rate, int soft_resample, unsigned int latency)
{
	(void)pcm, (void)format, (void)access, (void)channels, (void)rate, (void)soft_resample, (void)latency;
	return -ENODEV;
}

int snd_pcm_close(snd_pcm_t *pcm) { (void)pcm; return 0; }
int snd_pcm_start(snd_pcm_t *pcm) { (void)pcm; return -ENODEV; }
int snd_pcm_drop(snd_pcm_t *pcm) { (void)pcm; return -ENODEV; }
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) { (void)pcm; return SND_PCM_STATE_DISCONNECTED; }
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent) { (void)pcm, (void)silent; return err; }
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm) { (void)pcm; return -ENODEV; }

int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay)
{
	(void)pcm;
	*delay = 0;
	return -ENODEV;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t size)
{
	(void)pcm, (void)buf, (void)size;
	return -ENODEV;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t size)
{
	(void)pcm, (void)buf, (void)size;
	return -ENODEV;
}

const char *snd_strerror(int errnum)
{
	return errnum == -ENODEV ? "no sound card in the browser" : strerror(-errnum);
}

/* The control and mixer interfaces: the alloca'd structures are opaque bytes here */
size_t snd_ctl_elem_id_sizeof(void) { return 64; }
size_t snd_ctl_elem_info_sizeof(void) { return 64; }
size_t snd_ctl_elem_value_sizeof(void) { return 64; }
size_t snd_mixer_selem_id_sizeof(void) { return 64; }

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
	(void)name, (void)mode;
	*ctl = NULL;
	return -ENODEV;
}

int snd_ctl_close(snd_ctl_t *ctl) { (void)ctl; return 0; }
void snd_ctl_elem_id_clear(snd_ctl_elem_id_t *id) { (void)id; }
void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *id, snd_ctl_elem_iface_t iface) { (void)id, (void)iface; }
void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *id, const char *name) { (void)id, (void)name; }
int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *info) { (void)ctl, (void)info; return -ENODEV; }
void snd_ctl_elem_info_set_id(snd_ctl_elem_info_t *info, const snd_ctl_elem_id_t *id) { (void)info, (void)id; }
void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *value, const snd_ctl_elem_id_t *id) { (void)value, (void)id; }
int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *value) { (void)ctl, (void)value; return -ENODEV; }

int snd_ctl_ascii_value_parse(snd_ctl_t *ctl, snd_ctl_elem_value_t *dst, snd_ctl_elem_info_t *info,
                              const char *value)
{
	(void)ctl, (void)dst, (void)info, (void)value;
	return -ENODEV;
}

int snd_mixer_open(snd_mixer_t **mixer, int mode)
{
	(void)mode;
	*mixer = NULL;
	return -ENODEV;
}

int snd_mixer_attach(snd_mixer_t *mixer, const char *name) { (void)mixer, (void)name; return -ENODEV; }
int snd_mixer_close(snd_mixer_t *mixer) { (void)mixer; return 0; }
int snd_mixer_load(snd_mixer_t *mixer) { (void)mixer; return -ENODEV; }

int snd_mixer_selem_register(snd_mixer_t *mixer, struct snd_mixer_selem_regopt *options,
                             snd_mixer_class_t **classp)
{
	(void)mixer, (void)options, (void)classp;
	return -ENODEV;
}

snd_mixer_elem_t *snd_mixer_find_selem(snd_mixer_t *mixer, const snd_mixer_selem_id_t *id)
{
	(void)mixer, (void)id;
	return NULL;
}

void snd_mixer_selem_id_set_name(snd_mixer_selem_id_t *id, const char *name) { (void)id, (void)name; }
int snd_mixer_selem_set_playback_switch_all(snd_mixer_elem_t *elem, int value) { (void)elem, (void)value; return -ENODEV; }
int snd_mixer_selem_set_playback_volume_all(snd_mixer_elem_t *elem, long value) { (void)elem, (void)value; return -ENODEV; }
