#include <stdlib.h>

#include <gst/gst.h>
#include <gst/video/video.h>

static gint num_buffers = 50;
static gboolean camera = FALSE;
static gboolean randomcb = FALSE;

static GOptionEntry entries[] = {
  {"num-buffers", 'n', 0, G_OPTION_ARG_INT, &num_buffers,
      "Number of buffers (<= 0 : forever)", "N"},
  {"camera", 'c', 0, G_OPTION_ARG_NONE, &camera, "Use v4l2src as video source",
      NULL},
  {"random-cb", 'r', 0, G_OPTION_ARG_NONE, &randomcb,
      "Change colorbalance randomly every second", NULL},
  {NULL},
};

struct _app
{
  GMainLoop *loop;
  GstObject *display;
  GstElement *pipeline;
  GstElement *vpp;
  GstElement *caps;
  GMutex mutex;
};

static GstBusSyncReply
context_handler (GstBus * bus, GstMessage * msg, gpointer data)
{
  struct _app *app = data;
  const gchar *context_type;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_HAVE_CONTEXT:{
      GstContext *context = NULL;

      gst_message_parse_have_context (msg, &context);
      if (context) {
        context_type = gst_context_get_context_type (context);

        if (g_strcmp0 (context_type, "gst.va.display.handle") == 0) {
          const GstStructure *s = gst_context_get_structure (context);
          GstObject *display = NULL;

          gst_printerr ("got have context %s from %s: ", context_type,
              GST_MESSAGE_SRC_NAME (msg));

          gst_structure_get (s, "gst-display", GST_TYPE_OBJECT, &display, NULL);
          gst_printerrln ("%s", display ?
              GST_OBJECT_NAME (display) : "no gst display");
          gst_context_unref (context);

          if (display) {
            g_mutex_lock (&app->mutex);
            gst_object_replace (&app->display, display);
            gst_object_unref (display);
            g_mutex_unlock (&app->mutex);
          }
        }
      }

      gst_message_unref (msg);

      return GST_BUS_DROP;
    }
    case GST_MESSAGE_NEED_CONTEXT:
      gst_message_parse_context_type (msg, &context_type);

      if (g_strcmp0 (context_type, "gst.va.display.handle") == 0) {
        GstContext *context;
        GstStructure *s;

        gst_printerr ("got need context %s from %s: ", context_type,
            GST_MESSAGE_SRC_NAME (msg));

        g_mutex_lock (&app->mutex);
        if (!app->display) {
          g_mutex_unlock (&app->mutex);
          gst_printerrln ("no gst display yet");
          gst_message_unref (msg);
          return GST_BUS_DROP;
        }

        context = gst_context_new ("gst.va.display.handle", TRUE);
        s = gst_context_writable_structure (context);
        gst_structure_set (s, "gst-display", GST_TYPE_OBJECT, app->display,
            NULL);
        gst_printerrln ("%s", GST_OBJECT_NAME (app->display));
        gst_element_set_context (GST_ELEMENT (GST_MESSAGE_SRC (msg)), context);
        gst_context_unref (context);
        g_mutex_unlock (&app->mutex);

      }

      gst_message_unref (msg);

      return GST_BUS_DROP;

    default:
      break;
  }

  return GST_BUS_PASS;
}

