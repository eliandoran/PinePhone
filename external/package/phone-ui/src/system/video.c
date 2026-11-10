/*
 * Playing a video file (video.h) with GStreamer's playbin. The picture goes to an
 * appsink as NV12, which the video engine's decoders (v4l2codecs: v4l2slh264dec...,
 * ranked above software ones) write; the sink says it takes GstVideoMeta (a pad probe
 * answers the decoder's allocation query), or the decoder would copy each frame out
 * of uncached memory to crop it (848 lines decoded for 844 shown). A software
 * decoder's I420 goes through playbin's videoconvert. The sound goes to alsasink on
 * the device given. Everything runs from LVGL's thread: the bus is read in
 * video_poll(), no GLib main loop; GStreamer's threads only stream.
 *
 * GStreamer's registry (the plugins found) is kept in /var/lib/phone-ui: the root
 * file system is read-only, and scanning the plugins takes seconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "system/video.h"

#define REGISTRY "/var/lib/phone-ui/gstreamer-registry.bin"

/* playbin's flags (GstPlayFlags, not in a public header): video, audio, soft volume;
 * no subtitles, visualisations, deinterlacing or color balance */
#define PLAY_FLAGS (0x1 | 0x2 | 0x10)

struct video {
	GstElement *pipeline, *sink;
	GstBus *bus;
	enum video_state state;
	bool playing;    /* asked to */
	bool shown;      /* a frame was taken */
	bool ended;
	bool seeking;    /* a frame of the new position is wanted, playing or not */
	GstClockTime last_pts;
	GstSample *sample; /* the frame taken, until video_frame_done() */
	GstVideoFrame frame;
	bool mapped;
	int width, height;
	uint32_t duration;
	char error[256];
};

static bool init(void)
{
	static int ok = -1;
	if (ok < 0) {
		setenv("GST_REGISTRY", REGISTRY, 0);
		GError *err = NULL;
		ok = gst_init_check(NULL, NULL, &err);
		if (!ok) {
			printf("video: GStreamer: %s\n", err ? err->message : "?");
			g_clear_error(&err);
		}
	}
	return ok;
}

/* The decoder's allocation query: frames with their own layout (strides, offsets) are fine */
static GstPadProbeReturn allocation_cb(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
	(void)pad, (void)data;
	GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
	if (GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION)
		gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL);
	return GST_PAD_PROBE_OK;
}

static void fail(struct video *v, const char *what, const char *detail)
{
	v->state = VIDEO_FAILED;
	snprintf(v->error, sizeof(v->error), "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
}

struct video *video_open(const char *path, const char *audio_device)
{
	struct video *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->last_pts = GST_CLOCK_TIME_NONE;
	v->playing = true;
	if (!init()) {
		fail(v, "GStreamer did not start", NULL);
		return v;
	}

	v->pipeline = gst_element_factory_make("playbin", NULL);
	if (v->pipeline)
		gst_object_ref_sink(v->pipeline); /* ours, not floating */
	v->sink = gst_element_factory_make("appsink", NULL);
	if (!v->pipeline || !v->sink) {
		fail(v, "missing GStreamer elements", v->pipeline ? "appsink" : "playbin");
		if (v->sink)
			gst_object_unref(gst_object_ref_sink(v->sink));
		v->sink = NULL;
		return v;
	}
	GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12");
	g_object_set(v->sink, "caps", caps, "max-buffers", 2u, "drop", TRUE, "sync", TRUE,
	             "enable-last-sample", FALSE, NULL);
	gst_caps_unref(caps);
	GstPad *pad = gst_element_get_static_pad(v->sink, "sink");
	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation_cb, NULL, NULL);
	gst_object_unref(pad);
	gst_object_ref(v->sink); /* kept: playbin owns it once set */

	gchar *uri = gst_filename_to_uri(path, NULL);
	g_object_set(v->pipeline, "uri", uri, "video-sink", v->sink, "flags", PLAY_FLAGS, NULL);
	g_free(uri);
	if (audio_device) {
		GstElement *audio = gst_element_factory_make("alsasink", NULL);
		if (audio) {
			g_object_set(audio, "device", audio_device, NULL);
			g_object_set(v->pipeline, "audio-sink", audio, NULL);
		} else {
			printf("video: no alsasink, no sound\n");
		}
	}

	v->bus = gst_element_get_bus(v->pipeline);
	if (gst_element_set_state(v->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		fail(v, "cannot play", path); /* the bus may say why: video_poll() reads it */
	return v;
}

static void release(struct video *v)
{
	if (v->mapped)
		gst_video_frame_unmap(&v->frame);
	v->mapped = false;
	if (v->sample)
		gst_sample_unref(v->sample);
	v->sample = NULL;
}

void video_close(struct video *v)
{
	if (!v)
		return;
	release(v);
	if (v->pipeline) {
		gst_element_set_state(v->pipeline, GST_STATE_NULL);
		gst_object_unref(v->pipeline);
	}
	if (v->sink)
		gst_object_unref(v->sink);
	if (v->bus)
		gst_object_unref(v->bus);
	free(v);
}

static void error_message(struct video *v, GstMessage *m)
{
	GError *err = NULL;
	gchar *debug = NULL;
	gst_message_parse_error(m, &err, &debug);
	char what[128];
	snprintf(what, sizeof(what), "%s: %s", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), err ? err->message : "?");
	fail(v, what, debug);
	g_clear_error(&err);
	g_free(debug);
}

enum video_state video_poll(struct video *v)
{
	GstMessage *m;
	while (v->bus && v->state != VIDEO_FAILED && (m = gst_bus_pop(v->bus))) {
		switch (GST_MESSAGE_TYPE(m)) {
		case GST_MESSAGE_ERROR:
			error_message(v, m);
			break;
		case GST_MESSAGE_WARNING: {
			GError *err = NULL;
			gst_message_parse_warning(m, &err, NULL);
			printf("video: %s: %s\n", GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), err ? err->message : "?");
			g_clear_error(&err);
			break;
		}
		case GST_MESSAGE_EOS:
			v->ended = true;
			v->playing = false;
			gst_element_set_state(v->pipeline, GST_STATE_PAUSED);
			break;
		case GST_MESSAGE_DURATION_CHANGED:
			v->duration = 0; /* asked again */
			break;
		default:
			break;
		}
		gst_message_unref(m);
	}
	if (v->state == VIDEO_FAILED)
		return v->state;
	if (!v->duration && v->pipeline) {
		gint64 d;
		if (gst_element_query_duration(v->pipeline, GST_FORMAT_TIME, &d) && d > 0)
			v->duration = (uint32_t)(d / GST_MSECOND);
	}
	v->state = v->ended ? VIDEO_ENDED : !v->shown ? VIDEO_LOADING : v->playing ? VIDEO_PLAYING : VIDEO_PAUSED;
	return v->state;
}

