/*
 * Browser: pages with WPE WebKit, Safari's engine (JavaScript, current CSS), next to
 * Web Lite's litehtml while we try it. WebKit renders in its own processes
 * (WPEWebProcess, WPENetworkProcess) on the GPU; WPEBackend-fdo hands each frame
 * over as an EGLImage, bound to a texture that an lv_3dtexture shows. Touches go
 * to WebKit as touch events: it scrolls and follows links itself. WebKit runs on
 * GLib's main context, iterated by the screen's timer. Cookies and cache are in
 * /var/lib/web (XDG_DATA_HOME, XDG_CACHE_HOME).
 *
 * A plugin app (phone-ui's external.c): web.so, with the engine in root/ next to
 * it, laid out as in the image it was taken from (see the Makefile). web.so finds
 * the libraries in root/usr/lib through its rpath, they and WebKit's processes
 * through runpaths of their own; the image links WebKit's compiled-in folders (its
 * processes, the fonts, GIO's modules) to root/ (web-engine.sh).
 *
 * Not yet: typing into the page's own text fields (needs an input method context),
 * video and audio (no GStreamer).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "abi.h"
#include "i18n.h"
#include "ui.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/webkit.h>

#define SCALE 1.5f /* device pixels per CSS pixel, as in Web Lite */
#define DIR "/var/lib/web"
#define SEARCH "https://duckduckgo.com/?q="
#define TICK_MS 8
#define ITERATIONS 64 /* GLib dispatches per tick at most */
/* WebKit is Safari's engine: sites serve it their mobile Safari pages */
/* NON-NLS-BEGIN */
#define USER_AGENT "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 " \
	"(KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
/* NON-NLS-END */

/* The start page in the phone's language (g_free() it). The script says JavaScript works
 * with the text in data-works, an attribute: a translation needs no JavaScript quoting. */
static char *home_html(void)
{
	/* NON-NLS-BEGIN: HTML, the texts are its arguments */
	return g_strdup_printf("<html><head><meta name=\"viewport\" content=\"width=device-width\">"
	       "<title>Browser</title><style>"
	       "body { margin: 0; padding: 16px; background: #f2f2f7; color: #1c1c1e; font-family: sans-serif; }"
	       "h1 { font-size: 32px; margin: 16px 8px 24px; }"
	       "a { display: block; background: #fff; color: #0a84ff; text-decoration: none;"
	       " padding: 14px 16px; margin-bottom: 8px; font-size: 20px; border-radius: 12px; }"
	       "a span { display: block; color: #8e8e93; font-size: 14px; margin-top: 4px; }"
	       "p { color: #8e8e93; font-size: 14px; margin: 24px 8px; }"
	       "</style></head><body><h1>%s</h1>"
	       "<a href=\"https://en.m.wikipedia.org/\">Wikipedia<span>en.m.wikipedia.org</span></a>"
	       "<a href=\"https://news.ycombinator.com/\">Hacker News<span>news.ycombinator.com</span></a>"
	       "<a href=\"https://www.openstreetmap.org/\">OpenStreetMap<span>%s</span></a>"
	       "<a href=\"https://github.com/litehtml/litehtml\">GitHub<span>github.com</span></a>"
	       "<a href=\"https://duckduckgo.com/\">DuckDuckGo<span>duckduckgo.com</span></a>"
	       "<a href=\"https://browserbench.org/Speedometer3.1/\">Speedometer 3.1<span>%s</span></a>"
	       "<p id=\"js\" data-works=\"%s\">%s</p>"
	       "<script>var p = document.getElementById('js'); p.textContent = p.dataset.works + navigator.userAgent;</script>"
	       "</body></html>",
	       /* NON-NLS-END */
	       C_("start page", "Start"), _("a JavaScript map"), _("how fast is it"), _("JavaScript works: "),
	       _("JavaScript is off."));
}

static struct {
	lv_obj_t *box, *view, *title, *ta, *kb, *reload; /* view: the frame, in box: the page's area */
	lv_timer_t *timer;
	WebKitWebView *web;
	struct wpe_view_backend_exportable_fdo *exportable;
	struct wpe_view_backend *backend;
	struct wpe_fdo_egl_exported_image *image; /* shown */
	bool frame_shown; /* to report on the next tick, which lets WebKit draw the next */
	GLuint tex;
	int32_t w, h;
	uint32_t frames, frames_at;
	/* Load timeline in the log: when the load started, resources asked for */
	uint32_t load_at;
	unsigned resources;
	bool committed, painted;
} web;

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

