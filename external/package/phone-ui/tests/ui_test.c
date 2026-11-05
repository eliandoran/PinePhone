/*
 * phone-ui on the host, see ui_test.h. Linked with all of phone-ui but main.c, whose
 * startup it does here on its own display.
 */

#define _XOPEN_SOURCE 700 /* nftw */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <ftw.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui_test.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h" /* the keys */

#define WIDTH 720
#define HEIGHT 1440
#define FRAME_MS 16
/* Made by the Makefile in the sandbox's /var/lib and /sys: never empty other ones */
#define MARK ".phone-ui-tests"

#define BATTERY UI_BATTERY "/"
#define PMIC_ADC "/sys/devices/platform/axp813-adc/iio:device0"

/* The keyboard's mode keys (keyboard.c) */
#define KB_SHIFT LV_SYMBOL_UP
#define KB_TO_SYMBOLS "?123"
#define KB_TO_MORE "=\\<"
#define KB_DOUBLE_MS 400 /* shift twice within this: caps lock */

static lv_display_t *disp;

/* The phone's buttons as two lv_evdev key devices: Power (the PMIC's) and the volume
 * keys (gpio-keys), each with the last key it reported */
struct button_dev {
	lv_indev_t *indev;
	uint32_t key;
	bool pressed;
};
static struct button_dev power_dev, volume_dev;

/* system()'s commands (power.c: poweroff, reboot) are fakes first in PATH, in the
 * sandbox's /tmp: the real ones could reach the host's init */
#define FAKE_BIN "/tmp/ui-test-bin"
#define FAKE_RAN FAKE_BIN "/ran"
#define FAKE_FAIL FAKE_BIN "/fail"
#define MOBILE_DATA_SCRIPT "/tmp/mobile-data.sh" /* modem.conf's data_script= */
#define MOBILE_DATA_LOG "/tmp/mobile-data.log"
#define CALL_AUDIO_SCRIPT "/tmp/call-audio.sh" /* modem.conf's audio_script= */
#define CALL_AUDIO_LOG "/tmp/call-audio.log"

/* main.c's, without the hardware: the emulated input devices are made at startup */
void ui_input_start(void)
{
}

void ui_cpu_boost(bool on)
{
	LV_UNUSED(on);
}

/* OpenGL ES 2 (the Mali-400's) without a window or GPU: Mesa's surfaceless platform */
static void gl_init(void)
{
	EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint n = 0;
	EGLConfig config;
	static const EGLint config_attrs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE,
	};
	static const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = EGL_NO_CONTEXT;
	if (eglInitialize(dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_ES_API) &&
	    eglChooseConfig(dpy, config_attrs, &config, 1, &n) && n)
		ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "ui_test: no EGL context (EGL error %#x)\n", eglGetError());
		exit(EXIT_FAILURE);
	}
	/* LVGL calls GL through glad, loaded by its own EGL contexts otherwise */
	extern int gladLoadGLES2(void *load);
	if (!gladLoadGLES2((void *)eglGetProcAddress)) {
		fprintf(stderr, "ui_test: glad cannot load GLES2\n");
		exit(EXIT_FAILURE);
	}
}

static int remove_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	LV_UNUSED(st);
	LV_UNUSED(type);
	if (ftw->level > 0 && strcmp(path + ftw->base, MARK) != 0 && remove(path) != 0)
		perror(path);
	return 0;
}

