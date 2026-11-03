/*
 * External apps: programs that are not part of the firmware, each in a folder
 * /var/lib/apps/<id>/ (on the data partition: updates keep them) with
 * - app.json: {"name": "Doom", "exec": "doom", "args": ["-iwad", "freedoom1.wad"],
 *   "color": "#b0281a", "symbol": "D"}; all optional except the file itself (name,
 *   exec: the id; symbol: the name's first letter; color: gray)
 * - icon.png (optional): shown instead of the colored square, at 128 px in the
 *   menu; its corners transparent (`just app` renders apps/<name>/icon.svg)
 * They are found at startup (`just app` restarts phone-ui) and listed after the
 * built-in apps, by name.
 *
 * An app has the display and the input devices to itself, as with a VT switch:
 * phone-ui closes its input devices, drops DRM master and waits. The app opens
 * /dev/dri/card0 (it becomes master) and /dev/input/event*, and draws with KMS
 * (SDL's kmsdrm backend, dumb buffers, GBM/EGL). When it exits, phone-ui is master
 * again, its next frame sets the mode, and it opens the input devices again.
 *
 * The app runs as root in its folder, in its own process group (all of it ends
 * with it), with <folder>/lib in LD_LIBRARY_PATH; its output goes to phone-ui's
 * log. What it keeps (settings, saves) goes in $APP_DATA, /var/lib/app-data/<id>:
 * installing it again replaces its folder. Holding Power for 2 s ends it: SIGTERM,
 * then SIGKILL 3 s later. It also gets SIGTERM when phone-ui ends, which could not
 * have the display otherwise.
 *
 * Plugin apps are shared libraries instead ("plugin": "telegram.so" in app.json, no
 * exec): phone-ui loads them at startup and they run in its process with its UI,
 * like the built-in apps. They are built against the headers phone-ui installs in
 * Buildroot's staging (ui.h, json.h, LVGL's, and abi.h: UI_ABI, a hash of them) and
 * call into phone-ui, which exports its functions and the whole of LVGL. A plugin
 * defines `const char app_abi[] = UI_ABI;` and `void app_open(void)`; one built for
 * other headers is not loaded (the log says so: `just app` builds it again). Its own
 * libraries go in <folder>/lib, its runpath ($ORIGIN/lib).
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <png.h>
#include <xf86drm.h>

#include "abi.h"
#include "json.h"
#include "ui.h"
#include "src/drivers/display/drm/lv_linux_drm_egl_private.h"

#define APPS_DIR "/var/lib/apps"
#define DATA_DIR "/var/lib/app-data"
#define MANIFEST_MAX 65536
#define HOLD_MS 2000 /* Power held this long ends the app */
#define KILL_MS 3000 /* after SIGTERM */

extern char **environ;

/* app.json in `dir`, NULL if missing or not valid */
static struct json *manifest_read(const char *dir)
{
	char path[PATH_MAX + 16];
	snprintf(path, sizeof(path), "%s/app.json", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *text = malloc(MANIFEST_MAX + 1);
	size_t n = fread(text, 1, MANIFEST_MAX, f);
	fclose(f);
	text[n] = '\0';
	struct json *m = json_parse(text);
	free(text);
	if (m && m->kind != JSON_OBJECT) {
		json_free(m);
		m = NULL;
	}
	if (!m)
		printf("phone-ui: %s is not valid JSON\n", path);
	return m;
}

/* icon.png in `dir` as a texture, 0 if there is none */
static unsigned icon_load(const char *dir)
{
	char path[PATH_MAX + 16];
	snprintf(path, sizeof(path), "%s/icon.png", dir);
	png_image img = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_file(&img, path))
		return 0;
	/* B, G, R, A: the texture shader swaps red and blue back */
	img.format = PNG_FORMAT_BGRA;
	uint8_t *pixels = malloc(PNG_IMAGE_SIZE(img));
	unsigned tex = 0;
	if (pixels && png_image_finish_read(&img, NULL, pixels, 0, NULL))
		tex = ui_texture_create(pixels, img.width, img.height);
	else
		png_image_free(&img);
	free(pixels);
	return tex;
}

/* The first character of `name` (UTF-8), malloc'd */
static char *first_char(const char *name)
{
	size_t n = 1;
	while (name[0] && (name[n] & 0xc0) == 0x80)
		n++;
	return strndup(name, n);
}

