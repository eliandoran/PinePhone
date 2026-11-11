/*
 * WPE WebKit's API as apps/web/web.c uses it, for fake_webkit.c (test_web.c): the
 * declarations of WebKit 2.50.5's headers (the image's, wpe-webkit-2.0), only those, so
 * that web.c builds for the host without WebKit. The types are fake_webkit.c's; a
 * function web.c starts using goes here, as its header declares it, and in the fake.
 * libwpe's and WPEBackend-fdo's headers are the real ones (the flake's, the image's
 * versions).
 */

#ifndef FAKE_WEBKIT_H
#define FAKE_WEBKIT_H

#include <gio/gio.h>
#include <glib-object.h>
#include <wpe/wpe.h>

G_BEGIN_DECLS

typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
typedef struct _WebKitSettings WebKitSettings;
typedef struct _WebKitFeature WebKitFeature;
typedef struct _WebKitFeatureList WebKitFeatureList;
typedef struct _WebKitWebResource WebKitWebResource;
typedef struct _WebKitURIRequest WebKitURIRequest;
typedef struct _WebKitURIResponse WebKitURIResponse;
typedef struct _WebKitNavigationAction WebKitNavigationAction;
typedef struct _JSCValue JSCValue;

GType webkit_web_view_get_type(void);
#define WEBKIT_TYPE_WEB_VIEW (webkit_web_view_get_type())
#define WEBKIT_WEB_VIEW(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_WEB_VIEW, WebKitWebView))

typedef enum {
	WEBKIT_LOAD_STARTED,
	WEBKIT_LOAD_REDIRECTED,
	WEBKIT_LOAD_COMMITTED,
	WEBKIT_LOAD_FINISHED
} WebKitLoadEvent;

typedef enum {
	WEBKIT_WEB_PROCESS_CRASHED,
	WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT,
	WEBKIT_WEB_PROCESS_TERMINATED_BY_API
} WebKitWebProcessTerminationReason;

typedef enum {
	WEBKIT_NETWORK_ERROR_FAILED = 399,
	WEBKIT_NETWORK_ERROR_TRANSPORT = 300,
	WEBKIT_NETWORK_ERROR_UNKNOWN_PROTOCOL = 301,
	WEBKIT_NETWORK_ERROR_CANCELLED = 302,
	WEBKIT_NETWORK_ERROR_FILE_DOES_NOT_EXIST = 303
} WebKitNetworkError;

/* WebKitWebViewBackend.h */
WebKitWebViewBackend *webkit_web_view_backend_new(struct wpe_view_backend *backend, GDestroyNotify notify,
                                                  gpointer user_data);

/* WebKitWebView.h */
WebKitWebView *webkit_web_view_new(WebKitWebViewBackend *backend);
void webkit_web_view_load_uri(WebKitWebView *web_view, const gchar *uri);
void webkit_web_view_load_html(WebKitWebView *web_view, const gchar *content, const gchar *base_uri);
void webkit_web_view_load_request(WebKitWebView *web_view, WebKitURIRequest *request);
void webkit_web_view_stop_loading(WebKitWebView *web_view);
gboolean webkit_web_view_is_loading(WebKitWebView *web_view);
const gchar *webkit_web_view_get_uri(WebKitWebView *web_view);
const gchar *webkit_web_view_get_title(WebKitWebView *web_view);
void webkit_web_view_reload(WebKitWebView *web_view);
void webkit_web_view_go_back(WebKitWebView *web_view);
gboolean webkit_web_view_can_go_back(WebKitWebView *web_view);
WebKitSettings *webkit_web_view_get_settings(WebKitWebView *web_view);
void webkit_web_view_call_async_javascript_function(WebKitWebView *web_view, const char *body, gssize length,
                                                    GVariant *arguments, const char *world_name,
                                                    const char *source_uri, GCancellable *cancellable,
                                                    GAsyncReadyCallback callback, gpointer user_data);
JSCValue *webkit_web_view_call_async_javascript_function_finish(WebKitWebView *web_view, GAsyncResult *result,
                                                                GError **error);

/* WebKitInputMethodContext.h (WEBKIT_DECLARE_DERIVABLE_TYPE's instance: a private pointer) */
typedef struct _WebKitInputMethodContext WebKitInputMethodContext;
typedef struct _WebKitInputMethodContextClass WebKitInputMethodContextClass;
typedef struct _WebKitInputMethodContextPrivate WebKitInputMethodContextPrivate;
struct _WebKitInputMethodContext {
	GObject parent_instance;
	WebKitInputMethodContextPrivate *priv;
};
GType webkit_input_method_context_get_type(void);
#define WEBKIT_TYPE_INPUT_METHOD_CONTEXT (webkit_input_method_context_get_type())
#define WEBKIT_INPUT_METHOD_CONTEXT(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_INPUT_METHOD_CONTEXT, WebKitInputMethodContext))