/* The sandbox's directory, emptied */
static void empty(const char *dir)
{
	char mark[64];
	snprintf(mark, sizeof(mark), "%s/" MARK, dir);
	if (access(mark, F_OK) != 0) {
		fprintf(stderr, "ui_test: not in the tests' sandbox (no %s): run them with `just test`\n", mark);
		exit(EXIT_FAILURE);
	}
	nftw(dir, remove_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static bool reset;

/* A PinePhone's sysfs, as much as phone-ui reads of it: the status bar shows 76%,
 * discharging; Settings' pages find a backlight at half, a battery, temperatures */
void ui_test_reset(void)
{
	empty("/var/lib");
	empty("/sys");
	ui_test_board();
}

void ui_test_board(void)
{
	reset = true;
	/* The sensors' buffers of an earlier test: /dev is the sandbox's */
	remove("/dev/iio:device1");
	remove("/dev/iio:device2");
	/* An earlier test's cameras (fake_camera.c, made by ui_test_camera()) */
	remove("/dev/video0");
	remove("/dev/media0");
	for (int i = 0; i < 3; i++) {
		char node[32];
		snprintf(node, sizeof(node), "/dev/v4l-subdev%d", i);
		remove(node);
	}
	/* The display's scanout (fake_drm.c), always there */
	ui_test_write("/dev/dri/card0", "");
	/* An earlier test's modem (fake_modem.c), its pty gone with that process; /tmp is the sandbox's */
	remove("/tmp/modem");
	/* Booted again: the splash shows at the first ui_splash() (splash.o's MARKER) */
	remove("/tmp/phone-ui.splash");
	/* The board's modem (MODEM_CONF of modem.o), the PinePhone's on fake_modem.c's port,
	 * its mobile data script one that logs its arguments (ui_test_mobile_data_ran()) */
	ui_test_write("/tmp/modem.conf", "provider=at\nport=/tmp/modem\nquirks=quectel\n"
	              "data=wwan0\ndata_script=" MOBILE_DATA_SCRIPT "\naudio_script=" CALL_AUDIO_SCRIPT "\n");
	ui_test_write(MOBILE_DATA_SCRIPT, "#!/bin/sh\necho \"$*\" >> " MOBILE_DATA_LOG "\n");
	chmod(MOBILE_DATA_SCRIPT, 0755);
	remove(MOBILE_DATA_LOG);
	ui_test_write(CALL_AUDIO_SCRIPT, "#!/bin/sh\necho \"$*\" >> " CALL_AUDIO_LOG "\n");
	chmod(CALL_AUDIO_SCRIPT, 0755);
	remove(CALL_AUDIO_LOG);

	ui_test_write("/sys/class/backlight/backlight/max_brightness", "1000\n");
	ui_test_write("/sys/class/backlight/backlight/brightness", "500\n");
	ui_test_write("/sys/class/backlight/backlight/bl_power", "0\n");

	/* The USB input, unplugged (axp20x_usb_power) */
	ui_test_write("/sys/class/power_supply/axp20x-usb/online", "0\n");
	ui_test_write("/sys/class/power_supply/axp20x-usb/input_current_limit", "500000\n");

	ui_test_write(BATTERY "present", "1\n");
	ui_test_write(BATTERY "capacity", "76\n");
	ui_test_write(BATTERY "status", "Discharging\n");
	ui_test_write(BATTERY "health", "Good\n");
	ui_test_write(BATTERY "technology", "Li-ion\n");
	ui_test_write(BATTERY "charge_full_design", "2750000\n");
	ui_test_write(BATTERY "voltage_now", "3850000\n");
	ui_test_write(BATTERY "current_now", "-312000\n");

	ui_test_write("/sys/class/thermal/thermal_zone0/temp", "41234\n");
	ui_test_write("/sys/class/thermal/thermal_zone0/trip_point_0_temp", "75000\n");
	ui_test_write("/sys/class/thermal/thermal_zone1/temp", "38000\n");
	ui_test_write("/sys/class/thermal/thermal_zone2/temp", "39600\n");

	/* Found by its real path */
	ui_test_write(PMIC_ADC "/in_temp_raw", "1907\n");
	ui_test_write(PMIC_ADC "/in_temp_offset", "-1447\n");
	ui_test_write(PMIC_ADC "/in_temp_scale", "100\n");
	/* Channel 0 is the TS pin (the battery's thermistor): 300 mV */
	ui_test_write(PMIC_ADC "/in_voltage0_raw", "375\n");
	ui_test_write(PMIC_ADC "/in_voltage0_scale", "0.800000\n");
	ui_test_write("/sys/bus/iio/devices/.keep", "");
	remove("/sys/bus/iio/devices/iio:device0"); /* the simulator's /sys is kept */
	if (symlink(PMIC_ADC, "/sys/bus/iio/devices/iio:device0") != 0)
		perror("ui_test: iio:device0");
	remove("/sys/bus/iio/devices/.keep");

	ui_test_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1152000\n");
	ui_test_write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "schedutil\n");
}


/* Motion sensors: the PinePhone's MPU6050 (accelerometer, and the trigger) and
 * AF8133J (magnetometer) as IIO devices, their buffers files in the sandbox's /dev */
#define IIO_DEVICES "/sys/bus/iio/devices"
#define ACCEL_DEV "iio:device1"
#define MAGN_DEV "iio:device2"
#define ACCEL_LSB_G 16384.0f
#define MAGN_GAUSS_LSB 0.0001f
/* Mount matrices, to the phone's axes (rows): the accelerometer's as the kernel
 * patch 0016 corrects it (the chip turned a quarter, its x to the top edge); the
 * readings point up, as the MPU6050's do (+1 g on z lying face up) */
static const float accel_mount[3][3] = { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
static const float magn_mount[3][3] = { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } };

static void sensor_files(const char *dev, const char *type, const float m[3][3], bool device_matrix)
{
	static const char axis[3] = { 'x', 'y', 'z' };
	char path[160], text[128];
	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/in_%s_%c_raw", dev, type, axis[i]);
		ui_test_write(path, "0\n");
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_%s_%c_en", dev, type, axis[i]);
		ui_test_write(path, "0\n");
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_%s_%c_index", dev, type, axis[i]);
		snprintf(text, sizeof(text), "%d\n", i);
		ui_test_write(path, text);
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_%s_%c_type", dev, type, axis[i]);
		ui_test_write(path, "le:s16/16>>0\n");
	}
	/* A channel the compass leaves out */
	snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_timestamp_en", dev);
	ui_test_write(path, "1\n");
	snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_timestamp_index", dev);
	ui_test_write(path, "3\n");
	snprintf(path, sizeof(path), IIO_DEVICES "/%s/scan_elements/in_timestamp_type", dev);
	ui_test_write(path, "le:s64/64>>0\n");
	static const char *const attrs[] = {
		"buffer0/enable", "buffer0/length", "trigger/current_trigger", "sampling_frequency",
	};
	for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/%s", dev, attrs[i]);
		ui_test_write(path, "0\n");
	}
	snprintf(text, sizeof(text), "%g, %g, %g; %g, %g, %g; %g, %g, %g\n", m[0][0], m[0][1], m[0][2],
	         m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
	if (device_matrix)
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/in_mount_matrix", dev);
	else
		snprintf(path, sizeof(path), IIO_DEVICES "/%s/in_%s_mount_matrix", dev, type);
	ui_test_write(path, text);
	snprintf(path, sizeof(path), "/dev/%s", dev);
	ui_test_write(path, "");
}

