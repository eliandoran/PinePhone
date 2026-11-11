/*
 * Playing a video file (video.h) with GStreamer: filesrc ! parsebin, then for each
 * stream it finds, a branch made by us and linked before any data flows:
 *   video:  queue ! <decoder> ! appsink (NV12)
 *   audio:  queue ! <decoder> ! audioconvert ! audioresample ! alsasink (the device given)
 *   other:  queue ! fakesink
 * The decoder is the highest ranked one for the stream's caps: for video the video
 * engine's (v4l2codecs: v4l2slh264dec...); the image has no software video decoder.
 *
 * Not playbin: it holds the decoded streams back until it found them all, so the
 * decoder's first allocation query (on its first sequence, the only one it reads)
 * reached no sink, it took that as no GstVideoMeta, and copied every frame out of
 * uncached memory to crop it (1088 lines decoded for 1080 shown): 20 fps of a 24 fps
 * 1080p film at 648 MHz. Linked to the appsink first, its query does reach it, where a
 * pad probe says the sink takes GstVideoMeta (frames with their own strides and
 * offsets), and the frames are handed over as decoded.
 *
 * Everything runs from LVGL's thread but the branches, made in parsebin's streaming
 * thread (pad-added): the bus is read in video_poll(), no GLib main loop.
 *
 * GStreamer's registry (the plugins found) is kept in /var/lib/phone-ui: the root
 * file system is read-only, and scanning the plugins takes seconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "system/video.h"

#define REGISTRY "/var/lib/phone-ui/gstreamer-registry.bin"
#define QUEUE_NS (2 * GST_SECOND) /* each branch's queue: the file's streams interleave */

struct video {
	GstElement *pipeline, *sink;
	GstBus *bus;
	char *audio_device;
	enum video_state state;
	bool playing;    /* asked to */
	bool shown;      /* a frame was taken */
	bool ended;
	bool seeking;    /* a frame of the new position is wanted, playing or not */
	/* Set from parsebin's thread */
	int has_video, has_audio, no_video;
	char no_decoder[128]; /* the caps of a video stream without a decoder, then no_video */
	GstClockTime last_pts;
	GstSample *sample; /* the frame taken, until the next one */
	GstVideoFrame frame;
	bool mapped;
	int width, height;
	uint32_t duration;
	/* ms, read in video_poll(): during a seek the place sought, which GStreamer does not
	 * say until a frame of it came (0 meanwhile: the bar jumped to the start) */
	uint32_t pos, seek_ms;
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
	if (GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION &&
	    !gst_query_find_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL))
		gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL);
	return GST_PAD_PROBE_OK;
}

static void fail(struct video *v, const char *what, const char *detail)
{
	v->state = VIDEO_FAILED;
	snprintf(v->error, sizeof(v->error), "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
}

/* The highest ranked decoder of `media` (GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO...) for caps */
static GstElement *decoder_for(GstCaps *caps, GstElementFactoryListType media)
{
	GList *all = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER | media,
	                                                   GST_RANK_MARGINAL);
	GList *fit = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
	fit = g_list_sort(fit, gst_plugin_feature_rank_compare_func);
	GstElement *e = fit ? gst_element_factory_create(GST_ELEMENT_FACTORY(fit->data), NULL) : NULL;
	gst_plugin_feature_list_free(fit);
	gst_plugin_feature_list_free(all);
	return e;
}

static GstElement *element(const char *factory)
{
	GstElement *e = gst_element_factory_make(factory, NULL);
	if (!e)
		printf("video: no %s\n", factory);
	return e;
}