typedef enum {
	WEBKIT_INPUT_PURPOSE_FREE_FORM,
	WEBKIT_INPUT_PURPOSE_DIGITS,
	WEBKIT_INPUT_PURPOSE_NUMBER,
	WEBKIT_INPUT_PURPOSE_PHONE,
	WEBKIT_INPUT_PURPOSE_URL,
	WEBKIT_INPUT_PURPOSE_EMAIL,
	WEBKIT_INPUT_PURPOSE_PASSWORD
} WebKitInputPurpose;

typedef enum {
	WEBKIT_INPUT_HINT_NONE = 0,
	WEBKIT_INPUT_HINT_SPELLCHECK = 1 << 0,
	WEBKIT_INPUT_HINT_LOWERCASE = 1 << 1,
	WEBKIT_INPUT_HINT_UPPERCASE_CHARS = 1 << 2,
	WEBKIT_INPUT_HINT_UPPERCASE_WORDS = 1 << 3,
	WEBKIT_INPUT_HINT_UPPERCASE_SENTENCES = 1 << 4,
	WEBKIT_INPUT_HINT_INHIBIT_OSK = 1 << 5
} WebKitInputHints;

struct _WebKitInputMethodContextClass {
	GObjectClass parent_class;
	void (*preedit_started)(WebKitInputMethodContext *context);
	void (*preedit_changed)(WebKitInputMethodContext *context);
	void (*preedit_finished)(WebKitInputMethodContext *context);
	void (*committed)(WebKitInputMethodContext *context, const char *text);
	void (*delete_surrounding)(WebKitInputMethodContext *context, int offset, guint n_chars);
	void (*set_enable_preedit)(WebKitInputMethodContext *context, gboolean enabled);
	void (*get_preedit)(WebKitInputMethodContext *context, gchar **text, GList **underlines,
	                    guint *cursor_offset);
	gboolean (*filter_key_event)(WebKitInputMethodContext *context, gpointer key_event);
	void (*notify_focus_in)(WebKitInputMethodContext *context);
	void (*notify_focus_out)(WebKitInputMethodContext *context);
	void (*notify_cursor_area)(WebKitInputMethodContext *context, int x, int y, int width, int height);
	void (*notify_surrounding)(WebKitInputMethodContext *context, const gchar *text, guint length,
	                           guint cursor_index, guint selection_index);
	void (*reset)(WebKitInputMethodContext *context);
	void (*_webkit_reserved[16])(void);
};

WebKitInputPurpose webkit_input_method_context_get_input_purpose(WebKitInputMethodContext *context);
WebKitInputHints webkit_input_method_context_get_input_hints(WebKitInputMethodContext *context);
void webkit_web_view_set_input_method_context(WebKitWebView *web_view, WebKitInputMethodContext *context);

/* WebKitSettings.h, WebKitFeature.h */
void webkit_settings_set_user_agent(WebKitSettings *settings, const gchar *user_agent);
WebKitFeatureList *webkit_settings_get_all_features(void);
void webkit_settings_set_feature_enabled(WebKitSettings *settings, WebKitFeature *feature, gboolean enabled);
void webkit_feature_list_unref(WebKitFeatureList *feature_list);
gsize webkit_feature_list_get_length(WebKitFeatureList *feature_list);
WebKitFeature *webkit_feature_list_get(WebKitFeatureList *feature_list, gsize index);
const char *webkit_feature_get_identifier(WebKitFeature *feature);

/* WebKitNavigationAction.h, WebKitWebResource.h, WebKitURIResponse.h */
WebKitURIRequest *webkit_navigation_action_get_request(WebKitNavigationAction *navigation);
GType webkit_web_resource_get_type(void);
const gchar *webkit_web_resource_get_uri(WebKitWebResource *resource);
WebKitURIResponse *webkit_web_resource_get_response(WebKitWebResource *resource);
guint64 webkit_uri_response_get_content_length(WebKitURIResponse *response);

/* jsc/JSCValue.h */
char *jsc_value_to_json(JSCValue *value, guint indent);

G_END_DECLS

#endif