void ui_test_sensors(bool accelerometer, bool magnetometer)
{
	if (accelerometer) {
		sensor_files(ACCEL_DEV, "accel", accel_mount, false);
		ui_test_write(IIO_DEVICES "/" ACCEL_DEV "/name", "mpu6050\n");
		ui_test_write(IIO_DEVICES "/" ACCEL_DEV "/in_accel_scale", "0.000598\n"); /* m/s² */
		ui_test_write(IIO_DEVICES "/trigger0/name", "mpu6050-dev1\n");
	}
	if (magnetometer) {
		sensor_files(MAGN_DEV, "magn", magn_mount, true);
		ui_test_write(IIO_DEVICES "/" MAGN_DEV "/name", "af8133j\n");
		char scale[32];
		snprintf(scale, sizeof(scale), "%g\n", MAGN_GAUSS_LSB);
		ui_test_write(IIO_DEVICES "/" MAGN_DEV "/in_magn_scale", scale);
	}
}

/* A phone-axes vector as the sensor reads it: the mount matrix's transpose */
static void sensor_raw(const float m[3][3], const float v[3], float lsb, int16_t raw[3])
{
	for (int i = 0; i < 3; i++)
		raw[i] = (int16_t)lroundf((m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2]) * lsb);
}

static void records_append(const char *node, const int16_t raw[3], unsigned n)
{
	FILE *f = fopen(node, "ab");
	if (!f)
		return;
	for (unsigned i = 0; i < n; i++)
		for (int j = 0; j < 3; j++) {
			uint8_t le[2] = { (uint8_t)(raw[j] & 0xff), (uint8_t)((uint16_t)raw[j] >> 8) };
			fwrite(le, 1, 2, f);
		}
	fclose(f);
}

void ui_test_sensors_feed(const float up[3], const float field[3], unsigned samples)
{
	int16_t raw[3];
	char path[160], text[32];
	sensor_raw(accel_mount, up, ACCEL_LSB_G, raw);
	records_append("/dev/" ACCEL_DEV, raw, samples);
	for (int i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), IIO_DEVICES "/" ACCEL_DEV "/in_accel_%c_raw", "xyz"[i]);
		snprintf(text, sizeof(text), "%d\n", raw[i]);
		ui_test_write(path, text);
	}
	sensor_raw(magn_mount, field, 1 / MAGN_GAUSS_LSB, raw);
	records_append("/dev/" MAGN_DEV, raw, samples);
}

const char *ui_test_sensor_attr(const char *dev_attr)
{
	static char buf[128];
	char path[200];
	snprintf(path, sizeof(path), IIO_DEVICES "/%s", dev_attr);
	if (!ui_read_line(path, buf, sizeof(buf)))
		buf[0] = '\0';
	return buf;
}
/* LVGL's errors fail the test (test.h asks test_errors()), e.g. a draw task the
 * OpenGL ES draw unit does not support: on the phone it is not drawn either. They
 * are printed (this callback replaces LVGL's printing). */
static unsigned lv_errors;

static void log_cb(lv_log_level_t level, const char *buf)
{
	if (level != LV_LOG_LEVEL_ERROR)
		return;
	fprintf(stderr, "  %s", buf);
	lv_errors++;
}

unsigned test_errors(void)
{
	if (lv_errors)
		fprintf(stderr, "  ui_test: LVGL logged %u error%s\n", lv_errors, lv_errors > 1 ? "s" : "");
	return lv_errors;
}

/* As lv_evdev reports the keys it does not map */
static void button_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct button_dev *b = lv_indev_get_user_data(indev);
	data->key = b->key;
	data->state = b->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Taken by volume.c, as main.c does for key devices */
