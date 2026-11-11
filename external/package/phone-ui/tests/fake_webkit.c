/*
 * WPE WebKit and WPEBackend-fdo for test_web.c (ui_test.h): what apps/web/web.c calls,
 * linked into the test program (exported: web.so, loaded by external.c, finds it there).
 *
 * A web view is a GObject with WebKit's properties and signals, so web.c's handlers run
 * through GLib as with WebKit. Loads go on in idle sources of GLib's default context,
 * which web.c iterates on its timer, like WebKit's main-thread work: a page is asked of
 * fake_curl.c (the tests' pages, redirects followed), then committed (its <title>), a
 * frame exported, finished. A failed load emits its resource's failed and load-failed,
 * then shows an error page. The
 * history keeps each committed page; back shows one again without asking for it.
 *
 * Frames: an EGLImage of a texture as large as the frame, the page's color (its first
 * `background: #rrggbb`) with a marker's band across (UI_TEST_WEB_MARKER), made in the
 * current EGL context (llvmpipe's), exported once the previous frame was completed
 * (wpe_view_backend_exportable_fdo_dispatch_frame_complete()), as WPEBackend-fdo does;
 * an animated page (ui_test_web_animate()) wants the next one as soon as that is.
 * Released images are destroyed; ui_test_web_images() counts the rest.
 *
 * Text fields: the app's input method context (webkit_web_view_set_input_method_context())
 * is told as WebKit's InputMethodFilter tells it: the field's purpose and hints, then
 * focus in, its text around the cursor (notify_surrounding, UTF-8 bytes) from GLib's loop
 * after each change as WebKit's editor state, focus out. What it commits goes in at the
 * cursor, delete-surrounding deletes characters around it; Return keys are counted.
 * A script with "blur()" in it takes the focus away, as document.activeElement.blur().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <curl/curl.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/webkit.h>

#include "ui_test.h"

#define MAX_TOUCHES 4096

/* WPEBackend-fdo */

struct wpe_view_backend {
	struct wpe_view_backend_exportable_fdo *exportable;
};

struct wpe_view_backend_exportable_fdo {
	struct wpe_view_backend backend;
	const struct wpe_view_backend_exportable_fdo_egl_client *client;
	void *data;
	uint32_t w, h;
	uint32_t color; /* of the page to draw */
	bool wanted;    /* a frame to export */
	bool waiting;   /* for the last one's frame_complete */
	guint idle;
};

struct wpe_fdo_egl_exported_image {
	EGLImageKHR image;
	GLuint tex;
	uint32_t w, h;
};

static bool broken;
static float scale = 1;
static uint32_t activity;
static struct ui_test_touch touches[MAX_TOUCHES];
static unsigned touch_count;
static unsigned frames, images;
static bool frames_held, animating;
static unsigned frames_waited;
static uint32_t frame_w, frame_h; /* the last exported */
static struct wpe_view_backend_exportable_fdo *last_exportable;

bool wpe_loader_init(const char *impl_library_name)
{
	(void)impl_library_name;
	return true;
}

bool wpe_fdo_initialize_for_egl_display(EGLDisplay display)
{
	return !broken && display != EGL_NO_DISPLAY;
}

struct wpe_view_backend_exportable_fdo *
wpe_view_backend_exportable_fdo_egl_create(const struct wpe_view_backend_exportable_fdo_egl_client *client,
                                           void *data, uint32_t width, uint32_t height)
{
	struct wpe_view_backend_exportable_fdo *e = calloc(1, sizeof(*e));
	e->backend.exportable = e;
	e->client = client;
	e->data = data;
	e->w = width;
	e->h = height;
	e->color = 0xffffff;
	last_exportable = e;
	return e;
}

struct wpe_view_backend *wpe_view_backend_exportable_fdo_get_view_backend(struct wpe_view_backend_exportable_fdo *e)
{
	return &e->backend;
}

void wpe_view_backend_exportable_fdo_destroy(struct wpe_view_backend_exportable_fdo *e)
{
	if (last_exportable == e)
		last_exportable = NULL;
	if (e->idle)
		g_source_remove(e->idle);
	free(e);
}

