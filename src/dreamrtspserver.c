/*
 * GStreamer dreamrtspserver
 * Copyright 2015 Andreas Frisch <fraxinas@opendreambox.org>
 *
 * This program is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 Unported
 * License. To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-nc-sa/3.0/ or send a letter to
 * Creative Commons,559 Nathan Abbott Way,Stanford,California 94305,USA.
 *
 * Alternatively, this program may be distributed and executed on
 * hardware which is licensed by Dream Property GmbH.
 *
 * This program is NOT free software. It is open source, you are allowed
 * to modify it (if you keep the license), but it may not be commercially
 * distributed other than under the conditions noted above.
 */

#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/rtsp-server/rtsp-server.h>

GST_DEBUG_CATEGORY (dreamrtspserver_debug);
#define GST_CAT_DEFAULT dreamrtspserver_debug

#define DEFAULT_RTSP_PORT 554
#define DEFAULT_RTSP_PATH "/stream"

typedef struct {
	gboolean enabled;
	GstElement *apay, *vpay;
	GstElement *atcpsink, *vtcpsink;
	GstElement *abin, *vbin;
} DreamTCPupstream;

typedef struct {
	gboolean enabled;
	GstRTSPServer *server;
	GstRTSPMountPoints *mounts;
	GstRTSPMediaFactory *factory;
	GstRTSPMedia * media;
	GstElement *aappsrc, *vappsrc;
	GstClockTime rtsp_start_pts, rtsp_start_dts;
	gchar *rtsp_user, *rtsp_pass;
	GList *clients_list;
	gchar *rtsp_port;
	gchar *rtsp_path;
} DreamRTSPserver;

typedef struct {
	GMainLoop *loop;

	GstElement *pipeline, *aappsink, *vappsink;
	GstElement *asrc, *vsrc, *aq, *vq, *aparse, *vparse;
	GstElement *atee, *vtee;

	DreamTCPupstream *tcp_upstream;
	DreamRTSPserver *rtsp_server;
	GMutex rtsp_mutex;
} App;

static const gchar service[] = "com.dreambox.RTSPserver";
static const gchar object_name[] = "/com/dreambox/RTSPserver";
static GDBusNodeInfo *introspection_data = NULL;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='com.dreambox.RTSPserver'>"
  "    <method name='enableSource'>"
  "      <arg type='b' name='state' direction='in'/>"
  "      <arg type='b' name='result' direction='out'/>"
  "    </method>"
  "    <method name='enableRTSP'>"
  "      <arg type='b' name='state' direction='in'/>"
  "      <arg type='s' name='path' direction='in'/>"
  "      <arg type='i' name='port' direction='in'/>"
  "      <arg type='s' name='user' direction='in'/>"
  "      <arg type='s' name='pass' direction='in'/>"
  "      <arg type='b' name='result' direction='out'/>"
  "    </method>"
  "    <method name='enableUpstream'>"
  "      <arg type='b' name='state' direction='in'/>"
  "      <arg type='s' name='host' direction='in'/>"
  "      <arg type='i' name='aport' direction='in'/>"
  "      <arg type='i' name='vport' direction='in'/>"
  "      <arg type='b' name='result' direction='out'/>"
  "    </method>"
  "    <method name='setResolution'>"
  "      <arg type='i' name='width' direction='in'/>"
  "      <arg type='i' name='height' direction='in'/>"
  "    </method>"
  "    <property type='b' name='state' access='read'/>"
  "    <property type='i' name='clients' access='read'/>"
  "    <property type='i' name='audioBitrate' access='readwrite'/>"
  "    <property type='i' name='videoBitrate' access='readwrite'/>"
  "    <property type='i' name='framerate' access='readwrite'/>"
  "    <property type='i' name='width' access='read'/>"
  "    <property type='i' name='height' access='read'/>"
  "    <property type='s' name='path' access='read'/>"
  "  </interface>"
  "</node>";

gboolean create_source_pipeline(App *app);
gboolean enable_rtsp_server(App *app, gchar *path, guint32 port, gchar *user, gchar *pass);
gboolean disable_rtsp_server(App *app);
gboolean enable_tcp_upstream(App *app, gchar *upstream_host, guint32 atcpport, guint32 vtcpport);
gboolean disable_tcp_upstream(App *app);
gboolean destroy_pipeline(App *app);

static gboolean gst_set_framerate(App *app, int value)
{
	GstCaps *caps = NULL;
	GstStructure *structure;
	gboolean ret = FALSE;

	if (!app->pipeline)
		goto out;

	g_object_get (G_OBJECT (app->vsrc), "caps", &caps, NULL);

	if (!GST_IS_CAPS(caps))
		goto out;

	GST_INFO("gst_set_framerate %d old caps %" GST_PTR_FORMAT, value, caps);

	structure = gst_caps_steal_structure (caps, 0);
	if (!structure)
		goto out;

	if (value)
		gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, value, 1, NULL);

	gst_caps_append_structure (caps, structure);
	GST_INFO("new caps %" GST_PTR_FORMAT, caps);
	g_object_set (G_OBJECT (app->vsrc), "caps", caps, NULL);
	ret = TRUE;