static gboolean
message_handler (GstBus * bus, GstMessage * msg, gpointer data)
{
  struct _app *app = data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      g_main_loop_quit (app->loop);
      break;
    case GST_MESSAGE_ERROR:{
      gchar *debug = NULL;
      GError *err = NULL;

      gst_message_parse_error (msg, &err, &debug);
      gst_printerrln ("GStreamer error: %s\n%s", err->message,
          debug ? debug : "");
      if (debug)
        g_free (debug);
      if (err)
        g_error_free (err);

      g_main_loop_quit (app->loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

static void
config_vpp (GstElement * vpp)
{
  GParamSpec *pspec;
  GObjectClass *g_class = G_OBJECT_GET_CLASS (vpp);
  const static gchar *props[] = { "brightness", "hue", "saturation",
    "contrast"
  };
  gfloat max;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (props); i++) {
    pspec = g_object_class_find_property (g_class, props[i]);
    if (!pspec)
      continue;

    max = ((GParamSpecFloat *) pspec)->maximum;
    g_object_set (vpp, props[i], max, NULL);
  }
}

static gboolean
build_pipeline (struct _app *app)
{
  GstElement *src;
  GstBus *bus;
  GError *err = NULL;
  GString *cmd = g_string_new (NULL);
  const gchar *source = camera ? "v4l2src" : "videotestsrc";

  g_string_printf (cmd, "%s name=src ! tee name=t "
      "t. ! queue ! vapostproc name=vpp ! capsfilter name=caps ! autovideosink "
      "t. ! queue ! vapostproc ! timeoverlay ! autovideosink", source);

  app->pipeline = gst_parse_launch (cmd->str, &err);
  g_string_free (cmd, TRUE);
  if (err) {
    gst_printerrln ("Couldn't create pipeline: %s", err->message);
    g_error_free (err);
    return FALSE;
  }

  if (num_buffers > 0) {
    src = gst_bin_get_by_name (GST_BIN (app->pipeline), "src");
    g_object_set (src, "num-buffers", num_buffers, NULL);
    gst_object_unref (src);
  }

  app->vpp = gst_bin_get_by_name (GST_BIN (app->pipeline), "vpp");
  if (!randomcb)
    config_vpp (app->vpp);

  app->caps = gst_bin_get_by_name (GST_BIN (app->pipeline), "caps");

  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  gst_bus_set_sync_handler (bus, context_handler, app, NULL);
  gst_bus_add_watch (bus, message_handler, app);
  gst_object_unref (bus);

  return TRUE;
}

static gboolean
change_cb_randomly (gpointer data)
{
  struct _app *app = data;
  GstColorBalance *cb;
  GList *channels;

  if (!GST_COLOR_BALANCE_GET_INTERFACE (app->vpp))
    return G_SOURCE_REMOVE;

  cb = GST_COLOR_BALANCE (app->vpp);
  channels = (GList *) gst_color_balance_list_channels (cb);
  for (; channels && channels->data; channels = channels->next) {
    GstColorBalanceChannel *channel = channels->data;
    gint value =
        g_random_int_range (channel->min_value, channel->max_value + 1);

    gst_color_balance_set_value (cb, channel, value);
  }

  return G_SOURCE_CONTINUE;
}

static gboolean
parse_arguments (int *argc, char ***argv)
{
  GOptionContext *ctxt;
  GError *err = NULL;

  ctxt = g_option_context_new ("— Multiple VA postprocessors");
  g_option_context_add_main_entries (ctxt, entries, NULL);
  g_option_context_add_group (ctxt, gst_init_get_option_group ());

  if (!g_option_context_parse (ctxt, argc, argv, &err)) {
    gst_printerrln ("option parsing failed: %s", err->message);
    g_error_free (err);
    return FALSE;
  }

  g_option_context_free (ctxt);
  return TRUE;
}

int
main (int argc, char **argv)
{
  GstBus *bus;
  struct _app app = { NULL, };
  int ret = EXIT_FAILURE;

  if (!parse_arguments (&argc, &argv))
    return EXIT_FAILURE;

  g_mutex_init (&app.mutex);

  app.loop = g_main_loop_new (NULL, TRUE);

  if (!build_pipeline (&app))
    goto gst_failed;

  if (randomcb)
    g_timeout_add_seconds (1, change_cb_randomly, &app);

  gst_element_set_state (app.pipeline, GST_STATE_PLAYING);

  g_main_loop_run (app.loop);

  gst_element_set_state (app.pipeline, GST_STATE_NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (app.pipeline));
  gst_bus_remove_watch (bus);
  gst_object_unref (bus);

  gst_clear_object (&app.display);

  ret = EXIT_SUCCESS;

  gst_clear_object (&app.caps);
  gst_clear_object (&app.vpp);
  gst_clear_object (&app.pipeline);

gst_failed:
  g_mutex_clear (&app.mutex);
  g_main_loop_unref (app.loop);

  gst_deinit ();

  return ret;
}