static void button_dev_create(struct button_dev *b)
{
	b->indev = lv_indev_create();
	lv_indev_set_type(b->indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(b->indev, button_read_cb);
	lv_indev_set_user_data(b->indev, b);
	lv_timer_pause(lv_indev_get_read_timer(b->indev)); /* read by ui_test_frame() */
	ui_volume_keys(b->indev);
}


/* mpg123 --remote (music.c) as a script: it logs the commands it gets and answers
 * from files the test writes: the position, the end of the song, being killed */
#define MPG123_LOG FAKE_BIN "/mpg123.log"
#define MPG123_POS FAKE_BIN "/mpg123.pos"
#define MPG123_END FAKE_BIN "/mpg123.end"
#define MPG123_KILL FAKE_BIN "/mpg123.kill"

static const char mpg123_script[] =
	"#!/bin/sh\n"
	"while read -r cmd arg; do\n"
	"	echo \"$cmd${arg:+ $arg}\" >> " MPG123_LOG "\n"
	"	[ -e " MPG123_KILL " ] && exit 0\n"
	"	case $cmd in\n"
	"	LOAD|LOADPAUSED)\n"
	"		if [ -e \"$arg\" ]; then echo '@P 2'; else echo '@E No stream opened'; echo '@P 0'; fi ;;\n"
	"	PAUSE) echo '@P 1' ;;\n"
	"	STOP) echo '@P 0' ;;\n"
	"	SAMPLE)\n"
	"		if [ -e " MPG123_END " ]; then rm -f " MPG123_END "; echo '@P 3'\n"
	"		else echo \"@SAMPLE $(cat " MPG123_POS " 2>/dev/null || echo 0 0)\"; fi ;;\n"
	"	esac\n"
	"done\n";

/* A little real time per frame: the script answers in it */
static void mpg123_wait(void)
{
	ui_test_wait(FRAME_MS);
	nanosleep(&(struct timespec){ 0, 2000000 }, NULL);
}

/* Real time, for the waits on processes: under load (coverage, CI) frames come faster
 * than they answer, so a wait gives up only after `ms` of the tests' time and of this */
int __real_clock_gettime(clockid_t id, struct timespec *ts);
static uint64_t real_ms(void)
{
	struct timespec ts;
	__real_clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

bool ui_test_mpg123_got(const char *command, uint32_t ms)
{
	uint64_t start = real_ms();
	for (uint32_t t = 0; t <= ms || real_ms() - start <= ms; t += FRAME_MS) {
		FILE *f = fopen(MPG123_LOG, "r");
		char line[600];
		bool found = false;
		while (f && !found && fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = '\0';
			found = strcmp(line, command) == 0;
		}
		if (f)
			fclose(f);
		if (found)
			return true;
		mpg123_wait();
	}
	fprintf(stderr, "  ui_test: mpg123 did not get \"%s\"; it got:\n", command);
	FILE *f = fopen(MPG123_LOG, "r");
	char line[600];
	while (f && fgets(line, sizeof(line), f))
		fprintf(stderr, "    %s", line);
	if (f)
		fclose(f);
	return false;
}

/* A board script's log after the line `args` (consumed up to it), false if it has none */
static bool script_take(const char *log, const char *args)
{
	char text[4096], line[256];
	size_t n = 0;
	bool found = false;
	FILE *f = fopen(log, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (found) {
			n += (size_t)snprintf(text + n, sizeof(text) - n, "%s", line);
			if (n >= sizeof(text))
				n = sizeof(text) - 1;
			continue;
		}
		line[strcspn(line, "\n")] = '\0';
		found = strcmp(line, args) == 0;
	}
	if (f)
		fclose(f);
	if (found)
		ui_test_write(log, n ? text : "");
	return found;
}

/* Waits for the script logging into `log` to have run with `args` */
static bool script_ran(const char *what, const char *log, const char *args, uint32_t ms)
{
	uint64_t start = real_ms();
	for (uint32_t t = 0; t <= ms || real_ms() - start <= ms; t += FRAME_MS) {
		if (script_take(log, args))
			return true;
		ui_test_wait(FRAME_MS);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL); /* the script is a process */
	}
	fprintf(stderr, "  ui_test: the %s script did not run \"%s\"; it ran:\n", what, args);
	FILE *f = fopen(log, "r");
	char line[256];
	while (f && fgets(line, sizeof(line), f))
		fprintf(stderr, "    %s", line);
	if (f)
		fclose(f);
	return false;
}

bool ui_test_mobile_data_ran(const char *args, uint32_t ms)
{
	return script_ran("mobile data", MOBILE_DATA_LOG, args, ms);
}

bool ui_test_call_audio_ran(const char *args, uint32_t ms)
{
	return script_ran("call audio", CALL_AUDIO_LOG, args, ms);
}

void ui_test_mpg123_forget(void)
{
	remove(MPG123_LOG);
}

void ui_test_mpg123_at(long long sample, long long total)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%lld %lld\n", sample, total);
	ui_test_write(MPG123_POS, buf);
}

void ui_test_mpg123_end(void)
{
	ui_test_write(MPG123_END, "");
}

void ui_test_mpg123_kill(void)
{
	ui_test_write(MPG123_KILL, "");
}
static void fake_commands(void)
{
	static const char *const names[] = { "poweroff", "reboot" };
	mkdir(FAKE_BIN, 0755);
	remove(FAKE_RAN);
	remove(FAKE_FAIL);
	remove(MPG123_LOG);
	remove(MPG123_POS);
	remove(MPG123_END);
	remove(MPG123_KILL);
	ui_test_write(FAKE_BIN "/mpg123", mpg123_script);
	chmod(FAKE_BIN "/mpg123", 0755);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[64];
		snprintf(path, sizeof(path), FAKE_BIN "/%s", names[i]);
		ui_test_write(path, "#!/bin/sh\necho \"${0##*/}\" >> " FAKE_RAN "\n[ ! -e " FAKE_FAIL " ]\n");
		chmod(path, 0755);
	}
	const char *path = getenv("PATH");
	char *s = malloc(strlen(FAKE_BIN) + 2 + (path ? strlen(path) : 0));
	sprintf(s, FAKE_BIN ":%s", path ? path : "");
	setenv("PATH", s, 1);
	free(s);
}

const char *ui_test_ran(void)
{
	static char ran[256];
	size_t n = 0;
	FILE *f = fopen(FAKE_RAN, "r");
	if (f) {
		n = fread(ran, 1, sizeof(ran) - 1, f);
		fclose(f);
	}
	ran[n] = '\0';
	return ran;
}

void ui_test_commands_fail(bool fail)
{
	if (fail)
		ui_test_write(FAKE_FAIL, "");
	else
		remove(FAKE_FAIL);
}

static bool started;