const char *video_error(const struct video *v)
{
	return v->error;
}

bool video_frame(struct video *v, struct video_frame *f)
{
	release(v);
	if (!v->sink || v->state == VIDEO_FAILED)
		return false;
	GstAppSink *sink = GST_APP_SINK(v->sink);
	GstSample *s = gst_app_sink_try_pull_sample(sink, 0);
	if (!s && (!v->playing || v->seeking)) {
		/* Paused: the frame it stopped on, once (a seek's: a new one) */
		s = gst_app_sink_try_pull_preroll(sink, 0);
		GstBuffer *b = s ? gst_sample_get_buffer(s) : NULL;
		if (s && (!b || (GST_BUFFER_PTS(b) == v->last_pts && !v->seeking))) {
			gst_sample_unref(s);
			s = NULL;
		}
	}
	if (!s)
		return false;

	GstVideoInfo info;
	GstBuffer *b = gst_sample_get_buffer(s);
	if (!b || !gst_video_info_from_caps(&info, gst_sample_get_caps(s)) ||
	    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
	    !gst_video_frame_map(&v->frame, &info, b, GST_MAP_READ)) {
		gst_sample_unref(s);
		return false;
	}
	v->sample = s;
	v->mapped = true;
	v->last_pts = GST_BUFFER_PTS(b);
	v->seeking = false;
	v->shown = true;
	v->width = GST_VIDEO_INFO_WIDTH(&info);
	v->height = GST_VIDEO_INFO_HEIGHT(&info);
	*f = (struct video_frame){
		.y = GST_VIDEO_FRAME_PLANE_DATA(&v->frame, 0),
		.uv = GST_VIDEO_FRAME_PLANE_DATA(&v->frame, 1),
		.width = v->width,
		.height = v->height,
		.stride = GST_VIDEO_FRAME_PLANE_STRIDE(&v->frame, 0),
		.bt709 = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709,
		.full_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255,
	};
	if (GST_VIDEO_FRAME_PLANE_STRIDE(&v->frame, 1) != f->stride) {
		static bool told;
		if (!told)
			printf("video: planes of different strides (%d, %d), not shown\n", f->stride,
			       GST_VIDEO_FRAME_PLANE_STRIDE(&v->frame, 1));
		told = true;
		release(v);
		return false;
	}
	return true;
}

void video_frame_done(struct video *v)
{
	release(v);
}

void video_play(struct video *v, bool play)
{
	if (!v->pipeline || v->state == VIDEO_FAILED)
		return;
	if (play && v->ended) {
		v->ended = false;
		gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
	}
	v->playing = play;
	gst_element_set_state(v->pipeline, play ? GST_STATE_PLAYING : GST_STATE_PAUSED);
}

void video_seek(struct video *v, uint32_t ms)
{
	if (!v->pipeline || v->state == VIDEO_FAILED)
		return;
	if (v->duration && ms >= v->duration)
		ms = v->duration > 500 ? v->duration - 500 : 0;
	if (gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
	                            (gint64)ms * GST_MSECOND)) {
		v->ended = false;
		v->seeking = true;
	}
}

uint32_t video_position(const struct video *v)
{
	if (v->ended)
		return v->duration;
	gint64 p;
	if (!v->pipeline || !gst_element_query_position(v->pipeline, GST_FORMAT_TIME, &p) || p < 0)
		return 0;
	return (uint32_t)(p / GST_MSECOND);
}

uint32_t video_duration(const struct video *v)
{
	return v->duration;
}

void video_size(const struct video *v, int *width, int *height)
{
	*width = v->width;
	*height = v->height;
}