out:
	if (caps)
		gst_caps_unref(caps);
	return ret;
}

static gboolean gst_set_resolution(App *app, int width, int height)
{
	GstCaps *caps = NULL;
	GstStructure *structure;
	gboolean ret = FALSE;

	if (!app->pipeline)
		goto out;

	g_object_get (G_OBJECT (app->vsrc), "caps", &caps, NULL);

	if (!GST_IS_CAPS(caps))
		goto out;

	GST_INFO("old caps %" GST_PTR_FORMAT, caps);

	structure = gst_caps_steal_structure (caps, 0);
	if (!structure)
		goto out;

	if (width && height)
	{
		gst_structure_set (structure, "width", G_TYPE_INT, width, NULL);
		gst_structure_set (structure, "height", G_TYPE_INT, height, NULL);
	}
	gst_caps_append_structure (caps, structure);
	GST_INFO("new caps %" GST_PTR_FORMAT, caps);
	g_object_set (G_OBJECT (app->vsrc), "caps", caps, NULL);
	ret = TRUE;

out:
	if (caps)
		gst_caps_unref(caps);
	return ret;
}

static gboolean gst_get_capsprop(App *app, const gchar* element_name, const gchar* prop_name, guint32 *value)
{
	GstElement *element = NULL;
	GstCaps *caps = NULL;
	const GstStructure *structure;
	gboolean ret = FALSE;

	if (!app->pipeline)
		goto out;

	element = gst_bin_get_by_name(GST_BIN(app->pipeline), element_name);
	if (!element)
		goto out;

	g_object_get (G_OBJECT (element), "caps", &caps, NULL);

	if (!GST_IS_CAPS(caps))
		goto out;

	GST_INFO("current caps %" GST_PTR_FORMAT, caps);

	structure = gst_caps_get_structure (caps, 0);
	if (!structure)
		goto out;

	if (g_strcmp0 (prop_name, "framerate") == 0 && value)
	{
		const GValue *framerate = gst_structure_get_value (structure, "framerate");
		if (GST_VALUE_HOLDS_FRACTION(framerate))
			*value = gst_value_get_fraction_numerator (framerate);
		else
			*value = 0;
	}
	else if ((g_strcmp0 (prop_name, "width") == 0 || g_strcmp0 (prop_name, "height") == 0) && value)
	{
		if (!gst_structure_get_int (structure, prop_name, value))
			*value = 0;
	}
	else
		goto out;

	GST_INFO("%s.%s = %i", element_name, prop_name, *value);
	ret = TRUE;
out:
	if (element)
		gst_object_unref(element);
	if (caps)
		gst_caps_unref(caps);
	return ret;
}

static GVariant *handle_get_property (GDBusConnection  *connection,
				      const gchar      *sender,
				      const gchar      *object_path,
				      const gchar      *interface_name,
				      const gchar      *property_name,
				      GError          **error,
				      gpointer          user_data)
{
	App *app = user_data;

	GST_DEBUG("dbus get property %s from %s", property_name, sender);

	if (g_strcmp0 (property_name, "state") == 0)
	{
		GstState state;
		return g_variant_new_boolean ( !!app->pipeline  );
	}
	else if (g_strcmp0 (property_name, "clients") == 0)
	{
		return g_variant_new_int32 (g_list_length(app->rtsp_server->clients_list));
	}
	else if (g_strcmp0 (property_name, "audioBitrate") == 0)
	{
		gint rate = 0;
		if (app->asrc)
			g_object_get (G_OBJECT (app->asrc), "bitrate", &rate, NULL);
		return g_variant_new_int32 (rate);
	}
	else if (g_strcmp0 (property_name, "videoBitrate") == 0)
	{
		gint rate = 0;
		if (app->vsrc)
			g_object_get (G_OBJECT (app->vsrc), "bitrate", &rate, NULL);
		return g_variant_new_int32 (rate);
	}
	else if (g_strcmp0 (property_name, "width") == 0 || g_strcmp0 (property_name, "height") == 0 || g_strcmp0 (property_name, "framerate") == 0)
	{
		guint32 value;
		if (gst_get_capsprop(app, "dreamvideosource0", property_name, &value))
			return g_variant_new_int32(value);
		GST_WARNING("can't handle_get_property name=%s", property_name);
		return g_variant_new_int32(0);
	}
	else if (g_strcmp0 (property_name, "path") == 0)
	{
		return g_variant_new_string (app->rtsp_server->rtsp_path);
	}
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "[RTSPserver] Invalid property '%s'", property_name);
	return NULL;
} // handle_get_property