void ui_test_start(void)
{
	if (started)
		return;
	started = true;
	if (!reset)
		ui_test_reset();

	/* As main.c */
	setvbuf(stdout, NULL, _IOLBF, 0);
	settings_apply();
	gl_init();
	lv_init();
	lv_log_register_print_cb(log_cb);
	disp = lv_opengles_texture_create(WIDTH, HEIGHT);
	if (!disp) {
		fprintf(stderr, "ui_test: no display\n");
		exit(EXIT_FAILURE);
	}
	lv_display_set_dpi(disp, 269);
	lv_display_delete_refr_timer(disp);
	lv_timer_set_period(lv_anim_get_timer(), 0);
	lv_test_indev_create_all();
	/* The pointer is read once a frame, by ui_test_frame(): a read by its timer too sees no
	 * movement, which ends a gesture (LVGL's sum of the finger's travel) */
	lv_timer_pause(lv_indev_get_read_timer(lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER)));

	fake_commands();
	button_dev_create(&power_dev);
	button_dev_create(&volume_dev);

	ui_init(disp);
	ui_volume_init();
	external_scan();
	home_create();
	ui_test_settle();
}

/* phone-ui's monotonic clocks are the tests' time (the Makefile links UI tests with
 * --wrap=clock_gettime): a stopwatch counts ui_test_wait()'s ms. The wall clock is real
 * unless a test sets it (ui_test_wall_clock(), time() wrapped too): then it runs from
 * there with the tests' time. */
static uint64_t mono_ms = 1000000;
static uint64_t suspended_ms; /* ui_test_suspended()'s: CLOCK_BOOTTIME is mono_ms + it */
static time_t wall_set;   /* 0: the real wall clock */
static uint64_t wall_boot; /* CLOCK_BOOTTIME when it was set */

void ui_test_wall_clock(time_t t)
{
	wall_set = t;
	wall_boot = mono_ms + suspended_ms;
}

void ui_test_suspended(uint32_t ms)
{
	suspended_ms += ms;
}

/* The set wall clock, in ms */
static uint64_t wall_ms(void)
{
	return (uint64_t)wall_set * 1000 + (mono_ms + suspended_ms - wall_boot);
}

int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
	uint64_t ms;
	if (id == CLOCK_MONOTONIC)
		ms = mono_ms;
	else if (id == CLOCK_BOOTTIME)
		ms = mono_ms + suspended_ms;
	else if (wall_set && (id == CLOCK_REALTIME || id == CLOCK_REALTIME_COARSE))
		ms = wall_ms();
	else
		return __real_clock_gettime(id, ts);
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)(ms % 1000) * 1000000;
	return 0;
}

time_t __real_time(time_t *t);

time_t __wrap_time(time_t *t)
{
	if (!wall_set)
		return __real_time(t);
	time_t now = (time_t)(wall_ms() / 1000);
	if (t)
		*t = now;
	return now;
}

static bool pixels_read; /* ui_test_pixels() of the last frame, read back */

/* A pass of main.c's loop, `ms` later (lv_test_fast_forward() does not refresh a
 * display without its refresh timer, which main.c deletes) */
void ui_test_frame(uint32_t ms)
{
	ui_test_modem_sync();
	mono_ms += ms;
	lv_tick_inc(ms);
	lv_timer_handler();
	lv_indev_read(lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER));
	lv_indev_read(power_dev.indev);
	lv_indev_read(volume_dev.indev);
	lv_display_refr_timer(NULL);
	pixels_read = false;
}

void ui_test_button(unsigned code, bool pressed)
{
	struct button_dev *b = code == KEY_POWER ? &power_dev : &volume_dev;
	b->key = code + 0xFFFF; /* lv_evdev's key for the codes it does not map */
	b->pressed = pressed;
	ui_test_wait(FRAME_MS);
}

void ui_test_button_press(unsigned code, uint32_t ms)
{
	ui_test_button(code, true);
	ui_test_wait(ms);
	ui_test_button(code, false);
	ui_test_settle();
}

void ui_test_wait(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += FRAME_MS)
		ui_test_frame(FRAME_MS);
}

void ui_test_settle(void)
{
	ui_test_wait(2 * FRAME_MS);
	for (uint32_t t = 0; lv_anim_count_running() && t < 5000; t += FRAME_MS)
		ui_test_wait(FRAME_MS);
	ui_test_wait(FRAME_MS);
}

/* Objects shown, depth first: the screen on top, then the top layer */
typedef bool (*visit_cb)(lv_obj_t *obj, void *data); /* true: stop there */

static lv_obj_t *visit_tree(lv_obj_t *obj, visit_cb cb, void *data)
{
	/* Scrolled out of view counts: scrolling shows it, a tap scrolls to it */
	if (lv_obj_is_hidden(obj))
		return NULL;
	if (cb(obj, data))
		return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = visit_tree(lv_obj_get_child(obj, (int32_t)i), cb, data);
		if (found)
			return found;
	}
	return NULL;
}

static lv_obj_t *visit(visit_cb cb, void *data)
{
	lv_obj_t *found = visit_tree(lv_screen_active(), cb, data);
	if (!found)
		found = visit_tree(lv_layer_top(), cb, data);
	return found ? found : visit_tree(lv_layer_sys(), cb, data); /* the FPS counter */
}

static bool is_text_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), data) == 0;
}

static bool dump_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	if (lv_obj_check_type(obj, &lv_label_class)) {
		lv_area_t a;
		lv_obj_get_coords(obj, &a);
		fprintf(stderr, "    %4d,%4d  \"%s\"\n", (int)a.x1, (int)a.y1, lv_label_get_text(obj));
	}
	return false;
}

void ui_test_dump(void)
{
	fprintf(stderr, "  shown:\n");
	visit(dump_cb, NULL);
}

