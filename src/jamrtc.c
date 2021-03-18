/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

/* Generic includes */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

/* GTK includes */
#include <gtk/gtk.h>

/* GStreamer includes */
#include <gst/gst.h>

/* Local includes */
#include "webrtc.h"
#include "debug.h"


/* Logging */
int jamrtc_log_level = LOG_INFO;
gboolean jamrtc_log_timestamps = FALSE;
gboolean jamrtc_log_colors = TRUE;
int lock_debug = 0;

/* Reference counters */

#ifdef REFCOUNT_DEBUG
int refcount_debug = 1;
GHashTable *counters = NULL;
janus_mutex counters_mutex;
#else
int refcount_debug = 0;
#endif

/* Signal handler */
static volatile gint stop = 0;
static void jamrtc_handle_signal(int signum) {
	JAMRTC_LOG(LOG_INFO, "Stopping JamRTC...\n");
	if(g_atomic_int_compare_and_exchange(&stop, 0, 1)) {
		jamrtc_webrtc_cleanup();
	} else {
		g_atomic_int_inc(&stop);
		if(g_atomic_int_get(&stop) > 2)
			exit(1);
	}
}

/* Signalling/WebRTC callbacks */
static void jamrtc_server_connected(void);
static void jamrtc_server_disconnected(void);
static void jamrtc_joined_room(void);
static void jamrtc_participant_joined(const char *uuid, const char *display);
static void jamrtc_stream_started(const char *uuid, const char *display, const char *instrument,
	gboolean has_audio, gboolean has_video);
static void jamrtc_stream_stopped(const char *uuid, const char *display, const char *instrument);
static void jamrtc_participant_left(const char *uuid, const char *display);
static jamrtc_callbacks callbacks =
	{
		.server_connected = jamrtc_server_connected,
		.server_disconnected = jamrtc_server_disconnected,
		.joined_room = jamrtc_joined_room,
		.participant_joined = jamrtc_participant_joined,
		.stream_started = jamrtc_stream_started,
		.stream_stopped = jamrtc_stream_stopped,
		.participant_left = jamrtc_participant_left,
	};

/* Command line arguments */
static const char *server_url = NULL;
static guint64 room_id = 0;
static const char *display = NULL, *instrument = NULL;
static gboolean no_mic = FALSE, no_webcam = FALSE, no_instrument = FALSE,
	stereo = FALSE, no_jack = FALSE;
static const char *video_device = NULL, *src_opts = NULL;
static guint latency = 0;
static const char *stun_server = NULL, *turn_server = NULL;

static GOptionEntry opt_entries[] = {
	{ "ws", 'w', 0, G_OPTION_ARG_STRING, &server_url, "Address of the Janus WebSockets backend (e.g., ws://localhost:8188; required)", NULL },
	{ "room", 'r', 0, G_OPTION_ARG_INT, &room_id, "Room to join (e.g., 1234; required)", NULL },
	{ "display", 'd', 0, G_OPTION_ARG_STRING, &display, "Display name to use in the room (e.g., Lorenzo; required)", NULL },
	{ "no-mic", 'M', 0, G_OPTION_ARG_NONE, &no_mic, "Don't add an audio source for the local microphone (default: enable audio chat)", NULL },
	{ "no-webcam", 'W', 0, G_OPTION_ARG_NONE, &no_webcam, "Don't add a video source for the local webcam (default: enable video chat)", NULL },
	{ "video-device", 'v', 0, G_OPTION_ARG_STRING, &video_device, "Video device to use for the video chat (default: /dev/video0)", NULL },
	{ "instrument", 'i', 0, G_OPTION_ARG_STRING, &instrument, "Description of the instrument (e.g., Guitar; default: unknown)", NULL },
	{ "stereo", 's', 0, G_OPTION_ARG_NONE, &stereo, "Whether the instrument will be stereo or mono (default: mono)", NULL },
	{ "no-instrument", 'I', 0, G_OPTION_ARG_NONE, &no_instrument, "Don't add a source for the local instrument (default: enable instrument)", NULL },
	{ "jitter-buffer", 'b', 0, G_OPTION_ARG_INT, &latency, "Jitter buffer to use in RTP, in milliseconds (default: 0, no buffering)", NULL },
	{ "src-opts", 'c', 0, G_OPTION_ARG_STRING, &src_opts, "Custom properties to add to jackaudiosrc (local instrument only)", NULL },
	{ "stun-server", 'S', 0, G_OPTION_ARG_STRING, &stun_server, "STUN server to use, if any (hostname:port)", NULL },
	{ "turn-server", 'T', 0, G_OPTION_ARG_STRING, &turn_server, "TURN server to use, if any (username:password@host:port)", NULL },
	{ "log-level", 'l', 0, G_OPTION_ARG_INT, &jamrtc_log_level, "Logging level (0=disable logging, 7=maximum log level; default: 4)", NULL },
	{ "no-jack", 'J', 0, G_OPTION_ARG_NONE, &no_jack, "For testing purposes, use autoaudiosrc/autoaudiosink instead (default: use JACK)", NULL },
	{ NULL },
};