/* `pad` into the elements, linked in order: false (and none added) if one is missing */
static bool branch(struct video *v, GstPad *pad, GstElement **chain, int n)
{
	for (int i = 0; i < n; i++) {
		if (!chain[i]) {
			for (int j = 0; j < n; j++)
				if (chain[j] && chain[j] != v->sink)
					gst_object_unref(gst_object_ref_sink(chain[j]));
			return false;
		}
	}
	for (int i = 0; i < n; i++)
		gst_bin_add(GST_BIN(v->pipeline), chain[i]);
	bool ok = true;
	for (int i = 0; i + 1 < n && ok; i++)
		ok = gst_element_link(chain[i], chain[i + 1]);
	for (int i = n - 1; i >= 0; i--)
		gst_element_sync_state_with_parent(chain[i]);
	GstPad *in = gst_element_get_static_pad(chain[0], "sink");
	ok = ok && gst_pad_link(pad, in) == GST_PAD_LINK_OK;
	gst_object_unref(in);
	return ok;
}

static GstElement *queue(void)
{
	GstElement *q = element("queue");
	if (q)
		g_object_set(q, "max-size-buffers", 0u, "max-size-bytes", 0u, "max-size-time", (guint64)QUEUE_NS, NULL);
	return q;
}

/* A stream parsebin found (its thread): the first video and audio ones played, the rest dropped */
static void pad_added_cb(GstElement *parsebin, GstPad *pad, gpointer data)
{
	(void)parsebin;
	struct video *v = data;
	GstCaps *caps = gst_pad_get_current_caps(pad);
	if (!caps)
		caps = gst_pad_query_caps(pad, NULL);
	const char *type = gst_structure_get_name(gst_caps_get_structure(caps, 0));
	bool done = false;
	if (g_str_has_prefix(type, "video/") && !__atomic_load_n(&v->has_video, __ATOMIC_ACQUIRE)) {
		GstElement *dec = decoder_for(caps, GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
		if (!dec) {
			gchar *s = gst_caps_to_string(caps);
			snprintf(v->no_decoder, sizeof(v->no_decoder), "%s", s);
			g_free(s);
		} else {
			GstElement *chain[] = { queue(), dec, v->sink };
			done = branch(v, pad, chain, 3);
			if (done)
				__atomic_store_n(&v->has_video, 1, __ATOMIC_RELEASE);
		}
	} else if (g_str_has_prefix(type, "audio/") && !v->has_audio) {
		GstElement *dec = decoder_for(caps, GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO);
		GstElement *out = v->audio_device ? element("alsasink") : element("fakesink");
		if (out && v->audio_device)
			g_object_set(out, "device", v->audio_device, NULL);
		if (!dec) {
			gchar *s = gst_caps_to_string(caps);
			printf("video: no decoder for the sound (%s)\n", s);
			g_free(s);
		}
		GstElement *chain[] = { queue(), dec, element("audioconvert"), element("audioresample"), out };
		done = branch(v, pad, chain, 5);
		v->has_audio = done;
	}
	if (!done) {
		/* Not played: drained, or the pipeline would stop at it (not linked) */
		GstElement *sink = element("fakesink");
		if (sink)
			g_object_set(sink, "sync", FALSE, "async", FALSE, NULL);
		GstElement *chain[] = { queue(), sink };
		branch(v, pad, chain, 2);
	}
	gst_caps_unref(caps);
}

static void no_more_pads_cb(GstElement *parsebin, gpointer data)
{
	(void)parsebin;
	struct video *v = data;
	if (!__atomic_load_n(&v->has_video, __ATOMIC_ACQUIRE))
		__atomic_store_n(&v->no_video, 1, __ATOMIC_RELEASE);
}

struct video *video_open(const char *path, const char *audio_device)
{
	struct video *v = calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->last_pts = GST_CLOCK_TIME_NONE;
	v->playing = true;
	if (!init()) {
		fail(v, "GStreamer did not start", NULL); /* NON-NLS: for the log */
		return v;
	}
	v->audio_device = audio_device ? strdup(audio_device) : NULL;

	v->pipeline = gst_pipeline_new(NULL);
	gst_object_ref_sink(v->pipeline);
	GstElement *src = element("filesrc"), *parse = element("parsebin");
	v->sink = element("appsink");
	if (!src || !parse || !v->sink) {
		fail(v, "missing GStreamer elements", NULL); /* NON-NLS: for the log */
		GstElement *made[] = { src, parse, v->sink };
		for (int i = 0; i < 3; i++)
			if (made[i])
				gst_object_unref(gst_object_ref_sink(made[i]));
		v->sink = NULL;
		return v;
	}
	gst_object_ref_sink(v->sink); /* ours, also once in the pipeline */
	GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12");
	g_object_set(v->sink, "caps", caps, "max-buffers", 2u, "drop", TRUE, "sync", TRUE,
	             "enable-last-sample", FALSE, NULL);
	gst_caps_unref(caps);
	GstPad *pad = gst_element_get_static_pad(v->sink, "sink");
	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation_cb, NULL, NULL);
	gst_object_unref(pad);

	g_object_set(src, "location", path, NULL);
	gst_bin_add_many(GST_BIN(v->pipeline), src, parse, NULL);
	gst_element_link(src, parse);
	g_signal_connect(parse, "pad-added", G_CALLBACK(pad_added_cb), v);
	g_signal_connect(parse, "no-more-pads", G_CALLBACK(no_more_pads_cb), v);

	v->bus = gst_element_get_bus(v->pipeline);
	if (gst_element_set_state(v->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		fail(v, "cannot play", path); /* NON-NLS: for the log; the bus may say why: video_poll() reads it */
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
	free(v->audio_device);
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
	if (v->state != VIDEO_FAILED && __atomic_load_n(&v->no_video, __ATOMIC_ACQUIRE))
		fail(v, v->no_decoder[0] ? "no video decoder for" : "no video in the file", /* NON-NLS: for the log */
		     v->no_decoder[0] ? v->no_decoder : NULL);
	if (v->state == VIDEO_FAILED)
		return v->state;
	if (!v->duration && v->pipeline) {
		gint64 d;
		if (gst_element_query_duration(v->pipeline, GST_FORMAT_TIME, &d) && d > 0)
			v->duration = (uint32_t)(d / GST_MSECOND);
	}
	gint64 p;
	if (v->seeking)
		v->pos = v->seek_ms;
	else if (v->pipeline && gst_element_query_position(v->pipeline, GST_FORMAT_TIME, &p) && p >= 0)
		v->pos = (uint32_t)(p / GST_MSECOND); /* else the last one known */
	v->state = v->ended ? VIDEO_ENDED : !v->shown ? VIDEO_LOADING : v->playing ? VIDEO_PLAYING : VIDEO_PAUSED;
	return v->state;
}

const char *video_error(const struct video *v)
{
	return v->error;
}

/* Each plane's dma-buf and offset in it, where its memory is one (the video engine's) */
static void dmabufs(GstBuffer *b, const GstVideoFrame *frame, struct video_frame *f)
{
	for (int p = 0; p < 2; p++) {
		guint idx, len;
		gsize skip;
		f->fd[p] = -1;
		if (!gst_buffer_find_memory(b, GST_VIDEO_FRAME_PLANE_OFFSET(frame, p), 1, &idx, &len, &skip))
			continue;
		GstMemory *m = gst_buffer_peek_memory(b, idx);
		if (gst_is_dmabuf_memory(m)) {
			f->fd[p] = gst_dmabuf_memory_get_fd(m);
			f->offset[p] = m->offset + skip;
		}
	}
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
	dmabufs(b, &v->frame, f);
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

void video_play(struct video *v, bool play)
{
	if (!v->pipeline || v->state == VIDEO_FAILED)
		return;
	if (play && v->ended) {
		v->ended = false;
		gst_element_seek_simple(v->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
		v->pos = 0;
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
		v->seek_ms = v->pos = ms;
	}
}

uint32_t video_position(const struct video *v)
{
	return v->ended ? v->duration : v->pos;
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