lv_obj_t *ui_test_find(const char *text)
{
	return visit(is_text_cb, (void *)text);
}

bool ui_test_shows(const char *text)
{
	return ui_test_find(text) != NULL;
}

struct find_all {
	const char *text;
	lv_obj_t **found;
	unsigned count, max;
};

static bool find_all_cb(lv_obj_t *obj, void *data)
{
	struct find_all *s = data;
	if (is_text_cb(obj, (void *)s->text) && s->count++ < s->max)
		s->found[s->count - 1] = obj;
	return false;
}

unsigned ui_test_find_all(const char *text, lv_obj_t **found, unsigned max)
{
	struct find_all s = { text, found, 0, max };
	visit(find_all_cb, &s);
	return s.count;
}

static bool has_part_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), data);
}

bool ui_test_shows_part(const char *part)
{
	return visit(has_part_cb, (void *)part) != NULL;
}

static bool status_clock_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	if (!lv_obj_check_type(obj, &lv_label_class))
		return false;
	const char *t = lv_label_get_text(obj);
	int h, m;
	char rest;
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	return strlen(t) == 5 && sscanf(t, "%2d:%2d%c", &h, &m, &rest) == 2 && a.y2 <= UI_STATUS_H;
}

bool ui_test_status_bar_clock(void)
{
	return visit(status_clock_cb, NULL) != NULL;
}

/* How far up from obj is what a finger on its middle presses (0: obj itself), -1 if
 * that is not obj or what it is in, or only its screen */
static int tap_distance(lv_obj_t *obj)
{
	lv_obj_t *root = lv_obj_get_screen(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	lv_point_t p = { (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
	lv_obj_t *hit = lv_indev_search_obj(root, &p);
	if (!hit || hit == root)
		return -1;
	int d = 0;
	for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o), d++)
		if (o == hit)
			return d;
	return -1;
}

/* Of the labels showing a text, the one pressed nearest to it: a key's button, not a
 * clickable page around the number it typed */
struct tap_search {
	const char *text;
	lv_obj_t *best;
	int distance;
};

static bool tap_search_cb(lv_obj_t *obj, void *data)
{
	struct tap_search *s = data;
	if (is_text_cb(obj, (void *)s->text)) {
		int d = tap_distance(obj);
		if (d >= 0 && (!s->best || d < s->distance)) {
			s->best = obj;
			s->distance = d;
		}
	}
	return false; /* all of them */
}

static bool is_class_cb(lv_obj_t *obj, void *data)
{
	return lv_obj_check_type(obj, data);
}

lv_obj_t *ui_test_find_widget(const lv_obj_class_t *cls)
{
	return visit(is_class_cb, (void *)cls);
}

struct find_widgets {
	const lv_obj_class_t *cls;
	lv_obj_t **found;
	unsigned count, max;
};

static bool find_widgets_cb(lv_obj_t *obj, void *data)
{
	struct find_widgets *s = data;
	if (lv_obj_check_type(obj, s->cls) && s->count++ < s->max)
		s->found[s->count - 1] = obj;
	return false;
}

unsigned ui_test_find_widgets(const lv_obj_class_t *cls, lv_obj_t **found, unsigned max)
{
	struct find_widgets s = { cls, found, 0, max };
	visit(find_widgets_cb, &s);
	return s.count;
}

static void press_at(int32_t x, int32_t y, uint32_t ms)
{
	lv_test_mouse_move_to(x, y);
	lv_test_mouse_press();
	ui_test_wait(ms);
	lv_test_mouse_release();
	ui_test_settle();
}

void ui_test_swipe(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t ms)
{
	uint32_t frames = ms / FRAME_MS ? ms / FRAME_MS : 1;
	lv_test_mouse_move_to(x1, y1);
	lv_test_mouse_press();
	ui_test_wait(FRAME_MS);
	for (uint32_t i = 1; i <= frames; i++) {
		lv_test_mouse_move_to(x1 + (x2 - x1) * (int32_t)i / (int32_t)frames,
		                      y1 + (y2 - y1) * (int32_t)i / (int32_t)frames);
		ui_test_wait(FRAME_MS);
	}
	lv_test_mouse_release();
	ui_test_settle();
}

void ui_test_tap_at(int32_t x, int32_t y)
{
	press_at(x, y, 3 * FRAME_MS);
}

/* The touchscreen's point under a point of the display, turned or not (LVGL turns
 * touches back counterclockwise: turn.c) */
static void touch_point(int32_t x, int32_t y, int32_t *tx, int32_t *ty)
{
	lv_display_t *d = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(d), h = lv_display_get_vertical_resolution(d);
	switch (lv_display_get_rotation(d)) {
	case LV_DISPLAY_ROTATION_90:
		*tx = h - 1 - y, *ty = x;
		break;
	case LV_DISPLAY_ROTATION_180:
		*tx = w - 1 - x, *ty = h - 1 - y;
		break;
	case LV_DISPLAY_ROTATION_270:
		*tx = y, *ty = w - 1 - x;
		break;
	default:
		*tx = x, *ty = y;
	}
}

/* The finger down on obj's middle for `ms`, then what it started */
static void press(lv_obj_t *obj, uint32_t ms)
{
	lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
	ui_test_wait(FRAME_MS);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int32_t x, y;
	touch_point((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, &x, &y);
	press_at(x, y, ms);
}

void ui_test_tap(lv_obj_t *obj)
{
	press(obj, 3 * FRAME_MS);
}

void ui_test_long_press(lv_obj_t *obj)
{
	press(obj, LV_INDEV_DEF_LONG_PRESS_TIME + 200);
}

static lv_obj_t *must_find(const char *text)
{
	struct tap_search search = { text, NULL, 0 };
	visit(tap_search_cb, &search);
	lv_obj_t *obj = search.best ? search.best : ui_test_find(text);
	if (!obj) {
		fprintf(stderr, "  ui_test: \"%s\" not shown\n", text);
		ui_test_dump();
	}
	return obj;
}

bool ui_test_tap_text(const char *text)
{
	lv_obj_t *obj = must_find(text);
	if (obj)
		ui_test_tap(obj);
	return obj != NULL;
}

bool ui_test_long_press_text(const char *text)
{
	lv_obj_t *obj = must_find(text);
	if (obj)
		ui_test_long_press(obj);
	return obj != NULL;
}

static bool is_field_cb(lv_obj_t *obj, void *data)
{
	if (!lv_obj_check_type(obj, &lv_textarea_class))
		return false;
	const char *placeholder = lv_textarea_get_placeholder_text(obj);
	return strcmp(lv_textarea_get_text(obj), data) == 0 || (placeholder && strcmp(placeholder, data) == 0);
}

bool ui_test_tap_field(const char *text)
{
	lv_obj_t *ta = visit(is_field_cb, (void *)text);
	if (!ta) {
		fprintf(stderr, "  ui_test: no field \"%s\"\n", text);
		ui_test_dump();
		return false;
	}
	ui_test_tap(ta);
	return true;
}

bool ui_test_write(const char *path, const char *text)
{
	char dir[256];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *s = strchr(dir + 1, '/'); s; s = strchr(s + 1, '/')) {
		*s = '\0';
		mkdir(dir, 0755);
		*s = '/';
	}
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		return false;
	}
	fputs(text, f);
	return fclose(f) == 0;
}