/* Frames */

static void export_cb(void *data, struct wpe_fdo_egl_exported_image *image)
{
	LV_UNUSED(data);
	glBindTexture(GL_TEXTURE_2D, web.tex);
	image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)wpe_fdo_egl_exported_image_get_egl_image(image));
	glBindTexture(GL_TEXTURE_2D, 0);
	if (web.image)
		wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(web.exportable, web.image);
	/* At its own size from the area's top left, never scaled: until WebKit has a frame of a
	 * new size (the keyboard came or went), the last one is cut off or leaves a strip */
	lv_obj_set_size(web.view, (int32_t)wpe_fdo_egl_exported_image_get_width(image),
	                (int32_t)wpe_fdo_egl_exported_image_get_height(image));
	if (!web.frames++)
		printf("web: first frame %ux%u\n", wpe_fdo_egl_exported_image_get_width(image),
		       wpe_fdo_egl_exported_image_get_height(image));
	web.image = image;
	web.frame_shown = true;
	if (web.committed && !web.painted) {
		web.painted = true;
		printf("web: +%u ms first paint\n", lv_tick_elaps(web.load_at));
	}
	lv_obj_invalidate(web.view);
}

/* Development: the result of a `js:` line of /tmp/web-load (a function body, awaited) */
static void js_done(GObject *obj, GAsyncResult *res, gpointer data)
{
	LV_UNUSED(data);
	GError *error = NULL;
	JSCValue *value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(obj), res, &error);
	if (!value) {
		printf("web: js error: %s\n", error->message);
		g_error_free(error);
		return;
	}
	char *json = jsc_value_to_json(value, 0);
	printf("web: js = %s\n", json ? json : "undefined");
	g_free(json);
	g_object_unref(value);
}

static void tick_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (web.frame_shown) {
		web.frame_shown = false;
		wpe_view_backend_exportable_fdo_dispatch_frame_complete(web.exportable);
	}
	for (int i = 0; i < ITERATIONS && g_main_context_iteration(NULL, FALSE); i++)
		;
	/* Development: `echo URL > /tmp/web-load` over SSH loads it (profiling without the
	 * screen); `echo "js:return await ..." > /tmp/web-load` runs a function body in the page */
	static uint32_t polled;
	if (web.web && lv_tick_elaps(polled) >= 500) {
		polled = lv_tick_get();
		char uri[2048];
		FILE *f = fopen("/tmp/web-load", "r");
		if (f) {
			if (fgets(uri, sizeof(uri), f)) {
				uri[strcspn(uri, "\n")] = '\0';
				if (!strncmp(uri, "js:", 3))
					webkit_web_view_call_async_javascript_function(web.web, uri + 3, -1, NULL, NULL,
					                                               NULL, NULL, js_done, NULL);
				else
					webkit_web_view_load_uri(web.web, uri);
			}
			fclose(f);
			remove("/tmp/web-load");
		}
	}
	if (lv_tick_elaps(web.frames_at) >= 5000) {
		if (web.frames > 1)
			printf("web: %.1f frames/s\n", web.frames * 1000.0f / lv_tick_elaps(web.frames_at));
		web.frames = 1; /* not the first frame again */
		web.frames_at = lv_tick_get();
	}
}

/* Touches: WebKit's own gestures (scroll, tap, long press) */

static void touch(enum wpe_input_touch_event_type type)
{
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(web.box, &a);
	struct wpe_input_touch_event_raw raw = {
		.type = type,
		.time = lv_tick_get(),
		.id = 0,
		/* Device pixels: WebKit divides them by the scale factor itself */
		.x = p.x - a.x1,
		.y = p.y - a.y1,
	};
	struct wpe_input_touch_event ev = {
		.touchpoints = &raw,
		.touchpoints_length = 1,
		.type = type,
		.id = 0,
		.time = raw.time,
	};
	wpe_view_backend_dispatch_touch_event(web.backend, &ev);
}