/* Helper method to ensure GStreamer has the modules we need */
static gboolean jamrtc_check_gstreamer_plugins(void) {
	/* Note: maybe some of these should be optional? And OS-aware... */
	const char *needed[] = {
		"jack",
		"opus",
		"vpx",
		"nice",
		"webrtc",
		"dtls",
		"srtp",
		"rtpmanager",
		"video4linux2",
		NULL
	};
	GstRegistry *registry = gst_registry_get();
	if(registry == NULL) {
		JAMRTC_LOG(LOG_FATAL, "No plugins registered in gstreamer\n");
		return FALSE;
	}
	gboolean ret = TRUE;
	int i = 0;
	GstPlugin *plugin = NULL;
	for(i = 0; i < g_strv_length((char **) needed); i++) {
		plugin = gst_registry_find_plugin(registry, needed[i]);
		if(plugin == NULL) {
			JAMRTC_LOG(LOG_FATAL, "Required gstreamer plugin '%s' not found\n", needed[i]);
			ret = FALSE;
			continue;
		}
		gst_object_unref(plugin);
	}
	return ret;
}

/* Thread responsible for the main loop */
static gpointer jamrtc_loop_thread(gpointer user_data) {
	GtkBuilder *builder = (GtkBuilder *)user_data;
	/* Start the main Glib loop */
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	/* Initialize the Janus stack: we'll continue in the 'server_connected' callback */
	if(jamrtc_webrtc_init(&callbacks, builder, loop, server_url, stun_server, turn_server, src_opts, latency, no_jack) < 0) {
		g_main_loop_unref(loop);
		exit(1);
	}
	/* Loop forever */
	g_main_loop_run(loop);

	/* When we leave the loop, we're done */
	g_main_loop_unref(loop);
	gtk_main_quit();
	return NULL;
}

/* This function is called when the main window is closed */
static void jamrtc_window_closed(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
	/* Simulate a SIGINT */
	jamrtc_handle_signal(SIGINT);
}

/* Main application */
int main(int argc, char *argv[]) {

	/* Parse the command-line arguments */
	GError *error = NULL;
	GOptionContext *opts = g_option_context_new("-- Jam sessions with Janus!");
	g_option_context_set_help_enabled(opts, TRUE);
	g_option_context_add_main_entries(opts, opt_entries, NULL);
	if(!g_option_context_parse(opts, &argc, &argv, &error)) {
		g_error_free(error);
		exit(1);
	}
	/* If some arguments are missing, fail */
	if(server_url == NULL || room_id == 0 || display == NULL) {
		char *help = g_option_context_get_help(opts, TRUE, NULL);
		g_print("%s", help);
		g_free(help);
		g_option_context_free(opts);
		exit(1);
	}
	/* Assign some defaults */
	if(video_device == NULL)
		video_device = "/dev/video0";
	if(instrument == NULL)
		instrument = "unknown";
	if(src_opts == NULL)
		src_opts = "";
	if(latency > 1000)
		JAMRTC_LOG(LOG_WARN, "Very high jitter-buffer latency configured (%u)\n", latency);

	/* Logging level: default is info and no timestamps */
	if(jamrtc_log_level == 0)
		jamrtc_log_level = LOG_INFO;
	if(jamrtc_log_level < LOG_NONE)
		jamrtc_log_level = 0;
	else if(jamrtc_log_level > LOG_MAX)
		jamrtc_log_level = LOG_MAX;

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, jamrtc_handle_signal);
	signal(SIGTERM, jamrtc_handle_signal);

	/* Start JamRTC */
	JAMRTC_LOG(LOG_INFO, "\n---------------------------------\n");
	JAMRTC_LOG(LOG_INFO, "JamRTC -- Jam sessions with Janus!\n");
	JAMRTC_LOG(LOG_INFO, "---------------------------------\n\n");
	JAMRTC_LOG(LOG_WARN, "Very alpha version!\n\n");
	JAMRTC_LOG(LOG_INFO, "Janus backend:  %s\n", server_url);
	JAMRTC_LOG(LOG_INFO, "VideoRoom ID:   %"SCNu64"\n", room_id);
	JAMRTC_LOG(LOG_INFO, "Display name:   %s\n", display);
	JAMRTC_LOG(LOG_INFO, "Videochat:      mic %s, webcam %s\n", no_mic ? "disabled" : "enabled", no_webcam ? "disabled" : "enabled");
	if(!no_webcam)
		JAMRTC_LOG(LOG_INFO, "Video device:   %s\n", video_device);
	if(no_instrument)
		JAMRTC_LOG(LOG_INFO, "Instrument:     disabled\n");
	else
		JAMRTC_LOG(LOG_INFO, "Instrument:     %s (%s JACK input)\n", instrument, stereo ? "stereo" : "mono");
	if(strlen(src_opts) > 0)
		JAMRTC_LOG(LOG_INFO, "JACK capture:   %s\n", src_opts);
	JAMRTC_LOG(LOG_INFO, "STUN server:    %s\n", stun_server ? stun_server : "(none)");
	JAMRTC_LOG(LOG_INFO, "TURN server:    %s\n\n", turn_server ? turn_server : "(none)");
	if(no_jack)
		JAMRTC_LOG(LOG_WARN, "For testing purposes, we'll use autoaudiosrc/autoaudiosink, instead of jackaudiosrc/jackaudiosink\n\n");

	/* Initialize GStreamer */
	gst_init(NULL, NULL);
	/* Make sure our gstreamer dependency has all we need */
	if(!jamrtc_check_gstreamer_plugins()) {
		g_option_context_free(opts);
		exit(1);
	}

	/* Initialize GTK */
	gtk_init(NULL, NULL);
	GtkBuilder *builder = gtk_builder_new();
	gtk_builder_add_from_file(builder, "JamRTC.glade", NULL);
	gtk_builder_connect_signals(builder, NULL);
	GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
	g_signal_connect(G_OBJECT(window), "delete-event", G_CALLBACK(jamrtc_window_closed), NULL);
	gtk_widget_show(window);

	/* Spawn a thread to initialize the WebRTC code */
	(void)g_thread_try_new("jamrtc loop", jamrtc_loop_thread, builder, &error);
	if(error != NULL) {
		JAMRTC_LOG(LOG_FATAL, "Got error %d (%s) trying to launch the Jamus loop thread...\n",
			error->code, error->message ? error->message : "??");
		g_option_context_free(opts);
		gst_deinit();
		exit(1);
	}

	/* Show the application */
	gtk_main();