bool ui_test_back(void)
{
	return ui_test_tap_text(LV_SYMBOL_LEFT);
}

bool ui_test_open(const char *app)
{
	ui_test_start();
	ui_home();
	ui_test_settle();
	ui_open_menu();
	ui_test_settle();
	return ui_test_tap_text(app);
}

/* Keyboard */

static bool is_keyboard_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	return lv_obj_check_type(obj, &lv_keyboard_class);
}

/* Presses the shown key `key` as a tap does (its selection, then VALUE_CHANGED) */
static bool key_press(lv_obj_t *kb, const char *key)
{
	lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)kb;
	for (uint32_t id = 0; id < bm->btn_cnt; id++) {
		if (lv_buttonmatrix_has_button_ctrl(kb, id, LV_BUTTONMATRIX_CTRL_HIDDEN) ||
		    strcmp(lv_buttonmatrix_get_button_text(kb, id), key) != 0)
			continue;
		lv_buttonmatrix_set_selected_button(kb, id);
		lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, &id);
		ui_test_wait(FRAME_MS);
		return true;
	}
	return false;
}

static bool type_char(lv_obj_t *kb, char c)
{
	char key[2] = { c, '\0' };
	const char *k = c == '\n' ? LV_SYMBOL_NEW_LINE : c == '\b' ? LV_SYMBOL_BACKSPACE : key;
	bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

	/* Letters: shift (not twice in a row: caps lock), back to letters from symbols;
	 * the rest: the symbols, then more of them */
	for (int tries = 0; tries < 4; tries++) {
		if (key_press(kb, k))
			return true;
		if (letter) {
			ui_test_wait(KB_DOUBLE_MS + FRAME_MS);
			if (!key_press(kb, KB_SHIFT) && !key_press(kb, "ABC"))
				break;
		} else if (!key_press(kb, KB_TO_SYMBOLS) && !key_press(kb, KB_TO_MORE)) {
			break;
		}
	}
	fprintf(stderr, "  ui_test: no key for '%c'\n", c);
	return false;
}

static lv_obj_t *keyboard(void)
{
	lv_obj_t *kb = visit(is_keyboard_cb, NULL);
	if (!kb)
		fprintf(stderr, "  ui_test: no keyboard shown\n");
	return kb;
}

bool ui_test_type(const char *text)
{
	lv_obj_t *kb = keyboard();
	if (!kb)
		return false;
	for (; *text; text++)
		if (!type_char(kb, *text))
			return false;
	ui_test_settle();
	return true;
}

bool ui_test_key(const char *key)
{
	lv_obj_t *kb = keyboard();
	if (!kb)
		return false;
	if (!key_press(kb, key)) {
		fprintf(stderr, "  ui_test: no key \"%s\"\n", key);
		return false;
	}
	ui_test_settle();
	return true;
}


/* Button matrices (keyboards, the emoji panel): a button tapped where it is */

struct button_search {
	const char *text;
	lv_obj_t *bm;
	uint32_t id;
};

/* Its middle, on the display */
static lv_point_t button_center(lv_obj_t *bm, uint32_t id)
{
	lv_area_t a = ((lv_buttonmatrix_t *)bm)->button_areas[id], c;
	lv_obj_get_coords(bm, &c);
	return (lv_point_t){ c.x1 + (a.x1 + a.x2) / 2, c.y1 + (a.y1 + a.y2) / 2 };
}