static void view_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		touch(wpe_input_touch_event_type_down);
		break;
	case LV_EVENT_PRESSING:
		touch(wpe_input_touch_event_type_motion);
		break;
	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST:
		touch(wpe_input_touch_event_type_up);
		break;
	case LV_EVENT_SIZE_CHANGED:
		/* The keyboard takes the bottom: WebKit lays the page out in what is left (as
		 * Android does), rather than its frame stretched into it */
		web.w = lv_obj_get_width(web.box);
		web.h = lv_obj_get_height(web.box);
		if (web.backend)
			wpe_view_backend_dispatch_set_size(web.backend, (uint32_t)(web.w / SCALE),
			                                   (uint32_t)(web.h / SCALE));
		break;
	default:
		break;
	}
}

/* The web view's state */

static void title_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	LV_UNUSED(obj);
	LV_UNUSED(pspec);
	LV_UNUSED(data);
	const char *title = webkit_web_view_get_title(web.web);
	if (title && *title)
		lv_label_set_text(web.title, title);
	else
		lv_label_set_text_static(web.title, "Browser"); /* NON-NLS: app.json's name */
}

static void address_show(void)
{
	const char *uri = webkit_web_view_get_uri(web.web);
	if (!lv_obj_has_state(web.ta, LV_STATE_FOCUSED))
		lv_textarea_set_text(web.ta, uri && strncmp(uri, "about:", 6) ? uri : "");
}

static void uri_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	LV_UNUSED(obj);
	LV_UNUSED(pspec);
	LV_UNUSED(data);
	address_show();
}

static void loading_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	LV_UNUSED(obj);
	LV_UNUSED(pspec);
	LV_UNUSED(data);
	bool loading = webkit_web_view_is_loading(web.web);
	lv_label_set_text_static(lv_obj_get_child(web.reload, 0), loading ? LV_SYMBOL_CLOSE : LV_SYMBOL_REFRESH);
	if (loading)
		lv_label_set_text_static(web.title, _("Loading..."));
	else
		title_changed(NULL, NULL, NULL);
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event, char *uri, GError *error, gpointer data)
{
	LV_UNUSED(view);
	LV_UNUSED(event);
	LV_UNUSED(data);
	printf("web: %s: %s\n", uri, error->message);
	return FALSE; /* WebKit's error page */
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	LV_UNUSED(data);
	switch (event) {
	case WEBKIT_LOAD_STARTED:
		web.load_at = lv_tick_get();
		web.resources = 0;
		web.committed = web.painted = false;
		printf("web: load %s\n", webkit_web_view_get_uri(view));
		break;
	case WEBKIT_LOAD_REDIRECTED:
		printf("web: +%u ms redirected to %s\n", lv_tick_elaps(web.load_at), webkit_web_view_get_uri(view));
		break;
	case WEBKIT_LOAD_COMMITTED:
		web.committed = true;
		printf("web: +%u ms committed\n", lv_tick_elaps(web.load_at));
		break;
	case WEBKIT_LOAD_FINISHED:
		printf("web: +%u ms finished, %u resources\n", lv_tick_elaps(web.load_at), web.resources);
		break;
	}
}

/* Each resource with when it started and how long it took */
static void resource_finished(WebKitWebResource *resource, gpointer data)
{
	uint32_t start = GPOINTER_TO_UINT(data);
	WebKitURIResponse *response = webkit_web_resource_get_response(resource);
	printf("web: +%u ms %5u ms %7llu B %s\n", start - web.load_at, lv_tick_elaps(start),
	       response ? (unsigned long long)webkit_uri_response_get_content_length(response) : 0ULL,
	       webkit_web_resource_get_uri(resource));
}

static void resource_failed(WebKitWebResource *resource, GError *error, gpointer data)
{
	uint32_t start = GPOINTER_TO_UINT(data);
	printf("web: +%u ms %5u ms failed %s: %s\n", start - web.load_at, lv_tick_elaps(start),
	       webkit_web_resource_get_uri(resource), error->message);
}

static void resource_started(WebKitWebView *view, WebKitWebResource *resource, WebKitURIRequest *request,
                             gpointer data)
{
	LV_UNUSED(view);
	LV_UNUSED(request);
	LV_UNUSED(data);
	web.resources++;
	gpointer start = GUINT_TO_POINTER(lv_tick_get());
	g_signal_connect(resource, "finished", G_CALLBACK(resource_finished), start);
	g_signal_connect(resource, "failed", G_CALLBACK(resource_failed), start);
}