static gboolean handle_set_property (GDBusConnection  *connection,
				     const gchar      *sender,
				     const gchar      *object_path,
				     const gchar      *interface_name,
				     const gchar      *property_name,
				     GVariant         *value,
				     GError          **error,
				     gpointer          user_data)
{
	App *app = user_data;

	gchar *valstr = g_variant_print (value, TRUE);
	GST_DEBUG("dbus set property %s = %s from %s", property_name, valstr, sender);
	g_free (valstr);

	if (g_strcmp0 (property_name, "audioBitrate") == 0)
	{
		GstElement *source = gst_bin_get_by_name(GST_BIN(app->pipeline), "dreamaudiosource0");
		if (app->asrc)
		{
			g_object_set (G_OBJECT (app->asrc), "bitrate", g_variant_get_int32 (value), NULL);
			return 1;
		}
	}
	else if (g_strcmp0 (property_name, "videoBitrate") == 0)
	{
		if (app->vsrc)
		{
			g_object_set (G_OBJECT (app->vsrc), "bitrate", g_variant_get_int32 (value), NULL);
			return 1;
		}
	}
	else if (g_strcmp0 (property_name, "framerate") == 0)
	{
		if (gst_set_framerate(app, g_variant_get_int32 (value)))
			return 1;
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "[RTSPserver] can't set property '%s' to %d", property_name, g_variant_get_int32 (value));
	}
	else
	{
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "[RTSPserver] Invalid property: '%s'", property_name);
	} // unknown property
	return 0;
} // handle_set_property

static void handle_method_call (GDBusConnection       *connection,
				const gchar           *sender,
				const gchar           *object_path,
				const gchar           *interface_name,
				const gchar           *method_name,
				GVariant              *parameters,
				GDBusMethodInvocation *invocation,
				gpointer               user_data)
{
	App *app = user_data;

	gchar *paramstr = g_variant_print (parameters, TRUE);
	GST_DEBUG("dbus handle method %s %s from %s", method_name, paramstr, sender);
	g_free (paramstr);

	if (g_strcmp0 (method_name, "enableSource") == 0)
	{
		gboolean result;
		gboolean state;
		g_variant_get (parameters, "(b)", &state);
		GST_DEBUG("app->pipeline=%p init state=%i", app->pipeline, state);
		if (state == TRUE)
			result = create_source_pipeline(app);
		else if (state == FALSE)
			result = destroy_pipeline(app);
		GST_DEBUG("result=%i", result);
		g_dbus_method_invocation_return_value (invocation,  g_variant_new ("(b)", result));
	}
	else if (g_strcmp0 (method_name, "enableRTSP") == 0)
	{
		gboolean result = FALSE;
		if (app->pipeline)
		{
			gboolean state;
			guint32 port;
			gchar *path, *user, *pass;

			g_variant_get (parameters, "(bsiss)", &state, &path, &port, &user, &pass);
			GST_DEBUG("app->pipeline=%p, enableRTSP state=%i path=%s port=%i user=%s pass=%s", app->pipeline, state, path, port, user, pass);

			if (state == TRUE)
				result = enable_rtsp_server(app, path, port, user, pass);
			else if (state == FALSE)
				result = disable_rtsp_server(app);
		}
		g_dbus_method_invocation_return_value (invocation,  g_variant_new ("(b)", result));
	}
	else if (g_strcmp0 (method_name, "enableUpstream") == 0)
	{
		gboolean result = FALSE;
		if (app->pipeline)
		{
			gboolean state;
			gchar *upstream_host;
			guint32 atcpport, vtcpport;

			g_variant_get (parameters, "(bsuu)", &state, &upstream_host, &atcpport, &vtcpport);
			GST_DEBUG("app->pipeline=%p, enableUpstream state=%i host=%s audioport=%i videoport=%i", app->pipeline, state, upstream_host, atcpport, vtcpport);

			if (state == TRUE && !app->tcp_upstream->enabled)
				result = enable_tcp_upstream(app, upstream_host, atcpport, vtcpport);
			else if (state == FALSE && app->tcp_upstream->enabled)
				result = disable_tcp_upstream(app);
		}
		g_dbus_method_invocation_return_value (invocation,  g_variant_new ("(b)", result));
	}
	else if (g_strcmp0 (method_name, "setResolution") == 0)
	{
		int width, height;
		g_variant_get (parameters, "(ii)", &width, &height);
		if (gst_set_resolution(app, width, height))
			g_dbus_method_invocation_return_value (invocation, NULL);
		else
			g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "[RTSPserver] can't set resolution %dx%d", width, height);
	}
	// Default: No such method
	else
	{
		g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "[RTSPserver] Invalid method: '%s'", method_name);
	} // if it's an unknown method
} // handle_method_call