static bool button_search_cb(lv_obj_t *obj, void *data)
{
	struct button_search *s = data;
	if (!lv_obj_has_class(obj, &lv_buttonmatrix_class))
		return false;
	lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)obj;
	for (uint32_t id = 0; id < bm->btn_cnt; id++) {
		if (lv_buttonmatrix_has_button_ctrl(obj, id, LV_BUTTONMATRIX_CTRL_HIDDEN) ||
		    strcmp(lv_buttonmatrix_get_button_text(obj, id), s->text) != 0)
			continue;
		/* Not one under something else (the keys under the emoji panel) */
		lv_point_t p = button_center(obj, id);
		lv_obj_t *parent = lv_obj_get_parent(obj);
		lv_area_t view;
		lv_obj_get_coords(parent, &view);
		bool in_view = p.y >= view.y1 && p.y <= view.y2;
		if (in_view && lv_indev_search_obj(lv_obj_get_screen(obj), &p) != obj)
			continue;
		s->bm = obj;
		s->id = id;
		return true;
	}
	return false;
}

bool ui_test_tap_button(const char *text)
{
	struct button_search s = { text, NULL, 0 };
	visit(button_search_cb, &s);
	if (!s.bm) {
		fprintf(stderr, "  ui_test: no button \"%s\" to tap\n", text);
		return false;
	}
	/* Scrolled to it, in a matrix taller than its parent (the emoji grid) */
	lv_point_t p = button_center(s.bm, s.id);
	lv_obj_t *parent = lv_obj_get_parent(s.bm);
	lv_area_t view;
	lv_obj_get_coords(parent, &view);
	if (p.y < view.y1 + 40 || p.y > view.y2 - 40) {
		lv_obj_scroll_by_bounded(parent, 0, (view.y1 + view.y2) / 2 - p.y, LV_ANIM_OFF);
		ui_test_wait(FRAME_MS);
		p = button_center(s.bm, s.id);
	}
	press_at(p.x, p.y, 3 * FRAME_MS);
	return true;
}
static bool is_focused_ta_cb(lv_obj_t *obj, void *data)
{
	LV_UNUSED(data);
	return lv_obj_check_type(obj, &lv_textarea_class) && lv_obj_has_state(obj, LV_STATE_FOCUSED);
}

lv_obj_t *ui_test_textarea(void)
{
	return visit(is_focused_ta_cb, NULL);
}

const char *ui_test_typed(void)
{
	lv_obj_t *ta = ui_test_textarea();
	return ta ? lv_textarea_get_text(ta) : NULL;
}

const uint8_t *ui_test_pixels(void)
{
	static GLuint fbo;
	static uint8_t px[WIDTH * HEIGHT * 4];
	GLint bound;

	if (pixels_read)
		return px; /* once a frame: a test reads many pixels (ui_test_pixel()) */
	pixels_read = true;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
	if (!fbo)
		glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       lv_opengles_texture_get_texture_id(disp), 0);
	glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_FRAMEBUFFER, bound);
	for (size_t i = 3; i < sizeof(px); i += 4)
		px[i] = 0xff; /* opaque, whatever the texture's alpha */
	return px;
}

lv_color_t ui_test_pixel(int32_t x, int32_t y)
{
	const uint8_t *p = ui_test_pixels() + ((size_t)(HEIGHT - 1 - y) * WIDTH + x) * 4;
	return lv_color_make(p[2], p[1], p[0]);
}

bool ui_test_screenshot(const char *path)
{
	const uint8_t *px = ui_test_pixels();
	png_image image = {
		.version = PNG_IMAGE_VERSION, .width = WIDTH, .height = HEIGHT, .format = PNG_FORMAT_BGRA,
	};
	if (!png_image_write_to_file(&image, path, 0, px, -WIDTH * 4, NULL)) {
		fprintf(stderr, "ui_test: %s: %s\n", path, image.message);
		return false;
	}
	return true;
}

void *ui_test_png(int w, int h, uint32_t rgb, size_t *size)
{
	uint8_t *px = malloc((size_t)w * h * 3);
	for (int i = 0; i < w * h; i++) {
		px[i * 3] = rgb >> 16;
		px[i * 3 + 1] = rgb >> 8;
		px[i * 3 + 2] = rgb;
	}
	png_image image = { .version = PNG_IMAGE_VERSION, .width = w, .height = h, .format = PNG_FORMAT_RGB };
	png_alloc_size_t n = 0;
	void *png = NULL;
	if (png_image_write_to_memory(&image, NULL, &n, 0, px, 0, NULL) && (png = malloc(n)) &&
	    !png_image_write_to_memory(&image, png, &n, 0, px, 0, NULL)) {
		free(png);
		png = NULL;
	}
	free(px);
	*size = png ? n : 0;
	return png;
}

bool ui_test_wait_for(const char *text, uint32_t ms)
{
	uint64_t start = real_ms();
	for (uint32_t t = 0; !ui_test_shows(text); t += FRAME_MS) {
		if (t >= ms && real_ms() - start >= ms) {
			fprintf(stderr, "  ui_test: \"%s\" not shown after %u ms\n", text, ms);
			ui_test_dump();
			return false;
		}
		ui_test_wait(FRAME_MS);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL); /* real time too, for the processes that answer (mpg123's fake) */
	}
	return true;
}

bool ui_test_wait_until(bool (*done)(void), uint32_t ms)
{
	uint64_t start = real_ms();
	for (uint32_t t = 0; !done(); t += FRAME_MS) {
		if (t >= ms && real_ms() - start >= ms)
			return false;
		ui_test_wait(FRAME_MS);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	return true;
}