static void process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data)
{
	LV_UNUSED(data);
	printf("web: web process %s, reloading\n",
	       reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "out of memory" : "crashed");
	webkit_web_view_reload(view);
}

/* Links that open a new window (target=_blank, window.open) load here instead */
static WebKitWebView *create_cb(WebKitWebView *view, WebKitNavigationAction *action, gpointer data)
{
	LV_UNUSED(data);
	webkit_web_view_load_request(view, webkit_navigation_action_get_request(action));
	return NULL;
}

/* Bar */

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (web.web && webkit_web_view_can_go_back(web.web))
		webkit_web_view_go_back(web.web);
	else
		ui_pop();
}

static void reload_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (!web.web)
		return;
	if (webkit_web_view_is_loading(web.web))
		webkit_web_view_stop_loading(web.web);
	else
		webkit_web_view_reload(web.web);
}

static void load_typed(const char *text)
{
	while (*text == ' ')
		text++;
	if (!*text || !web.web)
		return;
	char *uri;
	if (strstr(text, "://") || !strncmp(text, "about:", 6))
		uri = g_strdup(text);
	else if (!strchr(text, ' ') && strchr(text, '.'))
		uri = g_strdup_printf("https://%s", text);
	else {
		char *q = g_uri_escape_string(text, NULL, FALSE);
		uri = g_strconcat(SEARCH, q, NULL);
		g_free(q);
	}
	webkit_web_view_load_uri(web.web, uri);
	g_free(uri);
}

static void ta_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_CLICKED:
		ui_keyboard_show(web.kb, true);
		break;
	case LV_EVENT_READY:
		ui_keyboard_show(web.kb, false);
		lv_obj_remove_state(web.ta, LV_STATE_FOCUSED);
		load_typed(lv_textarea_get_text(web.ta));
		break;
	case LV_EVENT_CANCEL:
		ui_keyboard_show(web.kb, false);
		lv_obj_remove_state(web.ta, LV_STATE_FOCUSED);
		address_show();
		break;
	default:
		break;
	}
}

/* Setup */

/* Features off by default outside Apple's ports, by identifier (the preference
 * without "Enabled"). navigator.storage: StorageAPI, its estimate(); its getDirectory()
 * (origin private file system): FileSystem, with sync access handles and writables. */
static const char *const features[] = { /* NON-NLS-BEGIN: identifiers */
	"StorageAPI", "StorageAPIEstimate", "FileSystem", "AccessHandle", "FileSystemWritableStream",
}; /* NON-NLS-END */

static void features_enable(WebKitSettings *settings)
{
	WebKitFeatureList *list = webkit_settings_get_all_features();
	for (gsize i = 0; i < webkit_feature_list_get_length(list); i++) {
		WebKitFeature *feature = webkit_feature_list_get(list, i);
		for (size_t k = 0; k < G_N_ELEMENTS(features); k++)
			if (!strcmp(webkit_feature_get_identifier(feature), features[k]))
				webkit_settings_set_feature_enabled(settings, feature, TRUE);
	}
	webkit_feature_list_unref(list);
}

static bool wpe_ready(void)
{
	static int ready = -1;
	if (ready >= 0)
		return ready;
	ready = 0;
	/* Before anything asks GLib for them: WebKit's data on the data partition */
	mkdir(DIR, 0755);
	setenv("XDG_DATA_HOME", DIR "/data", 1);
	setenv("XDG_CACHE_HOME", DIR "/cache", 1);

	EGLDisplay display = eglGetCurrentDisplay();
	image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (display == EGL_NO_DISPLAY || !image_target_texture) {
		printf("web: no current EGL display or EGLImage textures\n");
		return false;
	}
	if (!wpe_loader_init("libWPEBackend-fdo-1.0.so.1") || !wpe_fdo_initialize_for_egl_display(display)) {
		printf("web: WPEBackend-fdo did not initialize (EGL_WL_bind_wayland_display?)\n");
		return false;
	}
	ready = 1;
	return true;
}

static void backend_destroy(gpointer data)
{
	wpe_view_backend_exportable_fdo_destroy(data);
}