static void on_bus_acquired (GDBusConnection *connection,
			     const gchar     *name,
			     gpointer        user_data)
{
	static GDBusInterfaceVTable interface_vtable =
	{
		handle_method_call,
		handle_get_property,
		handle_set_property
	};

	guint registration_id;
	GError *error = NULL;

	GST_DEBUG ("aquired dbus (\"%s\" @ %p)", name, connection);

	registration_id =
	g_dbus_connection_register_object (connection,
					   object_name,
				    introspection_data->interfaces[0],
				    &interface_vtable,
				    user_data,    // Optional user data
				    NULL,    // Func. for freeing user data
				    &error);
} // on_bus_acquired

static void on_name_acquired (GDBusConnection *connection,
			      const gchar     *name,
			      gpointer         user_data)
{
	GST_DEBUG ("aquired dbus name (\"%s\")", name);
} // on_name_acquired

static void on_name_lost (GDBusConnection *connection,
			  const gchar     *name,
			  gpointer         user_data)
{
	App *app = user_data;

	GST_WARNING ("lost dbus name (\"%s\" @ %p)", name, connection);
	//  g_main_loop_quit (app->loop);
} // on_name_lost

static gboolean message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
	App *app = user_data;

	switch (GST_MESSAGE_TYPE (message)) {
		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState old_state, new_state;
			gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
			if (old_state == new_state)
				break;

			if (GST_MESSAGE_SRC(message) == GST_OBJECT(app->pipeline))
			{
				GST_INFO_OBJECT(app, "state transition %s -> %s", gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));

				GstStateChange transition = (GstStateChange)GST_STATE_TRANSITION(old_state, new_state);

				switch(transition)
				{
					case GST_STATE_CHANGE_READY_TO_PAUSED:
					{
						if (GST_MESSAGE_SRC (message) == GST_OBJECT (app->pipeline))
						{
						}
					}	break;
					default:
					{
					}	break;
				}
			}
			break;
		}
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *name, *debug = NULL;
			name = gst_object_get_path_string (message->src);
			gst_message_parse_error (message, &err, &debug);
			g_printerr ("ERROR: from element %s: %s\n", name, err->message);
			if (debug != NULL)
				g_printerr ("Additional debug info:\n%s\n", debug);
			g_error_free (err);
			g_free (debug);
			g_free (name);
			break;
		}
		case GST_MESSAGE_WARNING:
		{
			GError *err = NULL;
			gchar *name, *debug = NULL;
			name = gst_object_get_path_string (message->src);
			gst_message_parse_warning (message, &err, &debug);
			g_printerr ("WARNING: from element %s: %s\n", name, err->message);
			if (debug != NULL)
				g_printerr ("Additional debug info:\n%s\n", debug);
			g_error_free (err);
			g_free (debug);
			g_free (name);
			break;
		}
		case GST_MESSAGE_EOS:
			g_print ("Got EOS\n");
			g_main_loop_quit (app->loop);
			break;
		default:
			break;
	}
	return TRUE;
}

static void media_unprepare (GstRTSPMedia * media, gpointer user_data)
{
	App *app = user_data;
	GstStateChangeReturn ret = gst_element_set_state (app->pipeline, GST_STATE_PAUSED);

	GST_INFO("no more clients -> media unprepared! bringing source pipeline to PAUSED=%i", ret);
	app->rtsp_server->media = NULL;
}

static void client_closed (GstRTSPClient * client, gpointer user_data)
{
	App *app = user_data;
	app->rtsp_server->clients_list = g_list_remove(g_list_first (app->rtsp_server->clients_list), client);
	GST_INFO("client_closed  (number of clients: %i)", g_list_length(app->rtsp_server->clients_list));
}

static void client_connected (GstRTSPServer * server, GstRTSPClient * client, gpointer user_data)
{
	App *app = user_data;
	app->rtsp_server->clients_list = g_list_append(app->rtsp_server->clients_list, client);
	const gchar *ip = gst_rtsp_connection_get_ip (gst_rtsp_client_get_connection (client));
	GST_INFO("client_connected %" GST_PTR_FORMAT " from %s  (number of clients: %i)", client, ip, g_list_length(app->rtsp_server->clients_list));
	g_signal_connect (client, "closed", (GCallback) client_closed, app);
}

static void media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer user_data)
{
	App *app = user_data;
	g_mutex_lock (&app->rtsp_mutex);
	app->rtsp_server->media = media;
	GstElement *element = gst_rtsp_media_get_element (media);
	app->rtsp_server->aappsrc = gst_bin_get_by_name_recurse_up (GST_BIN (element), "aappsrc");
	app->rtsp_server->vappsrc = gst_bin_get_by_name_recurse_up (GST_BIN (element), "vappsrc");
	gst_object_unref(element);
	g_signal_connect (media, "unprepared", (GCallback) media_unprepare, app);
	g_object_set (app->rtsp_server->aappsrc, "format", GST_FORMAT_TIME, NULL);
	g_object_set (app->rtsp_server->vappsrc, "format", GST_FORMAT_TIME, NULL);
	app->rtsp_server->rtsp_start_pts = app->rtsp_server->rtsp_start_dts = GST_CLOCK_TIME_NONE;
	GstStateChangeReturn ret = gst_element_set_state (app->pipeline, GST_STATE_PLAYING);
	GST_INFO("media configured! bringing source pipeline to PLAYING=%i", ret);
	g_mutex_unlock (&app->rtsp_mutex);
}