#ifdef REFCOUNT_DEBUG
	/* Any reference counters that are still up while we're leaving? (debug-mode only) */
	jamrtc_mutex_lock(&counters_mutex);
	if(counters && g_hash_table_size(counters) > 0) {
		JAMRTC_PRINT("Debugging reference counters: %d still allocated\n", g_hash_table_size(counters));
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, counters);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			JAMRTC_PRINT("  -- %p\n", value);
		}
	} else {
		JAMRTC_PRINT("Debugging reference counters: 0 still allocated\n");
	}
	jamrtc_mutex_unlock(&counters_mutex);
#endif

	g_option_context_free(opts);
	gst_deinit();
	JAMRTC_LOG(LOG_INFO, "\nBye!\n");
	exit(0);
}

/* We connected to the Janus instance */
static void jamrtc_server_connected(void) {
	/* Now that we're connected, we can join the room */
	jamrtc_join_room(room_id, display);
}

/* We lost the connection to the Janus instance */
static void jamrtc_server_disconnected(void) {
	/* Simulate a SIGINT */
	jamrtc_handle_signal(SIGINT);
}

/* We successfully joined the room */
static void jamrtc_joined_room(void) {
	/* Check if we need to publish our mic/webcam */
	if(!no_mic || !no_webcam)
		jamrtc_webrtc_publish_micwebcam(no_mic, no_webcam, video_device);
	/* Check if we need to publish our local instrument now */
	if(!no_instrument)
		jamrtc_webrtc_publish_instrument(instrument, stereo);
}

/* A new participant just joined the session */
static void jamrtc_participant_joined(const char *uuid, const char *display) {
	JAMRTC_LOG(LOG_INFO, "Participant joined (%s, %s)\n", uuid, display);
}

/* A new stream for a remote participant just became available */
static void jamrtc_stream_started(const char *uuid, const char *display, const char *instrument,
		gboolean has_audio, gboolean has_video) {
	/* TODO Subscribe */
	JAMRTC_LOG(LOG_INFO, "Stream started (%s, %s, %s)\n", uuid, display, instrument);
	jamrtc_webrtc_subscribe(uuid, instrument != NULL);
}

/* An existing stream for a remote participant just went away */
static void jamrtc_stream_stopped(const char *uuid, const char *display, const char *instrument) {
	/* TODO Unsubscribe */
	JAMRTC_LOG(LOG_INFO, "Stream stopped (%s, %s, %s)\n", uuid, display, instrument);
}

/* An existing participant just left the session */
static void jamrtc_participant_left(const char *uuid, const char *display) {
	JAMRTC_LOG(LOG_INFO, "Participant left (%s, %s)\n", uuid, display);
}