static void web_create(void)
{
	static const struct wpe_view_backend_exportable_fdo_egl_client client = {
		.export_fdo_egl_image = export_cb,
	};
	web.exportable = wpe_view_backend_exportable_fdo_egl_create(&client, NULL, (uint32_t)(web.w / SCALE),
	                                                            (uint32_t)(web.h / SCALE));
	web.backend = wpe_view_backend_exportable_fdo_get_view_backend(web.exportable);
	WebKitWebViewBackend *backend = webkit_web_view_backend_new(web.backend, backend_destroy, web.exportable);
	web.web = webkit_web_view_new(backend);
	/* Only once the view is the backend's client: before, nobody gets it */
	wpe_view_backend_dispatch_set_device_scale_factor(web.backend, SCALE);
	wpe_view_backend_add_activity_state(web.backend, wpe_view_activity_state_visible |
	                                    wpe_view_activity_state_focused | wpe_view_activity_state_in_window);

	WebKitSettings *settings = webkit_web_view_get_settings(web.web);
	webkit_settings_set_user_agent(settings, USER_AGENT);
	features_enable(settings);

	g_signal_connect(web.web, "notify::title", G_CALLBACK(title_changed), NULL);
	g_signal_connect(web.web, "notify::uri", G_CALLBACK(uri_changed), NULL);
	g_signal_connect(web.web, "notify::is-loading", G_CALLBACK(loading_changed), NULL);
	g_signal_connect(web.web, "load-failed", G_CALLBACK(load_failed), NULL);
	g_signal_connect(web.web, "load-changed", G_CALLBACK(load_changed), NULL);
	g_signal_connect(web.web, "resource-load-started", G_CALLBACK(resource_started), NULL);
	g_signal_connect(web.web, "web-process-terminated", G_CALLBACK(process_terminated), NULL);
	g_signal_connect(web.web, "create", G_CALLBACK(create_cb), NULL);
	char *home = home_html();
	webkit_web_view_load_html(web.web, home, NULL);
	g_free(home);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_timer_delete(web.timer);
	if (web.web) {
		if (web.image)
			wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(web.exportable, web.image);
		g_signal_handlers_disconnect_by_data(web.web, NULL);
		g_object_unref(web.web); /* and the backend, through backend_destroy() */
		for (int i = 0; i < ITERATIONS && g_main_context_iteration(NULL, FALSE); i++)
			;
	}
	ui_texture_delete(web.tex);
	memset(&web, 0, sizeof(web));
}

static void web_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Browser", &header, &body); /* NON-NLS: app.json's name */
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	web.title = lv_obj_get_child(header, 1);

	web.box = ui_box(body);
	lv_obj_set_width(web.box, LV_PCT(100));
	lv_obj_set_flex_grow(web.box, 1);
	lv_obj_set_clickable(web.box, true);
	lv_obj_add_event_cb(web.box, view_cb, LV_EVENT_ALL, NULL);
	web.view = lv_3dtexture_create(web.box);
	lv_obj_set_clickable(web.view, false); /* the box gets the touches */

	lv_obj_t *bar = ui_bar_create(body);
	lv_obj_t *back = ui_button(bar, 80, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_LEFT,
	                           back_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(back, 0), &ui_font_32, 0);
	web.ta = ui_text_field(bar, 80, 2048, ta_cb, NULL);
	lv_textarea_set_placeholder_text(web.ta, _("Address or search"));
	lv_obj_set_flex_grow(web.ta, 1);
	web.reload = ui_button(bar, 80, 80, UI_COLOR_BUTTON, UI_COLOR_BUTTON_PRESSED, LV_SYMBOL_REFRESH,
	                       reload_cb, NULL);
	lv_obj_set_style_text_font(lv_obj_get_child(web.reload, 0), &ui_font_32, 0);
	web.kb = ui_keyboard_create(body, web.ta);

	ui_push(scr);
	lv_obj_update_layout(scr);
	web.w = lv_obj_get_width(web.box);
	web.h = lv_obj_get_height(web.box);
	lv_obj_set_size(web.view, web.w, web.h);
	web.timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	web.frames_at = lv_tick_get();
	if (!wpe_ready())
		return;
	web.tex = ui_texture_create(NULL, web.w, web.h);
	lv_3dtexture_set_src(web.view, web.tex);
	lv_3dtexture_set_rb_swap(web.view, true); /* WebKit's frames are RGBA */
	web_create();
}

/* For phone-ui (external.c) */

const char app_abi[] = UI_ABI;

void app_open(void)
{
	web_open();
}