/* A plugin app's open function, NULL if it cannot be used (log says why) */
static void (*plugin_load(const char *dir, const char *file))(void)
{
	char path[PATH_MAX + 64];
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("phone-ui: %s\n", dlerror());
		return NULL;
	}
	const char *abi = dlsym(lib, "app_abi");
	void (*open)(void) = (void (*)(void))dlsym(lib, "app_open");
	if (!abi || strcmp(abi, UI_ABI) || !open) {
		printf("phone-ui: %s was built for another phone-ui (%s, this one %s): `just app` again\n",
		       path, abi ? abi : "no app_abi", UI_ABI);
		dlclose(lib);
		return NULL;
	}
	return open;
}

static int app_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct ui_app *)a)->name, ((const struct ui_app *)b)->name);
}

void external_scan(void)
{
	DIR *d = opendir(APPS_DIR);
	if (!d)
		return;
	unsigned builtin = ui_app_count, count = builtin;
	struct ui_app *apps = malloc(count * sizeof(*apps));
	memcpy(apps, ui_apps, count * sizeof(*apps));

	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), APPS_DIR "/%s", e->d_name);
		struct json *m = manifest_read(dir);
		if (!m)
			continue;
		const char *name = json_str(json_get(m, "name"), e->d_name);
		const char *symbol = json_str(json_get(m, "symbol"), NULL);
		const char *color = json_str(json_get(m, "color"), "#636366");
		const char *plugin = json_str(json_get(m, "plugin"), NULL);
		void (*open)(void) = plugin ? plugin_load(dir, plugin) : NULL;
		if (plugin && !open) {
			json_free(m);
			continue;
		}

		apps = realloc(apps, (count + 1) * sizeof(*apps));
		apps[count++] = (struct ui_app){
			.name = strdup(name),
			.symbol = symbol ? strdup(symbol) : first_char(name),
			.color = strtoul(color + (color[0] == '#'), NULL, 16),
			.open = open,
			.dir = strdup(dir),
			.icon = icon_load(dir),
		};
		printf("phone-ui: %s %s in %s\n", plugin ? "plugin app" : "external app", name, dir);
		json_free(m);
	}
	closedir(d);

	qsort(apps + builtin, count - builtin, sizeof(*apps), app_cmp);
	ui_apps = apps;
	ui_app_count = count;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The input device with the Power key (the PMIC's), -1 if none */
static int power_key_open(void)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return -1;
	int fd = -1;
	struct dirent *e;
	while (fd < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
		    !(keys[KEY_POWER / (8 * sizeof(long))] >> (KEY_POWER % (8 * sizeof(long))) & 1)) {
			close(fd);
			fd = -1;
		}
	}
	closedir(d);
	return fd;
}

/* Until `pid` exits; holding Power ends it */
static void app_wait(pid_t pid)
{
	int pidfd = syscall(SYS_pidfd_open, pid, 0);
	int key = power_key_open();
	uint64_t pressed = 0, termed = 0; /* when, 0: not */
	bool killed = false;

	while (pidfd >= 0) {
		uint64_t now = now_ms();
		int timeout = -1;
		if (termed && !killed)
			timeout = termed + KILL_MS > now ? termed + KILL_MS - now : 0;
		else if (pressed && !termed)
			timeout = pressed + HOLD_MS > now ? pressed + HOLD_MS - now : 0;

		struct pollfd fds[2] = { { pidfd, POLLIN, 0 }, { key, POLLIN, 0 } };
		if (poll(fds, key >= 0 ? 2 : 1, timeout) < 0 && errno != EINTR)
			break;
		if (fds[0].revents)
			break;
		struct input_event ev;
		if (fds[1].revents & POLLIN)
			while (read(key, &ev, sizeof(ev)) == sizeof(ev))
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value != 2) /* not repeats */
					pressed = ev.value ? now_ms() : 0;

		now = now_ms();
		if (pressed && !termed && now - pressed >= HOLD_MS) {
			printf("phone-ui: Power held, ending the app\n");
			ui_vibrate(60);
			kill(-pid, SIGTERM);
			termed = now;
		} else if (termed && !killed && now - termed >= KILL_MS) {
			printf("phone-ui: the app did not end, killing it\n");
			kill(-pid, SIGKILL);
			killed = true;
		}
	}
	if (key >= 0)
		close(key);
	if (pidfd >= 0)
		close(pidfd);

	int status;
	if (waitpid(pid, &status, 0) == pid) {
		if (WIFEXITED(status))
			printf("phone-ui: the app exited with %d\n", WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			printf("phone-ui: the app ended by signal %d\n", WTERMSIG(status));
	}
	/* What it started too: nothing may keep the display */
	kill(-pid, SIGKILL);
}