static GstFlowReturn handover_payload (GstElement * appsink, gpointer user_data)
{
	App *app = user_data;
	DreamRTSPserver *r = app->rtsp_server;

	GstAppSrc* appsrc = NULL;
	if ( appsink == app->vappsink )
		appsrc = GST_APP_SRC(r->vappsrc);
	else if ( appsink == app->aappsink )
		appsrc = GST_APP_SRC(r->aappsrc);

	g_mutex_lock (&app->rtsp_mutex);
	if (appsrc && g_list_length(r->clients_list) > 0) {
		GstSample *sample = gst_app_sink_pull_sample (GST_APP_SINK (appsink));
		GstBuffer *buffer = gst_sample_get_buffer (sample);
		GstCaps *caps = gst_sample_get_caps (sample);

		GST_LOG("original PTS %" GST_TIME_FORMAT " DTS %" GST_TIME_FORMAT " @ %"GST_PTR_FORMAT"", GST_TIME_ARGS (GST_BUFFER_PTS (buffer)), GST_TIME_ARGS (GST_BUFFER_DTS (buffer)), appsrc);
		if (r->rtsp_start_pts == GST_CLOCK_TIME_NONE) {
			if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
			{
				GST_LOG("GST_BUFFER_FLAG_DELTA_UNIT dropping!");
				gst_sample_unref(sample);
				g_mutex_unlock (&app->rtsp_mutex);
				return GST_FLOW_OK;
			}
			else if (appsink == app->vappsink)
			{
				r->rtsp_start_pts = GST_BUFFER_PTS (buffer);
				r->rtsp_start_dts = GST_BUFFER_DTS (buffer);
				GST_INFO("frame is IFRAME! set rtsp_start_pts=%" GST_TIME_FORMAT " rtsp_start_dts=%" GST_TIME_FORMAT " @ %"GST_PTR_FORMAT"", GST_TIME_ARGS (GST_BUFFER_PTS (buffer)), GST_TIME_ARGS (GST_BUFFER_DTS (buffer)), appsrc);
			}
		}
		if (GST_BUFFER_PTS (buffer) < r->rtsp_start_pts)
			GST_BUFFER_PTS (buffer) = 0;
		else
			GST_BUFFER_PTS (buffer) -= r->rtsp_start_pts;
		GST_BUFFER_DTS (buffer) -= r->rtsp_start_dts;
		//    GST_LOG("new PTS %" GST_TIME_FORMAT " DTS %" GST_TIME_FORMAT "", GST_TIME_ARGS (GST_BUFFER_PTS (buffer)), GST_TIME_ARGS (GST_BUFFER_DTS (buffer)));

		GstCaps *oldcaps = gst_app_src_get_caps (appsrc);
		if (!oldcaps || !gst_caps_is_equal (oldcaps, caps))
		{
			GST_LOG("CAPS changed! %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT, oldcaps, caps);
			gst_app_src_set_caps (appsrc, caps);
		}
		gst_app_src_push_buffer (appsrc, gst_buffer_ref(buffer));
		gst_sample_unref (sample);
	}
	else
	{
		if ( gst_debug_category_get_threshold (dreamrtspserver_debug) >= GST_LEVEL_DEBUG)
			GST_LOG("no rtsp clients, discard payload!");
		else
			g_print (".");
	}
	g_mutex_unlock (&app->rtsp_mutex);

	return GST_FLOW_OK;
}

gboolean create_source_pipeline(App *app)
{
	GST_INFO_OBJECT(app, "create_source_pipeline");

	app->pipeline = gst_pipeline_new (NULL);
	app->asrc = gst_element_factory_make ("dreamaudiosource", "dreamaudiosource0");

	app->vsrc = gst_element_factory_make ("dreamvideosource", "dreamvideosource0");

	app->aparse = gst_element_factory_make ("aacparse", NULL);
	app->vparse = gst_element_factory_make ("h264parse", NULL);

	app->aq = gst_element_factory_make ("queue", NULL);
	app->vq = gst_element_factory_make ("queue", NULL);

	app->aappsink = gst_element_factory_make ("appsink", "aappsink");
	app->vappsink = gst_element_factory_make ("appsink", "vappsink");

	app->atee = gst_element_factory_make ("tee", NULL);
	app->vtee = gst_element_factory_make ("tee", NULL);

	if (!(app->asrc && app->vsrc && app->aparse && app->vparse && app->aq && app->vq && app->aappsink && app->vappsink && app->atee && app->vtee))
	{
		g_error ("Failed to create source pipeline element(s):%s%s%s%s%s%s%s%s%s%s", app->asrc?"":" dreamaudiosource", app->vsrc?"":" dreamvideosource", app->aparse?"":" aacparse", app->vparse?"":" h264parse",
			app->aq?"":" aqueue", app->vq?"":" vqueue", app->aappsink?"":" aappsink", app->vappsink?"":" vappsink", app->atee?"":" atee", app->vtee?"":" vtee");
	}

	gst_bin_add_many (GST_BIN (app->pipeline), app->asrc, app->vsrc, app->aparse, app->vparse, app->aq, app->vq, app->atee, app->vtee, app->aappsink, app->vappsink, NULL);
	gst_element_link_many (app->asrc, app->aparse, app->aq, app->atee, NULL);
	gst_element_link_many (app->vsrc, app->vparse, app->vq, app->vtee, NULL);

	GstPad *teepad, *sinkpad;
	teepad = gst_element_get_request_pad (app->atee, "src_%u");
	sinkpad = gst_element_get_static_pad (app->aappsink, "sink");
	gst_pad_link (teepad, sinkpad);
	gst_object_unref (teepad);
	gst_object_unref (sinkpad);
	teepad = gst_element_get_request_pad (app->vtee, "src_%u");
	sinkpad = gst_element_get_static_pad (app->vappsink, "sink");
	gst_pad_link (teepad, sinkpad);
	gst_object_unref (teepad);
	gst_object_unref (sinkpad);

	g_object_set (G_OBJECT (app->aq), "leaky", 2, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", G_GINT64_CONSTANT(5)*GST_SECOND, NULL);
	g_object_set (G_OBJECT (app->vq), "leaky", 2, "max-size-buffers", 0, "max-size-bytes", 0, "max-size-time", G_GINT64_CONSTANT(5)*GST_SECOND, NULL);

	g_object_set (G_OBJECT (app->aappsink), "emit-signals", TRUE, NULL);
	g_object_set (G_OBJECT (app->aappsink), "enable-last-sample", FALSE, NULL);
	//  g_object_set (G_OBJECT (app->aappsink), "sync", FALSE, NULL);
	g_signal_connect (app->aappsink, "new-sample", G_CALLBACK (handover_payload), app);

	g_object_set (G_OBJECT (app->vappsink), "emit-signals", TRUE, NULL);
	g_object_set (G_OBJECT (app->vappsink), "enable-last-sample", FALSE, NULL);
	//  g_object_set (G_OBJECT (app->vappsink), "sync", FALSE, NULL);
	g_signal_connect (app->vappsink, "new-sample", G_CALLBACK (handover_payload), app);

	GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
	gst_bus_add_signal_watch (bus);
	g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), app);
	gst_object_unref (GST_OBJECT (bus));

	return gst_element_set_state (app->pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE;
}

DreamTCPupstream *create_tcp_upstream(App *app)
{
	GST_INFO_OBJECT(app, "create_tcp_upstream");

	DreamTCPupstream *t = malloc(sizeof(DreamTCPupstream));

	t->abin = gst_element_factory_make ("bin", "abin");
	t->vbin = gst_element_factory_make ("bin", "abin");

	t->apay = gst_element_factory_make ("gdppay", "agdppay");
	t->vpay = gst_element_factory_make ("gdppay", "vgdppay");

	t->atcpsink = gst_element_factory_make ("tcpclientsink", "atcpclientsink");
	t->vtcpsink = gst_element_factory_make ("tcpclientsink", "vtcpclientsink");

	if (!(t->abin && t->vbin && t->apay && t->vpay && t->atcpsink && t->vtcpsink))
		g_error ("Failed to create tcp upstream element(s):%s%s%s%s%s%s", t->abin?"":"  audio bin", t->vpay?"":"  video bin", t->abin?"":"  audio gdppay", t->vpay?"":"  video gdppay", t->atcpsink?"":"  audio tcpclientsink", t->vtcpsink?"":"  video tcpclientsink" );

	gst_bin_add_many (GST_BIN(t->abin), t->apay, t->atcpsink, NULL);
	gst_bin_add_many (GST_BIN(t->vbin), t->vpay, t->vtcpsink, NULL);
	if (!gst_element_link_many (t->apay, t->atcpsink, NULL)) {
		g_error ("Failed to link tcp upstreambin audio elements");
	}
	if (!gst_element_link_many (t->vpay, t->vtcpsink, NULL)) {
		g_error ("Failed to link tcp upstreambin video elements");
	}

	GstPad *asinkpad, *vsinkpad, *paypad;
	paypad = gst_element_get_static_pad (t->apay, "sink");
	asinkpad = gst_ghost_pad_new ("sink", paypad);
	gst_pad_set_active (asinkpad, TRUE);
	gst_element_add_pad (t->abin, asinkpad);

	gst_object_unref (paypad);
	paypad = gst_element_get_static_pad (t->vpay, "sink");
	vsinkpad = gst_ghost_pad_new ("sink", paypad);
	gst_pad_set_active (vsinkpad, TRUE);
	gst_element_add_pad (t->vbin, vsinkpad);

	t->enabled = FALSE;

	return t;
}

DreamRTSPserver *create_rtsp_server(App *app)
{
	GST_INFO_OBJECT(app, "create_rtsp_server");

	GstElement *appsrc, *vpay, *apay, *udpsrc;
	appsrc = gst_element_factory_make ("appsrc", NULL);
	vpay = gst_element_factory_make ("rtph264pay", NULL);
	apay = gst_element_factory_make ("rtpmp4apay", NULL);
	udpsrc = gst_element_factory_make ("udpsrc", NULL);

	if (!(appsrc && vpay && apay && udpsrc))
		g_error ("Failed to create rtsp element(s):%s%s%s%s", appsrc?"":" appsrc", vpay?"": "rtph264pay", apay?"":" rtpmp4apay", udpsrc?"":" udpsrc" );
	else
	{
		gst_object_unref (appsrc);
		gst_object_unref (vpay);
		gst_object_unref (apay);
		gst_object_unref (udpsrc);
	}

	DreamRTSPserver *r = malloc(sizeof(DreamRTSPserver));

	r->server = gst_rtsp_server_new ();
	g_signal_connect (r->server, "client-connected", (GCallback) client_connected, app);

	r->factory = gst_rtsp_media_factory_new ();
	gst_rtsp_media_factory_set_launch (r->factory, "( appsrc name=vappsrc ! h264parse ! rtph264pay name=pay0 pt=96   appsrc name=aappsrc ! aacparse ! rtpmp4apay name=pay1 pt=97 )");
	gst_rtsp_media_factory_set_shared (r->factory, TRUE);

	g_signal_connect (r->factory, "media-configure", (GCallback) media_configure, app);

	r->enabled = FALSE;

	return r;
}

gboolean enable_tcp_upstream(App *app, gchar *upstream_host, guint32 atcpport, guint32 vtcpport)
{
	GST_DEBUG_OBJECT(app, "enable_tcp_upstream host=%s audioport=%i videoport=%i", upstream_host, atcpport, vtcpport);

	DreamTCPupstream *t = app->tcp_upstream;

	if (!t->enabled)
	{
		GstPadLinkReturn ret;
		GstPad *teepad, *sinkpad;
		teepad = gst_element_get_request_pad (app->atee, "src_%u");
		sinkpad = gst_element_get_static_pad (t->abin, "sink");
		ret = gst_pad_link (teepad, sinkpad);
		gst_object_unref (teepad);
		gst_object_unref (sinkpad);
		if (ret != GST_PAD_LINK_OK)
			goto fail;
		teepad = gst_element_get_request_pad (app->vtee, "src_%u");
		sinkpad = gst_element_get_static_pad (t->vbin, "sink");
		gst_pad_link (teepad, sinkpad);
		if (ret != GST_PAD_LINK_OK)
			goto fail;
		gst_object_unref (teepad);
		gst_object_unref (sinkpad);

		g_object_set (t->atcpsink, "host", upstream_host, NULL);
		g_object_set (t->vtcpsink, "host", upstream_host, NULL);
		g_object_set (t->atcpsink, "port", atcpport, NULL);
		g_object_set (t->vtcpsink, "port", vtcpport, NULL);

		t->enabled = TRUE;
		return TRUE;
	}
	return FALSE;

fail:
	GST_ERROR_OBJECT (app, "failed to enable tcp upstream!");
	return FALSE;
}

gboolean enable_rtsp_server(App *app, gchar *path, guint32 port, gchar *user, gchar *pass)
{
	GST_INFO_OBJECT(app, "enable_rtsp_server path=%s port=%i user=%s pass=%s", path, port, user, pass);
	DreamRTSPserver *r = app->rtsp_server;
	if (!r->enabled)
	{
		gchar *credentials = "";
		if (strlen(user)) {
			r->rtsp_user = user;
			r->rtsp_pass = pass;
			GstRTSPToken *token;
			gchar *basic;
			GstRTSPAuth *auth = gst_rtsp_auth_new ();
			gst_rtsp_media_factory_add_role (r->factory, "user", GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, G_TYPE_BOOLEAN, TRUE, GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE, NULL);
			token = gst_rtsp_token_new (GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "user", NULL);
			basic = gst_rtsp_auth_make_basic (r->rtsp_user, r->rtsp_pass);
			gst_rtsp_server_set_auth (r->server, auth);
			gst_rtsp_auth_add_basic (auth, basic, token);
			g_free (basic);
			gst_rtsp_token_unref (token);
			credentials = g_strdup_printf("%s:%s@", user, pass);
		}

		r->rtsp_port = g_strdup_printf("%i", port ? port : DEFAULT_RTSP_PORT);

		gst_rtsp_server_set_service (r->server, r->rtsp_port);

		if (strlen(path))
		{
			r->rtsp_path = g_strdup_printf ("%s%s", path[0]=='/' ? "" : "/", path);
			g_free(path);
		}
		else
			r->rtsp_path = g_strdup(DEFAULT_RTSP_PATH);

		r->mounts = gst_rtsp_server_get_mount_points (r->server);
		gst_rtsp_mount_points_add_factory (r->mounts, r->rtsp_path, g_object_ref(r->factory));
		g_object_unref (r->mounts);
		r->clients_list = NULL;
		r->media = NULL;
		r->enabled = TRUE;
		gst_rtsp_server_attach (r->server, NULL);
		g_print ("dreambox encoder stream ready at rtsp://%s127.0.0.1:%s%s\n", credentials, app->rtsp_server->rtsp_port, app->rtsp_server->rtsp_path);
		return TRUE;
	}
	return FALSE;
}

GstRTSPFilterResult *client_filter_func (GstRTSPServer *server, GstRTSPClient *client, gpointer user_data)
{
	App *app = user_data;
	GST_INFO("client_filter_func %" GST_PTR_FORMAT "  (number of clients: %i)", client, g_list_length(app->rtsp_server->clients_list));
	return GST_RTSP_FILTER_REMOVE;
}

gboolean disable_rtsp_server(App *app)
{
	GST_INFO("disable_rtsp_server");
	if (app->rtsp_server->enabled)
	{
		if (app->rtsp_server->media)
		{
			GList *filter = gst_rtsp_server_client_filter(app->rtsp_server->server, (GstRTSPServerClientFilterFunc) client_filter_func, app);
		}
		gst_rtsp_mount_points_remove_factory (app->rtsp_server->mounts, app->rtsp_server->rtsp_path);
		app->rtsp_server->enabled = FALSE;
		return TRUE;
	}
	return FALSE;
}

gboolean disable_tcp_upstream(App *app)
{
	GST_INFO("disable_tcp_upstream");
	DreamTCPupstream *t = app->tcp_upstream;
	if (t->enabled)
	{
		GstPad *teepad, *sinkpad;
		teepad = gst_element_get_request_pad (app->atee, "src_%u");
		sinkpad = gst_element_get_static_pad (t->abin, "sink");
		gst_pad_unlink (teepad, sinkpad);
		gst_object_unref (teepad);
		gst_object_unref (sinkpad);
		teepad = gst_element_get_request_pad (app->vtee, "src_%u");
		sinkpad = gst_element_get_static_pad (t->vbin, "sink");
		gst_pad_unlink (teepad, sinkpad);
		gst_object_unref (teepad);
		gst_object_unref (sinkpad);
		t->enabled = FALSE;
		return TRUE;
	}
	return FALSE;
}

gboolean destroy_pipeline(App *app)
{
	GST_INFO_OBJECT(app, "destroy_pipeline @%p", app->pipeline);
	if (app->pipeline)
	{
		GST_INFO_OBJECT(app, "really do destroy_pipeline");
		gst_element_set_state (app->pipeline, GST_STATE_NULL);
		gst_object_unref (app->pipeline);
		app->pipeline = NULL;
		return TRUE;
	}
	else
		GST_INFO_OBJECT(app, "don't destroy_pipeline");
	return FALSE;
}

int main (int argc, char *argv[])
{
	App app;
	guint owner_id;

	memset (&app, 0, sizeof(app));

	app.pipeline = NULL;

	introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

	owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
				   service,
			    G_BUS_NAME_OWNER_FLAGS_NONE,
			    on_bus_acquired,
			    on_name_acquired,
			    on_name_lost,
			    &app,
			    NULL);

	gst_init (0, NULL);

	GST_DEBUG_CATEGORY_INIT (dreamrtspserver_debug, "dreamrtspserver",
			GST_DEBUG_BOLD | GST_DEBUG_FG_YELLOW | GST_DEBUG_BG_BLUE,
			"Dreambox RTSP server daemon");

	app.tcp_upstream = create_tcp_upstream(&app);
	app.rtsp_server = create_rtsp_server(&app);

	g_mutex_init (&app.rtsp_mutex);

	app.loop = g_main_loop_new (NULL, FALSE);

	g_main_loop_run (app.loop);

	destroy_pipeline(&app);

	g_main_loop_unref (app.loop);

	g_mutex_clear (&app.rtsp_mutex);

	g_list_free (app.rtsp_server->clients_list);

	free(app.tcp_upstream);
	free(app.rtsp_server);

	g_bus_unown_name (owner_id);
	g_dbus_node_info_unref (introspection_data);
}
