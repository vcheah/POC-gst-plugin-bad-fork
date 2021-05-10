/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-d3d11videosink
 * @title: d3d11videosink
 *
 * Direct3D11 based video render element
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 videotestsrc ! d3d11videosink
 * ```
 * This pipeline will display test video stream on screen via #d3d11videosink
 *
 * Since: 1.18
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/videooverlay.h>
#include <gst/video/navigation.h>
#include <gst/d3d11/gstd3d11.h>
#include "gstd3d11videosink.h"
#include "gstd3d11videosinkbin.h"
#include "gstd3d11pluginutils.h"

enum
{
  PROP_0,
  /* basesink */
  PROP_SYNC,
  PROP_MAX_LATENESS,
  PROP_QOS,
  PROP_ASYNC,
  PROP_TS_OFFSET,
  PROP_ENABLE_LAST_SAMPLE,
  PROP_LAST_SAMPLE,
  PROP_BLOCKSIZE,
  PROP_RENDER_DELAY,
  PROP_THROTTLE_TIME,
  PROP_MAX_BITRATE,
  PROP_PROCESSING_DEADLINE,
  PROP_STATS,
  /* videosink */
  PROP_SHOW_PREROLL_FRAME,
  /* d3d11videosink */
  PROP_ADAPTER,
  PROP_FORCE_ASPECT_RATIO,
  PROP_ENABLE_NAVIGATION_EVENTS,
  PROP_FULLSCREEN_TOGGLE_MODE,
  PROP_FULLSCREEN,
  PROP_RENDER_STATS,
  PROP_DRAW_ON_SHARED_TEXTURE,
};

/* basesink */
#define DEFAULT_SYNC                TRUE
#define DEFAULT_MAX_LATENESS        -1
#define DEFAULT_QOS                 FALSE
#define DEFAULT_ASYNC               TRUE
#define DEFAULT_TS_OFFSET           0
#define DEFAULT_BLOCKSIZE           4096
#define DEFAULT_RENDER_DELAY        0
#define DEFAULT_ENABLE_LAST_SAMPLE  TRUE
#define DEFAULT_THROTTLE_TIME       0
#define DEFAULT_MAX_BITRATE         0
#define DEFAULT_DROP_OUT_OF_SEGMENT TRUE
#define DEFAULT_PROCESSING_DEADLINE (20 * GST_MSECOND)

/* videosink */
#define DEFAULT_SHOW_PREROLL_FRAME TRUE

/* d3d11videosink */
#define DEFAULT_ADAPTER                   -1
#define DEFAULT_FORCE_ASPECT_RATIO        TRUE
#define DEFAULT_ENABLE_NAVIGATION_EVENTS  TRUE
#define DEFAULT_FULLSCREEN_TOGGLE_MODE    GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_NONE
#define DEFAULT_FULLSCREEN                FALSE
#define DEFAULT_RENDER_STATS              FALSE
#define DEFAULT_DRAW_ON_SHARED_TEXTURE    FALSE

enum
{
  /* signals */
  SIGNAL_BEGIN_DRAW,

  /* actions */
  SIGNAL_DRAW,

  LAST_SIGNAL
};

static guint gst_d3d11_video_sink_bin_signals[LAST_SIGNAL] = { 0, };

static GstStaticCaps pad_template_caps =
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, GST_D3D11_SINK_FORMATS) "; "
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY ","
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,
        GST_D3D11_SINK_FORMATS) ";"
    GST_VIDEO_CAPS_MAKE (GST_D3D11_SINK_FORMATS) "; "
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES
    (GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY ","
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION,
        GST_D3D11_SINK_FORMATS));

GST_DEBUG_CATEGORY (d3d11_video_sink_bin_debug);
#define GST_CAT_DEFAULT d3d11_video_sink_bin_debug

struct _GstD3D11VideoSinkBin
{
  GstBin parent;

  GstPad *sinkpad;

  GstElement *upload;
  GstElement *sink;
};

static void gst_d3d11_video_sink_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_d3d11_video_sink_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static void
gst_d3d11_video_sink_bin_video_overlay_init (GstVideoOverlayInterface * iface);
static void
gst_d3d11_video_sink_bin_navigation_init (GstNavigationInterface * iface);
static void gst_d311_video_sink_bin_on_begin_draw (GstD3D11VideoSink * sink,
    gpointer self);
static gboolean
gst_d3d11_video_sink_bin_draw_action (GstD3D11VideoSinkBin * self,
    gpointer shared_handle, guint texture_misc_flags, guint64 acquire_key,
    guint64 release_key);

#define gst_d3d11_video_sink_bin_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstD3D11VideoSinkBin, gst_d3d11_video_sink_bin,
    GST_TYPE_BIN,
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_d3d11_video_sink_bin_video_overlay_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_NAVIGATION,
        gst_d3d11_video_sink_bin_navigation_init);
    GST_DEBUG_CATEGORY_INIT (d3d11_video_sink_bin_debug,
        "d3d11videosink", 0, "Direct3D11 Video Sink"));

static void
gst_d3d11_video_sink_bin_class_init (GstD3D11VideoSinkBinClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstCaps *caps;

  gobject_class->set_property = gst_d3d11_video_sink_bin_set_property;
  gobject_class->get_property = gst_d3d11_video_sink_bin_get_property;

  /* basesink */
  g_object_class_install_property (gobject_class, PROP_SYNC,
      g_param_spec_boolean ("sync", "Sync", "Sync on the clock", DEFAULT_SYNC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_LATENESS,
      g_param_spec_int64 ("max-lateness", "Max Lateness",
          "Maximum number of nanoseconds that a buffer can be late before it "
          "is dropped (-1 unlimited)", -1, G_MAXINT64, DEFAULT_MAX_LATENESS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QOS,
      g_param_spec_boolean ("qos", "Qos",
          "Generate Quality-of-Service events upstream", DEFAULT_QOS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ASYNC,
      g_param_spec_boolean ("async", "Async",
          "Go asynchronously to PAUSED", DEFAULT_ASYNC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_TS_OFFSET,
      g_param_spec_int64 ("ts-offset", "TS Offset",
          "Timestamp offset in nanoseconds", G_MININT64, G_MAXINT64,
          DEFAULT_TS_OFFSET, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ENABLE_LAST_SAMPLE,
      g_param_spec_boolean ("enable-last-sample", "Enable Last Buffer",
          "Enable the last-sample property", DEFAULT_ENABLE_LAST_SAMPLE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LAST_SAMPLE,
      g_param_spec_boxed ("last-sample", "Last Sample",
          "The last sample received in the sink", GST_TYPE_SAMPLE,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BLOCKSIZE,
      g_param_spec_uint ("blocksize", "Block size",
          "Size in bytes to pull per buffer (0 = default)", 0, G_MAXUINT,
          DEFAULT_BLOCKSIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_RENDER_DELAY,
      g_param_spec_uint64 ("render-delay", "Render Delay",
          "Additional render delay of the sink in nanoseconds", 0, G_MAXUINT64,
          DEFAULT_RENDER_DELAY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_THROTTLE_TIME,
      g_param_spec_uint64 ("throttle-time", "Throttle time",
          "The time to keep between rendered buffers (0 = disabled)", 0,
          G_MAXUINT64, DEFAULT_THROTTLE_TIME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_BITRATE,
      g_param_spec_uint64 ("max-bitrate", "Max Bitrate",
          "The maximum bits per second to render (0 = disabled)", 0,
          G_MAXUINT64, DEFAULT_MAX_BITRATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PROCESSING_DEADLINE,
      g_param_spec_uint64 ("processing-deadline", "Processing deadline",
          "Maximum processing deadline in nanoseconds", 0, G_MAXUINT64,
          DEFAULT_PROCESSING_DEADLINE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_STATS,
      g_param_spec_boxed ("stats", "Statistics",
          "Sink Statistics", GST_TYPE_STRUCTURE,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* videosink */
  g_object_class_install_property (gobject_class, PROP_SHOW_PREROLL_FRAME,
      g_param_spec_boolean ("show-preroll-frame", "Show preroll frame",
          "Whether to render video frames during preroll",
          DEFAULT_SHOW_PREROLL_FRAME,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /* d3d11videosink */
  g_object_class_install_property (gobject_class, PROP_ADAPTER,
      g_param_spec_int ("adapter", "Adapter",
          "Adapter index for creating device (-1 for default)",
          -1, G_MAXINT32, DEFAULT_ADAPTER,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio",
          "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio",
          DEFAULT_FORCE_ASPECT_RATIO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_ENABLE_NAVIGATION_EVENTS,
      g_param_spec_boolean ("enable-navigation-events",
          "Enable navigation events",
          "When enabled, navigation events are sent upstream",
          DEFAULT_ENABLE_NAVIGATION_EVENTS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FULLSCREEN_TOGGLE_MODE,
      g_param_spec_flags ("fullscreen-toggle-mode",
          "Full screen toggle mode",
          "Full screen toggle mode used to trigger fullscreen mode change",
          GST_D3D11_WINDOW_TOGGLE_MODE_GET_TYPE, DEFAULT_FULLSCREEN_TOGGLE_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FULLSCREEN,
      g_param_spec_boolean ("fullscreen",
          "fullscreen",
          "Ignored when \"fullscreen-toggle-mode\" does not include \"property\"",
          DEFAULT_FULLSCREEN, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#ifdef HAVE_DIRECT_WRITE
  g_object_class_install_property (gobject_class, PROP_RENDER_STATS,
      g_param_spec_boolean ("render-stats",
          "Render Stats",
          "Render statistics data (e.g., average framerate) on window",
          DEFAULT_RENDER_STATS,
          GST_PARAM_CONDITIONALLY_AVAILABLE | GST_PARAM_MUTABLE_READY |
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#endif

  /**
   * GstD3D11VideoSinkBin:draw-on-shared-texture:
   *
   * Instruct the sink to draw on a shared texture provided by user.
   * User must watch #d3d11videosink::begin-draw signal and should call
   * #d3d11videosink::draw method on the #d3d11videosink::begin-draw
   * signal handler.
   *
   * Currently supported formats for user texture are:
   * - DXGI_FORMAT_R8G8B8A8_UNORM
   * - DXGI_FORMAT_B8G8R8A8_UNORM
   * - DXGI_FORMAT_R10G10B10A2_UNORM
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class, PROP_DRAW_ON_SHARED_TEXTURE,
      g_param_spec_boolean ("draw-on-shared-texture",
          "Draw on shared texture",
          "Draw on user provided shared texture instead of window. "
          "When enabled, user can pass application's own texture to sink "
          "by using \"draw\" action signal on \"begin-draw\" signal handler, "
          "so that sink can draw video data on application's texture. "
          "Supported texture formats for user texture are "
          "DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, and "
          "DXGI_FORMAT_R10G10B10A2_UNORM.",
          DEFAULT_DRAW_ON_SHARED_TEXTURE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));

  /**
   * GstD3D11VideoSinkBin::begin-draw:
   * @videosink: the #d3d11videosink
   *
   * Emitted when sink has a texture to draw. Application needs to invoke
   * #d3d11videosink::draw action signal before returning from
   * #d3d11videosink::begin-draw signal handler.
   *
   * Since: 1.20
   */
  gst_d3d11_video_sink_bin_signals[SIGNAL_BEGIN_DRAW] =
      g_signal_new ("begin-draw", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstD3D11VideoSinkBinClass, begin_draw),
      NULL, NULL, NULL, G_TYPE_NONE, 0, G_TYPE_NONE);

  /**
   * GstD3D11VideoSinkBin::draw:
   * @videosink: the #d3d11videosink
   * @shard_handle: a pointer to HANDLE
   * @texture_misc_flags: a D3D11_RESOURCE_MISC_FLAG value
   * @acquire_key: a key value used for IDXGIKeyedMutex::AcquireSync
   * @release_key: a key value used for IDXGIKeyedMutex::ReleaseSync
   *
   * Draws on a shared texture. @shard_handle must be a valid pointer to
   * a HANDLE which was obtained via IDXGIResource::GetSharedHandle or
   * IDXGIResource1::CreateSharedHandle.
   *
   * If the texture was created with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX flag,
   * caller must specify valid @acquire_key and @release_key.
   * Otherwise (i.e., created with D3D11_RESOURCE_MISC_SHARED flag),
   * @acquire_key and @release_key will be ignored.
   *
   * Since: 1.20
   */
  gst_d3d11_video_sink_bin_signals[SIGNAL_DRAW] =
      g_signal_new ("draw", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GstD3D11VideoSinkBinClass, draw), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 4, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_UINT64,
      G_TYPE_UINT64);

  klass->draw = gst_d3d11_video_sink_bin_draw_action;

  gst_element_class_set_static_metadata (element_class,
      "Direct3D11 video sink bin", "Sink/Video",
      "A Direct3D11 based videosink bin",
      "Seungha Yang <seungha.yang@navercorp.com>");

  caps = gst_d3d11_get_updated_template_caps (&pad_template_caps);
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);
}

static void
gst_d3d11_video_sink_bin_init (GstD3D11VideoSinkBin * self)
{
  GstPad *pad;
  GstD3D11VideoSinkCallbacks callbacks;

  self->upload = gst_element_factory_make ("d3d11upload", NULL);
  if (!self->upload) {
    GST_ERROR_OBJECT (self, "d3d11upload unavailable");
    return;
  }

  self->sink = gst_element_factory_make ("d3d11videosinkelement", NULL);
  if (!self->sink) {
    gst_clear_object (&self->upload);
    GST_ERROR_OBJECT (self, "d3d11videosinkelement unavailable");
    return;
  }

  callbacks.begin_draw = gst_d311_video_sink_bin_on_begin_draw;
  gst_d3d11_video_sink_set_callbacks (GST_D3D11_VIDEO_SINK (self->sink),
      &callbacks, self);

  gst_bin_add_many (GST_BIN (self), self->upload, self->sink, NULL);

  gst_element_link_many (self->upload, self->sink, NULL);

  pad = gst_element_get_static_pad (self->upload, "sink");

  self->sinkpad = gst_ghost_pad_new ("sink", pad);
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->sinkpad);
  gst_object_unref (pad);
}

static void
gst_d3d11_video_sink_bin_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (object);
  GParamSpec *sink_pspec;

  sink_pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (self->sink),
      pspec->name);

  if (sink_pspec && G_PARAM_SPEC_TYPE (sink_pspec) == G_PARAM_SPEC_TYPE (pspec)) {
    g_object_set_property (G_OBJECT (self->sink), pspec->name, value);
  } else {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_d3d11_video_sink_bin_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (object);

  g_object_get_property (G_OBJECT (self->sink), pspec->name, value);
}

static void
gst_d311_video_sink_bin_on_begin_draw (GstD3D11VideoSink * sink, gpointer self)
{
  g_signal_emit (self, gst_d3d11_video_sink_bin_signals[SIGNAL_BEGIN_DRAW], 0,
      NULL);
}

static gboolean
gst_d3d11_video_sink_bin_draw_action (GstD3D11VideoSinkBin * self,
    gpointer shared_handle, guint texture_misc_flags, guint64 acquire_key,
    guint64 release_key)
{
  if (!self->sink) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("D3D11VideoSink element wasn't configured"), (NULL));
    return FALSE;
  }

  return gst_d3d11_video_sink_draw (GST_D3D11_VIDEO_SINK (self->sink),
      shared_handle, texture_misc_flags, acquire_key, release_key);
}

/* VideoOverlay interface */
static void
gst_d3d11_video_sink_bin_set_window_handle (GstVideoOverlay * overlay,
    guintptr window_id)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (overlay);

  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (self->sink),
      window_id);
}

static void
gst_d3d11_video_sink_bin_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint width, gint height)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (overlay);

  gst_video_overlay_set_render_rectangle (GST_VIDEO_OVERLAY (self->sink),
      x, y, width, height);
}

static void
gst_d3d11_video_sink_bin_expose (GstVideoOverlay * overlay)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (overlay);

  gst_video_overlay_expose (GST_VIDEO_OVERLAY (self->sink));
}

static void
gst_d3d11_video_sink_bin_handle_events (GstVideoOverlay * overlay,
    gboolean handle_events)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (overlay);

  gst_video_overlay_handle_events (GST_VIDEO_OVERLAY (self->sink),
      handle_events);
}

static void
gst_d3d11_video_sink_bin_video_overlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_d3d11_video_sink_bin_set_window_handle;
  iface->set_render_rectangle = gst_d3d11_video_sink_bin_set_render_rectangle;
  iface->expose = gst_d3d11_video_sink_bin_expose;
  iface->handle_events = gst_d3d11_video_sink_bin_handle_events;
}

/* Navigation interface */
static void
gst_d3d11_video_sink_bin_navigation_send_event (GstNavigation * navigation,
    GstStructure * structure)
{
  GstD3D11VideoSinkBin *self = GST_D3D11_VIDEO_SINK_BIN (navigation);

  gst_navigation_send_event (GST_NAVIGATION (self->sink), structure);
}

static void
gst_d3d11_video_sink_bin_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event = gst_d3d11_video_sink_bin_navigation_send_event;
}