/* Runs the app, phone-ui's display and input given up meanwhile */
static void run_cb(void *data)
{
	const struct ui_app *app = data;
	struct json *m = manifest_read(app->dir);
	if (!m)
		return;

	/* Command line and environment, made before forking (only exec after) */
	char exec[PATH_MAX], lib[PATH_MAX + 32], appdata[PATH_MAX + 16];
	const char *base = strrchr(app->dir, '/') + 1;
	snprintf(exec, sizeof(exec), "%s/%s", app->dir, json_str(json_get(m, "exec"), base));
	snprintf(lib, sizeof(lib), "LD_LIBRARY_PATH=%s/lib", app->dir);
	snprintf(appdata, sizeof(appdata), "APP_DATA=" DATA_DIR "/%s", base);
	mkdir(DATA_DIR, 0755);
	mkdir(appdata + 9, 0755);
	struct json *args = json_get(m, "args");
	unsigned argc = 1;
	for (struct json *a = args ? args->child : NULL; a; a = a->next)
		argc++;
	char **argv = calloc(argc + 1, sizeof(*argv));
	argc = 0;
	argv[argc++] = exec;
	for (struct json *a = args ? args->child : NULL; a; a = a->next)
		argv[argc++] = (char *)json_str(a, "");
	unsigned envc = 0;
	while (environ[envc])
		envc++;
	char **envp = calloc(envc + 3, sizeof(*envp));
	envc = 0;
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0 && strncmp(*e, "APP_DATA=", 9) != 0)
			envp[envc++] = *e;
	envp[envc++] = lib;
	envp[envc++] = appdata;
	static const char exec_failed[] = "phone-ui: cannot run the app\n";
	int max_fd = sysconf(_SC_OPEN_MAX); /* phone-ui's files closed in the app */

	printf("phone-ui: starting %s (%s)\n", app->name, exec);
	lv_display_t *disp = lv_display_get_default();
	lv_drm_ctx_t *drm = lv_display_get_driver_data(disp);

	/* Its input alone: phone-ui's devices closed, they are opened again after */
	lv_evdev_discovery_stop();
	lv_indev_t *indev;
	while ((indev = lv_indev_get_next(NULL)))
		lv_indev_delete(indev);
	ui_cpu_boost(false);
	if (drmDropMaster(drm->fd) < 0)
		perror("phone-ui: dropping DRM master");

	fflush(NULL);
	pid_t parent = getpid(), pid = fork();
	if (pid == 0) {
		/* Ends with phone-ui (restarted, crashed): it would keep the display */
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(1);
		setpgid(0, 0);
		sigset_t all;
		sigemptyset(&all);
		sigprocmask(SIG_SETMASK, &all, NULL);
		for (int s = 1; s < NSIG; s++)
			signal(s, SIG_DFL);
		if (chdir(app->dir) == 0) {
			for (int fd = 3; fd < max_fd; fd++)
				close(fd);
			execve(exec, argv, envp);
		}
		if (write(STDERR_FILENO, exec_failed, sizeof(exec_failed) - 1) < 0)
			_exit(126);
		_exit(127);
	}
	if (pid > 0) {
		setpgid(pid, pid); /* also here: kill(-pid) may come before the child's */
		app_wait(pid);
	} else {
		perror("phone-ui: fork");
	}

	if (drmSetMaster(drm->fd) < 0)
		perror("phone-ui: DRM master again");
	drm->crtc_isset = false; /* the next frame sets the mode: the app may have changed it */
	ui_input_start();
	ui_status_update();
	lv_obj_invalidate(lv_screen_active());

	free(argv);
	free(envp);
	json_free(m);
}

void external_open(const struct ui_app *app)
{
	/* Out of the click that opened it: its input device is closed meanwhile */
	lv_async_call(run_cb, (void *)app);
}