static struct wpe_fdo_egl_exported_image *image_create(struct wpe_view_backend_exportable_fdo *e)
{
	struct wpe_fdo_egl_exported_image *img = calloc(1, sizeof(*img));
	img->w = (uint32_t)(e->w * scale);
	img->h = (uint32_t)(e->h * scale);
	/* The page's color, the marker's band across at its rows: where it shows tells a
	 * frame drawn 1:1 from one stretched */
	uint8_t *px = malloc((size_t)img->w * img->h * 4);
	for (uint32_t y = 0; y < img->h; y++) {
		uint32_t c = y >= UI_TEST_WEB_MARKER_Y && y < UI_TEST_WEB_MARKER_Y + UI_TEST_WEB_MARKER_H ?
		             UI_TEST_WEB_MARKER : e->color;
		for (uint32_t x = 0; x < img->w; x++) {
			uint8_t *p = px + ((size_t)y * img->w + x) * 4;
			p[0] = c >> 16 & 0xff;
			p[1] = c >> 8 & 0xff;
			p[2] = c & 0xff;
			p[3] = 0xff;
		}
	}
	GLint bound;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glGenTextures(1, &img->tex);
	glBindTexture(GL_TEXTURE_2D, img->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img->w, (GLsizei)img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	free(px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	img->image = eglCreateImage(eglGetCurrentDisplay(), eglGetCurrentContext(), EGL_GL_TEXTURE_2D,
	                            (EGLClientBuffer)(uintptr_t)img->tex, NULL);
	if (img->image == EGL_NO_IMAGE)
		fprintf(stderr, "  ui_test: fake WebKit: no EGLImage (EGL error %#x)\n", eglGetError());
	return img;
}

static gboolean export_cb(gpointer data)
{
	struct wpe_view_backend_exportable_fdo *e = data;
	e->idle = 0;
	if (!e->wanted || e->waiting || frames_held)
		return G_SOURCE_REMOVE;
	e->wanted = false;
	e->waiting = true;
	/* This fake draws at once, WebKit takes a while: a second frame at the same time of the
	 * tests is one the app may hold until its next pass */
	static uint32_t exported_at;
	bool first = !frames || lv_tick_get() != exported_at;
	exported_at = lv_tick_get();
	frames++;
	images++;
	struct wpe_fdo_egl_exported_image *img = image_create(e);
	frame_w = img->w;
	frame_h = img->h;
	e->client->export_fdo_egl_image(e->data, img);
	if (e->waiting && first)
		frames_waited++; /* not completed as it was taken: WebKit idles until it is */
	return G_SOURCE_REMOVE;
}

/* WebKit draws a frame: exported from GLib's loop, once the last one is complete */
static void frame_want(struct wpe_view_backend_exportable_fdo *e, uint32_t color)
{
	e->color = color;
	e->wanted = true;
	if (!e->idle && !e->waiting)
		e->idle = g_idle_add(export_cb, e);
}

void wpe_view_backend_exportable_fdo_dispatch_frame_complete(struct wpe_view_backend_exportable_fdo *e)
{
	e->waiting = false;
	if (animating)
		e->wanted = true; /* the page changed meanwhile: the next frame at once */
	if (e->wanted && !e->idle)
		e->idle = g_idle_add(export_cb, e);
}

void wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(struct wpe_view_backend_exportable_fdo *e,
                                                                         struct wpe_fdo_egl_exported_image *img)
{
	(void)e;
	if (img->image != EGL_NO_IMAGE)
		eglDestroyImage(eglGetCurrentDisplay(), img->image);
	glDeleteTextures(1, &img->tex);
	free(img);
	images--;
}

EGLImageKHR wpe_fdo_egl_exported_image_get_egl_image(struct wpe_fdo_egl_exported_image *img)
{
	return img->image;
}

uint32_t wpe_fdo_egl_exported_image_get_width(struct wpe_fdo_egl_exported_image *img)
{
	return img->w;
}

uint32_t wpe_fdo_egl_exported_image_get_height(struct wpe_fdo_egl_exported_image *img)
{
	return img->h;
}

/* libwpe */

void wpe_view_backend_dispatch_set_device_scale_factor(struct wpe_view_backend *backend, float factor)
{
	(void)backend;
	scale = factor;
}

/* The page laid out again at that size: a frame of it */
void wpe_view_backend_dispatch_set_size(struct wpe_view_backend *backend, uint32_t width, uint32_t height)
{
	struct wpe_view_backend_exportable_fdo *e = backend->exportable;
	if (e->w == width && e->h == height)
		return;
	e->w = width;
	e->h = height;
	frame_want(e, e->color);
}

void wpe_view_backend_add_activity_state(struct wpe_view_backend *backend, uint32_t state)
{
	(void)backend;
	activity |= state;
}

void wpe_view_backend_dispatch_touch_event(struct wpe_view_backend *backend, struct wpe_input_touch_event *ev)
{
	(void)backend;
	if (touch_count < MAX_TOUCHES && ev->touchpoints_length > 0)
		touches[touch_count++] = (struct ui_test_touch){
			.type = ev->type,
			.x = ev->touchpoints[0].x,
			.y = ev->touchpoints[0].y,
		};
}

static void field_key(uint32_t key, bool pressed); /* the page's field */

void wpe_view_backend_dispatch_keyboard_event(struct wpe_view_backend *backend, struct wpe_input_keyboard_event *ev)
{
	(void)backend;
	field_key(ev->key_code, ev->pressed);
}

/* Settings */

static const char *const feature_ids[] = {
	"StorageAPI", "StorageAPIEstimate", "FileSystem", "AccessHandle", "FileSystemWritableStream", "WebGPU",
};
#define FEATURES G_N_ELEMENTS(feature_ids)

struct _WebKitFeature {
	const char *identifier;
};
static WebKitFeature features[FEATURES];

struct _WebKitFeatureList {
	gsize length;
};

struct _WebKitSettings {
	char *user_agent;
	bool enabled[FEATURES];
};
static WebKitSettings *last_settings; /* the last view's, for the tests */

WebKitFeatureList *webkit_settings_get_all_features(void)
{
	for (gsize i = 0; i < FEATURES; i++)
		features[i].identifier = feature_ids[i];
	WebKitFeatureList *list = g_new0(WebKitFeatureList, 1);
	list->length = FEATURES;
	return list;
}

gsize webkit_feature_list_get_length(WebKitFeatureList *list)
{
	return list->length;
}

WebKitFeature *webkit_feature_list_get(WebKitFeatureList *list, gsize index)
{
	return index < list->length ? &features[index] : NULL;
}

void webkit_feature_list_unref(WebKitFeatureList *list)
{
	g_free(list);
}

const char *webkit_feature_get_identifier(WebKitFeature *feature)
{
	return feature->identifier;
}

void webkit_settings_set_feature_enabled(WebKitSettings *settings, WebKitFeature *feature, gboolean enabled)
{
	settings->enabled[feature - features] = enabled;
}

void webkit_settings_set_user_agent(WebKitSettings *settings, const gchar *user_agent)
{
	g_free(settings->user_agent);
	settings->user_agent = g_strdup(user_agent);
}

/* Requests, responses, resources */

struct _WebKitURIRequest {
	char *uri;
};

struct _WebKitNavigationAction {
	WebKitURIRequest *request;
};

struct _WebKitURIResponse {
	guint64 length;
};

WebKitURIRequest *webkit_navigation_action_get_request(WebKitNavigationAction *navigation)
{
	return navigation->request;
}

guint64 webkit_uri_response_get_content_length(WebKitURIResponse *response)
{
	return response->length;
}

struct _WebKitWebResource {
	GObject parent;
	char *uri;
	WebKitURIResponse response;
};
typedef struct {
	GObjectClass parent;
} WebKitWebResourceClass;
G_DEFINE_TYPE(WebKitWebResource, webkit_web_resource, G_TYPE_OBJECT)

static void webkit_web_resource_finalize(GObject *obj)
{
	g_free(((WebKitWebResource *)obj)->uri);
	G_OBJECT_CLASS(webkit_web_resource_parent_class)->finalize(obj);
}

static void webkit_web_resource_class_init(WebKitWebResourceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = webkit_web_resource_finalize;
	g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	g_signal_new("failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
	             G_TYPE_POINTER);
}

static void webkit_web_resource_init(WebKitWebResource *resource)
{
	(void)resource;
}

const gchar *webkit_web_resource_get_uri(WebKitWebResource *resource)
{
	return resource->uri;
}

WebKitURIResponse *webkit_web_resource_get_response(WebKitWebResource *resource)
{
	return &resource->response;
}

/* The web view */

struct _WebKitWebViewBackend {
	struct wpe_view_backend *backend;
	GDestroyNotify notify;
	gpointer data;
};

WebKitWebViewBackend *webkit_web_view_backend_new(struct wpe_view_backend *backend, GDestroyNotify notify,
                                                  gpointer user_data)
{
	WebKitWebViewBackend *b = g_new0(WebKitWebViewBackend, 1);
	b->backend = backend;
	b->notify = notify;
	b->data = user_data;
	return b;
}

struct entry {
	char *uri, *title, *html; /* html: a page given as text (load_html), loaded again from it */
	uint32_t color;
};

enum load_kind { LOAD_NONE, LOAD_NETWORK, LOAD_HTML, LOAD_HISTORY };

struct _WebKitWebView {
	GObject parent;
	WebKitWebViewBackend *backend;
	WebKitSettings settings;
	char *uri, *title;
	gboolean loading;
	GPtrArray *history; /* struct entry */
	int current;        /* -1: none */
	/* The load going on */
	enum load_kind kind;
	bool replace; /* the current entry (a reload) rather than a new one */
	char *load_uri, *load_html;
	int load_index;
	CURLM *multi;
	CURL *curl;
	GString *body;
	guint idle;
	WebKitInputMethodContext *im;
};
typedef struct {
	GObjectClass parent;
} WebKitWebViewClass;
G_DEFINE_TYPE(WebKitWebView, webkit_web_view, G_TYPE_OBJECT)

enum { PROP_0, PROP_URI, PROP_TITLE, PROP_IS_LOADING };
enum { LOAD_CHANGED, LOAD_FAILED, RESOURCE_LOAD_STARTED, WEB_PROCESS_TERMINATED, CREATE, SIGNALS };
static guint signals[SIGNALS];
static unsigned views;
static WebKitWebView *last_view; /* alive, for the tests */

static void entry_free(gpointer p)
{
	struct entry *e = p;
	g_free(e->uri);
	g_free(e->title);
	g_free(e->html);
	g_free(e);
}

static void load_cancel(WebKitWebView *v)
{
	if (v->idle) {
		g_source_remove(v->idle);
		v->idle = 0;
	}
	if (v->curl) {
		curl_multi_remove_handle(v->multi, v->curl);
		curl_easy_cleanup(v->curl);
		v->curl = NULL;
	}
	if (v->body) {
		g_string_free(v->body, TRUE);
		v->body = NULL;
	}
	g_clear_pointer(&v->load_uri, g_free);
	g_clear_pointer(&v->load_html, g_free);
	v->kind = LOAD_NONE;
}

static void webkit_web_view_finalize(GObject *obj)
{
	WebKitWebView *v = (WebKitWebView *)obj;
	load_cancel(v);
	curl_multi_cleanup(v->multi);
	g_ptr_array_free(v->history, TRUE);
	g_free(v->uri);
	g_free(v->title);
	g_free(v->settings.user_agent);
	if (v->im)
		g_object_unref(v->im);
	if (v->backend->notify)
		v->backend->notify(v->backend->data);
	g_free(v->backend);
	if (last_view == v)
		last_view = NULL;
	if (last_settings == &v->settings)
		last_settings = NULL;
	views--;
	G_OBJECT_CLASS(webkit_web_view_parent_class)->finalize(obj);
}

static void get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
	WebKitWebView *v = (WebKitWebView *)obj;
	switch (id) {
	case PROP_URI:
		g_value_set_string(value, v->uri);
		break;
	case PROP_TITLE:
		g_value_set_string(value, v->title);
		break;
	case PROP_IS_LOADING:
		g_value_set_boolean(value, v->loading);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void webkit_web_view_class_init(WebKitWebViewClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	GType type = G_TYPE_FROM_CLASS(klass);
	oc->finalize = webkit_web_view_finalize;
	oc->get_property = get_property;
	g_object_class_install_property(oc, PROP_URI, g_param_spec_string("uri", NULL, NULL, NULL, G_PARAM_READABLE));
	g_object_class_install_property(oc, PROP_TITLE, g_param_spec_string("title", NULL, NULL, NULL, G_PARAM_READABLE));
	g_object_class_install_property(oc, PROP_IS_LOADING,
	                                g_param_spec_boolean("is-loading", NULL, NULL, FALSE, G_PARAM_READABLE));
	signals[LOAD_CHANGED] = g_signal_new("load-changed", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
	                                     G_TYPE_NONE, 1, G_TYPE_INT);
	signals[LOAD_FAILED] = g_signal_new("load-failed", type, G_SIGNAL_RUN_LAST, 0,
	                                    g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 3,
	                                    G_TYPE_INT, G_TYPE_STRING, G_TYPE_POINTER);
	signals[RESOURCE_LOAD_STARTED] = g_signal_new("resource-load-started", type, G_SIGNAL_RUN_LAST, 0, NULL,
	                                              NULL, NULL, G_TYPE_NONE, 2, G_TYPE_OBJECT, G_TYPE_POINTER);
	signals[WEB_PROCESS_TERMINATED] = g_signal_new("web-process-terminated", type, G_SIGNAL_RUN_LAST, 0, NULL,
	                                               NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
	signals[CREATE] = g_signal_new("create", type, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_POINTER, 1,
	                               G_TYPE_POINTER);
}

static void webkit_web_view_init(WebKitWebView *v)
{
	v->history = g_ptr_array_new_with_free_func(entry_free);
	v->current = -1;
	v->multi = curl_multi_init();
}

WebKitWebView *webkit_web_view_new(WebKitWebViewBackend *backend)
{
	WebKitWebView *v = g_object_new(WEBKIT_TYPE_WEB_VIEW, NULL);
	v->backend = backend;
	views++;
	last_view = v;
	last_settings = &v->settings;
	return v;
}

WebKitSettings *webkit_web_view_get_settings(WebKitWebView *v)
{
	return &v->settings;
}

const gchar *webkit_web_view_get_uri(WebKitWebView *v)
{
	return v->uri;
}

const gchar *webkit_web_view_get_title(WebKitWebView *v)
{
	return v->title;
}

gboolean webkit_web_view_is_loading(WebKitWebView *v)
{
	return v->loading;
}

gboolean webkit_web_view_can_go_back(WebKitWebView *v)
{
	return v->current > 0;
}

static void uri_set(WebKitWebView *v, const char *uri)
{
	if (g_strcmp0(v->uri, uri) == 0)
		return;
	g_free(v->uri);
	v->uri = g_strdup(uri);
	g_object_notify(G_OBJECT(v), "uri");
}

static void title_set(WebKitWebView *v, const char *title)
{
	if (g_strcmp0(v->title, title) == 0)
		return;
	g_free(v->title);
	v->title = g_strdup(title);
	g_object_notify(G_OBJECT(v), "title");
}

static void loading_set(WebKitWebView *v, gboolean loading)
{
	if (v->loading == loading)
		return;
	v->loading = loading;
	g_object_notify(G_OBJECT(v), "is-loading");
}

/* What a page shows: its <title>, its first `background: #rrggbb` */
static char *html_title(const char *html)
{
	const char *start = html ? strstr(html, "<title>") : NULL;
	const char *end = start ? strstr(start, "</title>") : NULL;
	return end ? g_strndup(start + 7, end - start - 7) : g_strdup("");
}

static uint32_t html_color(const char *html)
{
	const char *bg = html ? strstr(html, "background: #") : NULL;
	return bg ? (uint32_t)strtoul(bg + 13, NULL, 16) & 0xffffff : 0xffffff;
}

static struct entry *entry_current(WebKitWebView *v)
{
	return v->current >= 0 ? g_ptr_array_index(v->history, v->current) : NULL;
}

/* The page is there (e, NULL going back): in the history, its title, a frame of it,
 * the load finished */
static void commit(WebKitWebView *v, struct entry *e, size_t size)
{
	if (v->kind == LOAD_HISTORY)
		v->current = v->load_index;
	else if (v->replace && v->current >= 0) {
		entry_free(g_ptr_array_index(v->history, v->current));
		g_ptr_array_index(v->history, v->current) = e;
	} else {
		g_ptr_array_remove_range(v->history, v->current + 1, v->history->len - v->current - 1);
		g_ptr_array_add(v->history, e);
		v->current = v->history->len - 1;
	}
	load_cancel(v);
	struct entry *now = entry_current(v);
	uri_set(v, now->uri);
	g_signal_emit(v, signals[LOAD_CHANGED], 0, WEBKIT_LOAD_COMMITTED);
	title_set(v, now->title);

	WebKitWebResource *resource = g_object_new(webkit_web_resource_get_type(), NULL);
	resource->uri = g_strdup(now->uri);
	resource->response.length = size;
	g_signal_emit(v, signals[RESOURCE_LOAD_STARTED], 0, resource, NULL);
	g_signal_emit_by_name(resource, "finished");
	g_object_unref(resource);

	frame_want(v->backend->backend->exportable, now->color);
	loading_set(v, FALSE);
	g_signal_emit(v, signals[LOAD_CHANGED], 0, WEBKIT_LOAD_FINISHED);
}

static struct entry *entry_new(const char *uri, const char *html)
{
	struct entry *e = g_new0(struct entry, 1);
	e->uri = g_strdup(uri);
	e->title = html_title(html);
	e->color = html_color(html);
	return e;
}

static size_t body_write(char *data, size_t size, size_t n, void *p)
{
	g_string_append_len(p, data, size * n);
	return size * n;
}

static gboolean load_step(gpointer data)
{
	WebKitWebView *v = data;
	switch (v->kind) {
	case LOAD_HTML: {
		v->idle = 0;
		struct entry *e = entry_new(v->load_uri, v->load_html);
		e->html = g_strdup(v->load_html);
		size_t size = strlen(v->load_html);
		commit(v, e, size);
		return G_SOURCE_REMOVE;
	}
	case LOAD_HISTORY: {
		v->idle = 0;
		commit(v, NULL, 0);
		return G_SOURCE_REMOVE;
	}
	case LOAD_NETWORK:
		break;
	default:
		v->idle = 0;
		return G_SOURCE_REMOVE;
	}
	int running;
	curl_multi_perform(v->multi, &running);
	int left;
	CURLMsg *msg = curl_multi_info_read(v->multi, &left);
	if (!msg || msg->msg != CURLMSG_DONE)
		return G_SOURCE_CONTINUE;
	v->idle = 0;
	CURLcode result = msg->data.result;
	if (result != CURLE_OK) {
		char *uri = g_strdup(v->load_uri);
		GError *error = g_error_new_literal(g_quark_from_static_string("WebKitNetworkError"),
		                                    WEBKIT_NETWORK_ERROR_FAILED, curl_easy_strerror(result));
		/* The page's resource failed first, as in WebKit */
		WebKitWebResource *resource = g_object_new(webkit_web_resource_get_type(), NULL);
		resource->uri = g_strdup(uri);
		g_signal_emit(v, signals[RESOURCE_LOAD_STARTED], 0, resource, NULL);
		g_signal_emit_by_name(resource, "failed", error);
		g_object_unref(resource);
		gboolean handled = FALSE;
		g_signal_emit(v, signals[LOAD_FAILED], 0, WEBKIT_LOAD_STARTED, uri, error, &handled);
		g_error_free(error);
		if (v->kind != LOAD_NETWORK) { /* a handler loaded something else */
			g_free(uri);
			return G_SOURCE_REMOVE;
		}
		/* WebKit's error page, at the address that failed */
		struct entry *e = entry_new(uri, NULL);
		e->color = 0xcccccc;
		g_free(uri);
		commit(v, e, 0);
		return G_SOURCE_REMOVE;
	}
	char *effective = NULL;
	curl_easy_getinfo(v->curl, CURLINFO_EFFECTIVE_URL, &effective);
	char *uri = g_strdup(effective ? effective : v->load_uri);
	if (strcmp(uri, v->load_uri) != 0) {
		uri_set(v, uri);
		g_signal_emit(v, signals[LOAD_CHANGED], 0, WEBKIT_LOAD_REDIRECTED);
	}
	size_t size = v->body->len;
	struct entry *e = entry_new(uri, v->body->str);
	g_free(uri);
	commit(v, e, size);
	return G_SOURCE_REMOVE;
}

/* A new load, the one going on given up (as WebKit does, quietly here) */
static void load_start(WebKitWebView *v, enum load_kind kind, const char *uri, bool replace)
{
	load_cancel(v);
	v->kind = kind;
	v->replace = replace;
	v->load_uri = g_strdup(uri);
	uri_set(v, uri);
	loading_set(v, TRUE);
	g_signal_emit(v, signals[LOAD_CHANGED], 0, WEBKIT_LOAD_STARTED);
	if (v->kind != kind) /* a handler started another */
		return;
	if (kind == LOAD_NETWORK) {
		v->body = g_string_new(NULL);
		v->curl = curl_easy_init();
		curl_easy_setopt(v->curl, CURLOPT_URL, uri);
		curl_easy_setopt(v->curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(v->curl, CURLOPT_WRITEFUNCTION, body_write);
		curl_easy_setopt(v->curl, CURLOPT_WRITEDATA, v->body);
		curl_multi_add_handle(v->multi, v->curl);
	}
	v->idle = g_idle_add(load_step, v);
}

void webkit_web_view_load_uri(WebKitWebView *v, const gchar *uri)
{
	load_start(v, LOAD_NETWORK, uri, false);
}

static char *last_html; /* ui_test_web_html() */

void webkit_web_view_load_html(WebKitWebView *v, const gchar *content, const gchar *base_uri)
{
	g_free(last_html);
	last_html = g_strdup(content);
	load_start(v, LOAD_HTML, base_uri ? base_uri : "about:blank", false);
	if (v->kind == LOAD_HTML)
		v->load_html = g_strdup(content);
}

void webkit_web_view_load_request(WebKitWebView *v, WebKitURIRequest *request)
{
	webkit_web_view_load_uri(v, request->uri);
}

void webkit_web_view_reload(WebKitWebView *v)
{
	struct entry *e = entry_current(v);
	if (!e)
		return;
	if (e->html) {
		char *html = g_strdup(e->html);
		load_start(v, LOAD_HTML, e->uri, true);
		if (v->kind == LOAD_HTML)
			v->load_html = html;
		else
			g_free(html);
	} else
		load_start(v, LOAD_NETWORK, e->uri, true);
}

void webkit_web_view_go_back(WebKitWebView *v)
{
	if (v->current <= 0)
		return;
	struct entry *e = g_ptr_array_index(v->history, v->current - 1);
	char *uri = g_strdup(e->uri);
	load_start(v, LOAD_HISTORY, uri, false);
	v->load_index = v->current - 1;
	g_free(uri);
}

/* The load given up: load-failed (cancelled), the page shown stays */
void webkit_web_view_stop_loading(WebKitWebView *v)
{
	if (v->kind == LOAD_NONE)
		return;
	char *uri = g_strdup(v->load_uri);
	load_cancel(v);
	GError *error = g_error_new_literal(g_quark_from_static_string("WebKitNetworkError"),
	                                    WEBKIT_NETWORK_ERROR_CANCELLED, "Load request cancelled");
	gboolean handled = FALSE;
	g_signal_emit(v, signals[LOAD_FAILED], 0, WEBKIT_LOAD_STARTED, uri, error, &handled);
	g_error_free(error);
	g_free(uri);
	struct entry *e = entry_current(v);
	uri_set(v, e ? e->uri : NULL);
	loading_set(v, FALSE);
	g_signal_emit(v, signals[LOAD_CHANGED], 0, WEBKIT_LOAD_FINISHED);
}

static void field_blur(void);

/* JavaScript: not in the fake, but blur() (the focused field loses focus) */
void webkit_web_view_call_async_javascript_function(WebKitWebView *v, const char *body, gssize length,
                                                    GVariant *arguments, const char *world_name,
                                                    const char *source_uri, GCancellable *cancellable,
                                                    GAsyncReadyCallback callback, gpointer user_data)
{
	if (strstr(body, "blur()"))
		field_blur();
	(void)length;
	(void)arguments;
	(void)world_name;
	(void)source_uri;
	g_task_report_new_error(v, callback, user_data, NULL, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	                        "no JavaScript in the fake WebKit");
	(void)cancellable;
}

JSCValue *webkit_web_view_call_async_javascript_function_finish(WebKitWebView *v, GAsyncResult *result,
                                                                GError **error)
{
	(void)v;
	return g_task_propagate_pointer(G_TASK(result), error);
}

char *jsc_value_to_json(JSCValue *value, guint indent)
{
	(void)value;
	(void)indent;
	return NULL;
}

/* Input methods */

struct _WebKitInputMethodContextPrivate {
	WebKitInputPurpose purpose;
	WebKitInputHints hints;
};
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(WebKitInputMethodContext, webkit_input_method_context, G_TYPE_OBJECT)

enum { IM_PROP_0, IM_PROP_PURPOSE, IM_PROP_HINTS };

static void im_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
	WebKitInputMethodContextPrivate *p = ((WebKitInputMethodContext *)obj)->priv;
	switch (id) {
	case IM_PROP_PURPOSE:
		g_value_set_int(value, (int)p->purpose);
		break;
	case IM_PROP_HINTS:
		g_value_set_int(value, (int)p->hints);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
	}
}

static void webkit_input_method_context_class_init(WebKitInputMethodContextClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	GType type = G_TYPE_FROM_CLASS(klass);
	oc->get_property = im_get_property;
	/* Enums in WebKit, ints here: only their notify:: is used */
	g_object_class_install_property(oc, IM_PROP_PURPOSE,
	                                g_param_spec_int("input-purpose", NULL, NULL, 0, 6, 0, G_PARAM_READABLE));
	g_object_class_install_property(oc, IM_PROP_HINTS,
	                                g_param_spec_int("input-hints", NULL, NULL, 0, 63, 0, G_PARAM_READABLE));
	g_signal_new("committed", type, G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(WebKitInputMethodContextClass, committed),
	             NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
	g_signal_new("delete-surrounding", type, G_SIGNAL_RUN_LAST,
	             G_STRUCT_OFFSET(WebKitInputMethodContextClass, delete_surrounding), NULL, NULL, NULL, G_TYPE_NONE,
	             2, G_TYPE_INT, G_TYPE_UINT);
}

static void webkit_input_method_context_init(WebKitInputMethodContext *im)
{
	im->priv = webkit_input_method_context_get_instance_private(im);
}

WebKitInputPurpose webkit_input_method_context_get_input_purpose(WebKitInputMethodContext *im)
{
	return im->priv->purpose;
}

WebKitInputHints webkit_input_method_context_get_input_hints(WebKitInputMethodContext *im)
{
	return im->priv->hints;
}

static WebKitInputMethodContextClass *im_class(WebKitInputMethodContext *im)
{
	return (WebKitInputMethodContextClass *)G_OBJECT_GET_CLASS(im);
}

/* The page's focused field */
static struct {
	bool focused;
	GString *text;
	size_t cursor; /* bytes */
	unsigned enters;
	guint idle; /* to tell the text around the cursor */
} field;

static WebKitInputMethodContext *im_now(void)
{
	return last_view ? last_view->im : NULL;
}

static gboolean surrounding_cb(gpointer data)
{
	(void)data;
	field.idle = 0;
	WebKitInputMethodContext *im = im_now();
	if (im && field.focused && im_class(im)->notify_surrounding)
		im_class(im)->notify_surrounding(im, field.text->str, (guint)field.text->len, (guint)field.cursor,
		                                 (guint)field.cursor);
	return G_SOURCE_REMOVE;
}

static void field_changed(void)
{
	if (!field.idle)
		field.idle = g_idle_add(surrounding_cb, NULL);
}

static void committed_cb(WebKitInputMethodContext *im, const char *text, gpointer data)
{
	(void)im;
	(void)data;
	if (!field.focused)
		return;
	g_string_insert(field.text, (gssize)field.cursor, text);
	field.cursor += strlen(text);
	field_changed();
}

static void delete_surrounding_cb(WebKitInputMethodContext *im, int offset, guint n, gpointer data)
{
	(void)im;
	(void)data;
	if (!field.focused)
		return;
	const char *s = field.text->str;
	glong cursor = g_utf8_pointer_to_offset(s, s + field.cursor);
	glong chars = g_utf8_strlen(s, -1);
	glong from = CLAMP(cursor + offset, 0, chars), to = CLAMP(from + (glong)n, 0, chars);
	size_t a = (size_t)(g_utf8_offset_to_pointer(s, from) - s), b = (size_t)(g_utf8_offset_to_pointer(s, to) - s);
	g_string_erase(field.text, (gssize)a, (gssize)(b - a));
	if (field.cursor > b)
		field.cursor -= b - a;
	else if (field.cursor > a)
		field.cursor = a;
	field_changed();
}

void webkit_web_view_set_input_method_context(WebKitWebView *v, WebKitInputMethodContext *im)
{
	if (v->im) {
		g_signal_handlers_disconnect_by_data(v->im, v);
		g_object_unref(v->im);
	}
	v->im = im ? g_object_ref(im) : NULL;
	if (im) {
		g_signal_connect(im, "committed", G_CALLBACK(committed_cb), v);
		g_signal_connect(im, "delete-surrounding", G_CALLBACK(delete_surrounding_cb), v);
	}
}

static void field_key(uint32_t key, bool pressed)
{
	if (field.focused && pressed && key == WPE_KEY_Return)
		field.enters++;
}

static void field_blur(void)
{
	if (!field.focused)
		return;
	field.focused = false;
	WebKitInputMethodContext *im = im_now();
	if (im && im_class(im)->notify_focus_out)
		im_class(im)->notify_focus_out(im);
}

/* For the tests */

void ui_test_web_field(int purpose, int hints, const char *text)
{
	WebKitInputMethodContext *im = im_now();
	if (!field.text)
		field.text = g_string_new(NULL);
	g_string_assign(field.text, text);
	field.cursor = field.text->len;
	if (im) {
		/* As InputMethodFilter::setState(): the content type, then focus in if it was not */
		im->priv->purpose = (WebKitInputPurpose)purpose;
		im->priv->hints = (WebKitInputHints)hints;
		g_object_notify(G_OBJECT(im), "input-purpose");
		g_object_notify(G_OBJECT(im), "input-hints");
		if (!field.focused && im_class(im)->notify_focus_in)
			im_class(im)->notify_focus_in(im);
	}
	field.focused = true;
	field_changed();
}

void ui_test_web_field_set(const char *text)
{
	if (!field.text)
		field.text = g_string_new(NULL);
	g_string_assign(field.text, text);
	field.cursor = field.text->len;
	field_changed();
}

void ui_test_web_blur(void)
{
	field_blur();
}

const char *ui_test_web_field_text(void)
{
	return field.text ? field.text->str : "";
}

bool ui_test_web_field_focused(void)
{
	return field.focused;
}

unsigned ui_test_web_enters(void)
{
	return field.enters;
}

void ui_test_web_broken(bool b)
{
	broken = b;
}

void ui_test_web_crash(void)
{
	if (last_view)
		g_signal_emit(last_view, signals[WEB_PROCESS_TERMINATED], 0, WEBKIT_WEB_PROCESS_CRASHED);
}

void ui_test_web_new_window(const char *url)
{
	if (!last_view)
		return;
	WebKitURIRequest request = { .uri = (char *)url };
	WebKitNavigationAction action = { .request = &request };
	gpointer created = NULL;
	g_signal_emit(last_view, signals[CREATE], 0, &action, &created);
}

unsigned ui_test_web_touch_count(void)
{
	return touch_count;
}

const struct ui_test_touch *ui_test_web_touch(unsigned i)
{
	return i < touch_count ? &touches[i] : NULL;
}

unsigned ui_test_web_views(void)
{
	return views;
}

unsigned ui_test_web_frames(void)
{
	return frames;
}

unsigned ui_test_web_images(void)
{
	return images;
}

float ui_test_web_scale(void)
{
	return scale;
}

uint32_t ui_test_web_activity(void)
{
	return activity;
}

const char *ui_test_web_html(void)
{
	return last_html ? last_html : "";
}

const char *ui_test_web_user_agent(void)
{
	return last_settings && last_settings->user_agent ? last_settings->user_agent : "";
}

bool ui_test_web_feature(const char *identifier)
{
	for (gsize i = 0; last_settings && i < FEATURES; i++)
		if (!strcmp(feature_ids[i], identifier))
			return last_settings->enabled[i];
	return false;
}

void ui_test_web_size(int32_t *w, int32_t *h)
{
	*w = last_exportable ? (int32_t)last_exportable->w : 0;
	*h = last_exportable ? (int32_t)last_exportable->h : 0;
}

void ui_test_web_frame_size(int32_t *w, int32_t *h)
{
	*w = (int32_t)frame_w;
	*h = (int32_t)frame_h;
}

unsigned ui_test_web_frames_waited(void)
{
	return frames_waited;
}

void ui_test_web_animate(bool animate)
{
	animating = animate;
	struct wpe_view_backend_exportable_fdo *e = last_exportable;
	if (animate && e)
		frame_want(e, e->color);
}

void ui_test_web_hold_frames(bool hold)
{
	frames_held = hold;
	struct wpe_view_backend_exportable_fdo *e = last_exportable;
	if (!hold && e && e->wanted && !e->waiting && !e->idle)
		e->idle = g_idle_add(export_cb, e);
}
