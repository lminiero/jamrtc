/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

/* GTK/GDK includes */
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

/* GStreamer includes */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/video/videooverlay.h>

/* WebSockets/JSON stack(Janus API) */
#include <libwebsockets.h>
#include <json-glib/json-glib.h>

/* Local includes */
#include "webrtc.h"
#include "mutex.h"
#include "refcount.h"
#include "debug.h"


/* Janus signalling state management */
typedef enum jamrtc_state {
	JAMRTC_JANUS_DISCONNECTED = 0,
	JAMRTC_JANUS_CONNECTING = 1,
	JAMRTC_JANUS_CONNECTION_ERROR,
	JAMRTC_JANUS_CONNECTED,
	JAMRTC_JANUS_CREATING_SESSION,
	JAMRTC_JANUS_SESSION_CREATED,
	JAMRTC_JANUS_ATTACHING_PLUGIN,
	JAMRTC_JANUS_HANDLE_ATTACHED,
	JAMRTC_JANUS_SDP_PREPARED,
	JAMRTC_JANUS_STARTED,
	JAMRTC_JANUS_API_ERROR,
	JAMRTC_JANUS_STATE_ERROR
} jamrtc_state;

/* Callbacks handler (API) */
static const jamrtc_callbacks *cb = NULL;

/* Global properties */
static GtkBuilder *builder = NULL;
static GMainLoop *loop = NULL;
static const char *stun_server = NULL, *turn_server = NULL,
	*src_opts = NULL, *video_device = NULL;
static gboolean no_mic = FALSE,  no_webcam = FALSE, stereo = FALSE, no_jack = FALSE;
static guint latency = 0;

/* WebSocket properties */
static const char *server_url = NULL;
static const char *protocol = NULL, *address = NULL, *path = NULL;
static int port = 0;
static jamrtc_state state = 0;
static GSource *keep_alives = NULL;
static struct lws_context *context = NULL;
static struct lws *wsi = NULL;
static volatile gint stopping = 0;

typedef struct jamrtc_ws_client {
	struct lws *wsi;		/* The libwebsockets client instance */
	char *incoming;			/* Buffer containing incoming data until it's complete */
	unsigned char *buffer;	/* Buffer containing the message to send */
	int buflen;				/* Length of the buffer (may be resized after re-allocations) */
	int bufpending;			/* Data an interrupted previous write couldn't send */
	int bufoffset;			/* Offset from where the interrupted previous write should resume */
	jamrtc_mutex mutex;		/* Mutex to lock/unlock this instance */
} jamrtc_ws_client;
static jamrtc_ws_client *ws_client = NULL;
static GAsyncQueue *messages = NULL;	/* Queue of outgoing messages to push */
static jamrtc_mutex writable_mutex;
static GThread *ws_thread = NULL;

static int jamrtc_ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static struct lws_protocols protocols[] = {
	{ "janus-protocol", jamrtc_ws_callback, sizeof(jamrtc_ws_client), 0 },
	{ NULL, NULL, 0, 0 }
};
static const struct lws_extension exts[] = {
#ifndef LWS_WITHOUT_EXTENSIONS
	{ "permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate; client_max_window_bits" },
	{ "deflate-frame", lws_extension_callback_pm_deflate, "deflate_frame" },
#endif
	{ NULL, NULL, NULL }
};

/* Janus API properties */
static guint64 session_id = 0, room_id = 0, private_id = 0;
static char *local_uuid = NULL;
static const char *display_name = NULL;
/* Janus API handles management */
static guint64 *jamrtc_uint64_dup(guint64 num) {
	guint64 *numdup = g_malloc(sizeof(guint64));
	*numdup = num;
	return numdup;
}


/* JamRTC PeerConnection and related properties */
typedef struct jamrtc_webrtc_pc {
	/* Whether it's a remote stream, or a local feed */
	gboolean remote;
	/* Slot assigned to this stream */
	guint slot;
	/* Janus handle ID */
	guint64 handle_id;
	/* Janus VideoRoom user ID */
	guint64 user_id;
	/* UUID of who's sending this stream */
	char *uuid;
	/* Display name of who's sending this stream */
	char *display;
	/* Instrument played in this stream, if any (it may be for the audio/video chat) */
	char *instrument;
	/* Signalling state */
	jamrtc_state state;
	/* GStreamer pipeline in use */
	GstElement *pipeline;
	/* Pointer to the peerConnection object in the pipeline */
	GstElement *peerconnection;
	/* Whether there's audio and/or video */
	gboolean audio, video;
	/*! Atomic flag to check if this instance has been destroyed */
	volatile gint destroyed;
	/* Reference count */
	jamrtc_refcount ref;
} jamrtc_webrtc_pc;
static void jamrtc_webrtc_pc_free(const jamrtc_refcount *pc_ref) {
	jamrtc_webrtc_pc *pc = jamrtc_refcount_containerof(pc_ref, jamrtc_webrtc_pc, ref);
	/* This instance can be destroyed, free all the resources */
	g_free(pc->uuid);
	g_free(pc->display);
	g_free(pc->instrument);
	if(pc->pipeline)
		gst_object_unref(pc->pipeline);
	g_free(pc);
}
static void jamrtc_webrtc_pc_destroy(jamrtc_webrtc_pc *pc) {
	if(pc == NULL)
		return;
	if(!g_atomic_int_compare_and_exchange(&pc->destroyed, 0, 1))
		return;
	/* Send a detach to Janus */
		/* TODO */
	/* Quit the PeerConnection loop */
	if(pc->pipeline)
		gst_element_set_state(GST_ELEMENT(pc->pipeline), GST_STATE_NULL);
	/* The PeerConnection will actually be destroyed when the counter gets to 0 */
	jamrtc_refcount_decrease(&pc->ref);
}
static void jamrtc_webrtc_pc_unref(jamrtc_webrtc_pc *pc) {
	if(pc == NULL)
		return;
	jamrtc_refcount_decrease(&pc->ref);
}
static jamrtc_webrtc_pc *jamrtc_webrtc_pc_new(const char *uuid, const char *display, gboolean remote, const char *instrument) {
	if(uuid == NULL || display == NULL)
		return NULL;
	jamrtc_webrtc_pc *pc = g_malloc0(sizeof(jamrtc_webrtc_pc));
	pc->remote = remote;
	pc->uuid = g_strdup(uuid);
	pc->display = g_strdup(display);
	if(instrument != NULL)
		pc->instrument = g_strdup(instrument);
	pc->state = JAMRTC_JANUS_SESSION_CREATED;
	jamrtc_refcount_init(&pc->ref, jamrtc_webrtc_pc_free);
	/* Done */
	return pc;
}
/* Media we may be publishing ourselves */
static jamrtc_webrtc_pc *local_micwebcam = NULL;
static jamrtc_webrtc_pc *local_instrument = NULL;

/* JamRTC participants */
typedef struct jamrtc_webrtc_participant {
	/* Janus VideoRoom user IDs */
	guint64 user_id, instrument_user_id;
	/* Unique UUID as advertised via signalling */
	char *uuid;
	/* Display name of the participant */
	char *display;
	/* Slot assigned to this participant */
	guint slot;
	/* Mic/webcam PeerConnection of this participant, if any */
	jamrtc_webrtc_pc *micwebcam;
	/* Instrument PeerConnection of this participant, if any */
	jamrtc_webrtc_pc *instrument;
	/*! Atomic flag to check if this instance has been destroyed */
	volatile gint destroyed;
	/* Reference count */
	jamrtc_refcount ref;
} jamrtc_webrtc_participant;
static void jamrtc_webrtc_participant_free(const jamrtc_refcount *participant_ref) {
	jamrtc_webrtc_participant *participant = jamrtc_refcount_containerof(participant_ref, jamrtc_webrtc_participant, ref);
	if(participant == NULL)
		return;
	g_free(participant->uuid);
	g_free(participant->display);
	g_free(participant);
}
static void jamrtc_webrtc_participant_destroy(jamrtc_webrtc_participant *participant) {
	if(participant == NULL)
		return;
	if(!g_atomic_int_compare_and_exchange(&participant->destroyed, 0, 1))
		return;
	/* The PeerConnection will actually be destroyed when the counter gets to 0 */
	jamrtc_refcount_decrease(&participant->ref);
}
static void jamrtc_webrtc_participant_unref(jamrtc_webrtc_participant *participant) {
	if(participant == NULL)
		return;
	jamrtc_refcount_decrease(&participant->ref);
}
/* Remote participants and PeerConnections */
static GHashTable *participants = NULL;
static GHashTable *participants_byid = NULL;
static GHashTable *participants_byslot = NULL;
static GHashTable *peerconnections = NULL;
static jamrtc_mutex participants_mutex;

/* Signalling methods and callbacks */
static void jamrtc_connect_websockets(void);
static void jamrtc_send_message(char *text);
static gboolean jamrtc_create_session(void);
static gboolean jamrtc_attach_handle(jamrtc_webrtc_pc *pc);
static gboolean jamrtc_prepare_pipeline(jamrtc_webrtc_pc *pc, gboolean subscription, gboolean do_audio, gboolean do_video);
static void jamrtc_negotiation_needed(GstElement *element, gpointer user_data);
static void jamrtc_sdp_available(GstPromise *promise, gpointer user_data);
static void jamrtc_trickle_candidate(GstElement *webrtc,
	guint mlineindex, char *candidate, gpointer user_data);
static void jamrtc_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data);
static void jamrtc_server_message(char *text);
/* Transactions management */
static GHashTable *transactions = NULL;
static jamrtc_mutex transactions_mutex;


/* Video rendering callbacks */
typedef enum janus_video_message_action {
	JAMRTC_ACTION_NONE = 0,
	JAMRTC_ACTION_ADD_PARTICIPANT,
	JAMRTC_ACTION_ADD_STREAM,
	JAMRTC_ACTION_REMOVE_STREAM,
	JAMRTC_ACTION_REMOVE_PARTICIPANT
} janus_video_message_action;
typedef struct jamrtc_video_message {
	janus_video_message_action action;
	gpointer resource;
	gboolean video;
	char *sink;
} jamrtc_video_message;
static jamrtc_video_message *jamrtc_video_message_create(janus_video_message_action action,
		gpointer resource, gboolean video, const char *sink) {
	jamrtc_video_message *msg = g_malloc(sizeof(jamrtc_video_message));
	msg->action = action;
	msg->resource = resource;
	switch(action) {
		case JAMRTC_ACTION_ADD_PARTICIPANT:
		case JAMRTC_ACTION_REMOVE_PARTICIPANT: {
			jamrtc_webrtc_participant *participant = (jamrtc_webrtc_participant *)resource;
			if(participant != NULL)
				jamrtc_refcount_increase(&participant->ref);
			break;
		}
		case JAMRTC_ACTION_ADD_STREAM:
		case JAMRTC_ACTION_REMOVE_STREAM: {
			jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)resource;
			if(pc != NULL)
				jamrtc_refcount_increase(&pc->ref);
			break;
		}
		default: {
			g_free(msg);
			return NULL;
		}
	}
	msg->video = video;
	msg->sink = g_strdup(sink);
	return msg;
}
static void jamrtc_video_message_free(jamrtc_video_message *msg) {
	if(msg == NULL)
		return;
	g_free(msg->sink);
	switch(msg->action) {
		case JAMRTC_ACTION_ADD_PARTICIPANT:
		case JAMRTC_ACTION_REMOVE_PARTICIPANT: {
			jamrtc_webrtc_participant *participant = (jamrtc_webrtc_participant *)msg->resource;
			if(participant != NULL)
				jamrtc_refcount_decrease(&participant->ref);
			break;
		}
		case JAMRTC_ACTION_ADD_STREAM:
		case JAMRTC_ACTION_REMOVE_STREAM: {
			jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)msg->resource;
			if(pc != NULL)
				jamrtc_refcount_decrease(&pc->ref);
			break;
		}
		default:
			break;
	}
	g_free(msg);
}
static gboolean jamrtc_video_message_handle(gpointer user_data) {
	jamrtc_video_message *msg = (jamrtc_video_message *)user_data;
	if(msg == NULL)
		return G_SOURCE_REMOVE;
	if(msg->action == JAMRTC_ACTION_ADD_PARTICIPANT) {
		/* Initialize the labels in this slot */
		jamrtc_webrtc_participant *participant = (jamrtc_webrtc_participant *)msg->resource;
		guint slot = participant ? participant->slot : 1;
		char display_label[100];
		g_snprintf(display_label, sizeof(display_label), "user%u_name", slot);
		GtkLabel *display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
		gtk_label_set_text(display, participant ? participant->display : local_micwebcam->display);
		g_snprintf(display_label, sizeof(display_label), "user%u_mic", slot);
		display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
		gtk_label_set_text(display, "No microphone (chat)");
		g_snprintf(display_label, sizeof(display_label), "user%u_instrument", slot);
		display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
		gtk_label_set_text(display, "No instrument");
	} else if(msg->action == JAMRTC_ACTION_ADD_STREAM) {
		/* We have a new stream to render, update the related label too */
		jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)msg->resource;
		if(!msg->video) {
			/* Update the mic/instrument label */
			char audio_label[100];
			g_snprintf(audio_label, sizeof(audio_label), "user%u_%s", pc->slot,
				pc->instrument ? "instrument" : "mic");
			GtkLabel *label = GTK_LABEL(gtk_builder_get_object(builder, audio_label));
			gtk_label_set_text(label, pc->instrument ? pc->instrument : "Microphone (chat)");
			/* Render the wavescope associated with the audio stream */
			char draw[100];
			g_snprintf(draw, sizeof(draw), "user%u_%sdraw", pc->slot,
				pc->instrument ? "instrument" : "mic");
			GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
			gtk_widget_set_size_request(widget, 320, 100);
			GdkWindow *window = gtk_widget_get_window(widget);
			gulong xid = GDK_WINDOW_XID(window);
			GstElement *sink = gst_bin_get_by_name(GST_BIN(pc->pipeline), msg->sink);
			gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), xid);
		} else {
			/* Render this video stream */
			char draw[100];
			g_snprintf(draw, sizeof(draw), "user%u_videodraw", pc->slot);
			GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
			gtk_widget_set_size_request(widget, 320, 180);
			GdkWindow *window = gtk_widget_get_window(widget);
			gulong xid = GDK_WINDOW_XID(window);
			GstElement *sink = gst_bin_get_by_name(GST_BIN(pc->pipeline), msg->sink);
			gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), xid);
		}
	} else if(msg->action == JAMRTC_ACTION_REMOVE_STREAM) {
		/* A stream we were rendering has gone away, update the related label too */
		jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)msg->resource;
		if(pc->slot < 1 || pc->slot > 4) {
			JAMRTC_LOG(LOG_WARN, "Invalid stream slot %d, ignoring\n", pc->slot);
		} else {
			if(pc->audio) {
				/* Update the mic/instrument label */
				char audio_label[100];
				g_snprintf(audio_label, sizeof(audio_label), "user%u_%s", pc->slot,
					pc->instrument ? "instrument" : "mic");
				GtkLabel *label = GTK_LABEL(gtk_builder_get_object(builder, audio_label));
				gtk_label_set_text(label, pc->instrument ? "No instrument" : "No microphone (chat)");
				/* Empty the draw element */
				char draw[100];
				g_snprintf(draw, sizeof(draw), "user%u_%sdraw", pc->slot,
					pc->instrument ? "instrument" : "mic");
				GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
				gtk_widget_set_size_request(widget, 0, 0);
			}
			if(pc->video) {
				/* Empty the video draw element */
				char draw[100];
				g_snprintf(draw, sizeof(draw), "user%u_videodraw", pc->slot);
				GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
				gtk_widget_set_size_request(widget, 0, 0);
			}
		}
	} else if(msg->action == JAMRTC_ACTION_REMOVE_PARTICIPANT) {
		/* Reset the labels in this slot */
		jamrtc_webrtc_participant *participant = (jamrtc_webrtc_participant *)msg->resource;
		if(participant && (participant->slot < 1 || participant->slot > 4)) {
			JAMRTC_LOG(LOG_WARN, "Invalid participant slot %d for %s, ignoring\n", participant->slot, participant->display);
		} else {
			guint slot = participant ? participant->slot : 1;
			char display_label[100];
			g_snprintf(display_label, sizeof(display_label), "user%u_name", slot);
			GtkLabel *display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
			gtk_label_set_text(display, "");
			g_snprintf(display_label, sizeof(display_label), "user%u_mic", slot);
			display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
			gtk_label_set_text(display, "");
			g_snprintf(display_label, sizeof(display_label), "user%u_instrument", slot);
			display = GTK_LABEL(gtk_builder_get_object(builder, display_label));
			gtk_label_set_text(display, "");
			/* Get rid of the draw elements too, if any */
			char draw[100];
			g_snprintf(draw, sizeof(draw), "user%u_videodraw", slot);
			GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
			gtk_widget_set_size_request(widget, 0, 0);
			g_snprintf(draw, sizeof(draw), "user%u_micdraw", slot);
			widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
			gtk_widget_set_size_request(widget, 0, 0);
			g_snprintf(draw, sizeof(draw), "user%u_instrumentdraw", slot);
			widget = GTK_WIDGET(gtk_builder_get_object(builder, draw));
			gtk_widget_set_size_request(widget, 0, 0);
		}
	}
	jamrtc_video_message_free(msg);
	return G_SOURCE_REMOVE;
}

/* Janus stack initialization */
int jamrtc_webrtc_init(const jamrtc_callbacks* callbacks, GtkBuilder *gtkbuilder, GMainLoop *mainloop,
		const char *ws, const char *stun, const char *turn, const char *src, guint jitter, gboolean disable_jack) {
	/* Validate the input */
	if(lws_parse_uri((char *)ws, &protocol, &address, &port, &path)) {
		JAMRTC_LOG(LOG_FATAL, "Invalid Janus WebSocket address\n");
		return -1;
	}
	if((strcasecmp(protocol, "ws") && strcasecmp(protocol, "wss")) || !strlen(address)) {
		JAMRTC_LOG(LOG_FATAL, "Invalid Janus WebSocket address (only ws:// and wss:// addresses are supported)\n");
		JAMRTC_LOG(LOG_FATAL, "  -- Protocol: %s\n", protocol);
		JAMRTC_LOG(LOG_FATAL, "  -- Address:  %s\n", address);
		JAMRTC_LOG(LOG_FATAL, "  -- Path:     %s\n", path);
		return -1;
	}

	/* Take note of the settings */
	cb = callbacks;
	server_url = g_strdup(ws);
	builder = gtkbuilder;
	loop = mainloop;
	stun_server = stun;
	turn_server = turn;
	src_opts = src;
	latency = jitter;
	no_jack = disable_jack;

	/* Initialize hashtables and mutexes */
	participants = g_hash_table_new_full(g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify)jamrtc_webrtc_participant_destroy);
	participants_byid = g_hash_table_new_full(g_int64_hash, g_int64_equal,
		(GDestroyNotify)g_free, (GDestroyNotify)jamrtc_webrtc_participant_unref);
	participants_byslot = g_hash_table_new_full(NULL, NULL, NULL,
		(GDestroyNotify)jamrtc_webrtc_participant_unref);
	peerconnections = g_hash_table_new_full(g_int64_hash, g_int64_equal,
		(GDestroyNotify)g_free, (GDestroyNotify)jamrtc_webrtc_pc_unref);
	jamrtc_mutex_init(&participants_mutex);
	transactions = g_hash_table_new_full(g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, NULL);
	jamrtc_mutex_init(&transactions_mutex);

	/* Connect to Janus */
	jamrtc_connect_websockets();
	return 0;
}

/* Janus stack cleanup */
static gboolean jamrtc_webrtc_cleanup_internal(gpointer user_data) {
	/* Stop the client */
	g_atomic_int_set(&stopping, 1);
	if(ws_thread != NULL) {
		g_thread_join(ws_thread);
		ws_thread = NULL;
	}
	char *message = NULL;
	while((message = g_async_queue_try_pop(messages)) != NULL) {
		g_free(message);
	}
	g_async_queue_unref(messages);

	/* We're done */
	jamrtc_mutex_lock(&transactions_mutex);
	g_hash_table_destroy(transactions);
	transactions = NULL;
	jamrtc_mutex_unlock(&transactions_mutex);
	jamrtc_mutex_lock(&participants_mutex);
	g_hash_table_destroy(peerconnections);
	peerconnections = NULL;
	g_hash_table_destroy(participants);
	participants = NULL;
	g_hash_table_destroy(participants_byid);
	participants_byid = NULL;
	g_hash_table_destroy(participants_byslot);
	participants_byslot = NULL;
	jamrtc_webrtc_pc_destroy(local_micwebcam);
	local_micwebcam = NULL;
	jamrtc_webrtc_pc_destroy(local_instrument);
	local_instrument = NULL;
	jamrtc_mutex_unlock(&participants_mutex);

	/* Quit the main loop: this will eventually exit the application, when done */
	if(loop) {
		g_main_loop_quit(loop);
		loop = NULL;
	}
	return G_SOURCE_REMOVE;
}
void jamrtc_webrtc_cleanup() {
	/* Run this on the loop */
	GSource *timeout_source;
	timeout_source = g_timeout_source_new(0);
	g_source_set_callback(timeout_source, jamrtc_webrtc_cleanup_internal, NULL, NULL);
	g_source_attach(timeout_source, NULL);
	g_source_unref(timeout_source);
}
/* Join the room as a participant (but don't publish anything yet) */
void jamrtc_join_room(guint64 id, const char *display) {
	/* Take note of the properties */
	room_id = id;
	display_name = display;

	/* Generate a random UUID */
	local_uuid = g_uuid_string_random();
	JAMRTC_LOG(LOG_INFO, "Generated UUID: %s\n", local_uuid);

	/* Create an instance for our participant */
	local_micwebcam = jamrtc_webrtc_pc_new(local_uuid, display, FALSE, NULL);
	local_micwebcam->slot = 1;
	/* Attach to the VideoRoom plugin: we'll join once we do that */
	jamrtc_attach_handle(local_micwebcam);
}

/* Publish mic/webcam for the chat part */
static gboolean jamrtc_webrtc_publish_micwebcam_internal(gpointer user_data) {
	/* Create a GStreamer pipeline for the sendonly PeerConnection */
	jamrtc_prepare_pipeline(local_micwebcam, FALSE, !no_mic, !no_webcam);
	return G_SOURCE_REMOVE;
}
void jamrtc_webrtc_publish_micwebcam(gboolean ignore_mic, gboolean ignore_webcam, const char *device) {
	/* Take note of the properties */
	no_mic = ignore_mic;
	no_webcam = ignore_webcam;
	video_device = device;

	/* Run this on the loop */
	GSource *timeout_source;
	timeout_source = g_timeout_source_new(0);
	g_source_set_callback(timeout_source, jamrtc_webrtc_publish_micwebcam_internal, NULL, NULL);
	g_source_attach(timeout_source, NULL);
	g_source_unref(timeout_source);
}

/* Publish the instrument */
static gboolean jamrtc_webrtc_publish_instrument_internal(gpointer user_data) {
	/* Attach to the VideoRoom plugin: we'll join once we do that */
	jamrtc_attach_handle(local_instrument);
	return G_SOURCE_REMOVE;
}
void jamrtc_webrtc_publish_instrument(const char *instrument, gboolean capture_stereo) {
	/* Take note of the properties */
	stereo = capture_stereo;

	/* Create an instance for our instrument */
	local_instrument = jamrtc_webrtc_pc_new(local_uuid, display_name, FALSE, instrument);
	local_instrument->slot = 1;
	/* Run this on the loop (maybe wait if we're also publishing mic/webcam) */
	GSource *timeout_source;
	//~ timeout_source = g_timeout_source_new_seconds((no_mic && no_webcam) ? 0 : 1);
	timeout_source = g_timeout_source_new(0);
	g_source_set_callback(timeout_source, jamrtc_webrtc_publish_instrument_internal, NULL, NULL);
	g_source_attach(timeout_source, NULL);
	g_source_unref(timeout_source);
}

/* Subscribe to a remote stream */
static gboolean jamrtc_webrtc_subscribe_internal(gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL)
		return G_SOURCE_REMOVE;

	/* Attach to the VideoRoom plugin: we'll subscribe once we do that */
	jamrtc_attach_handle(pc);
	/* Done, let's wait for the event */
	jamrtc_refcount_decrease(&pc->ref);

	return G_SOURCE_REMOVE;
}
int jamrtc_webrtc_subscribe(const char *uuid, gboolean instrument) {
	if(uuid == NULL)
		return -1;
	jamrtc_mutex_lock(&participants_mutex);
	/* Find the remote participant */
	jamrtc_webrtc_participant *participant = g_hash_table_lookup(participants, uuid);
	if(participant == NULL) {
		jamrtc_mutex_unlock(&participants_mutex);
		JAMRTC_LOG(LOG_ERR, "No such participant %s\n", uuid);
		return -2;
	}
	/* Access the stream we need */
	jamrtc_webrtc_pc *pc = (instrument ? participant->instrument : participant->micwebcam);
	if(pc == NULL) {
		jamrtc_mutex_unlock(&participants_mutex);
		JAMRTC_LOG(LOG_ERR, "No such stream from participant %s\n", uuid);
		return -3;
	}
	if(pc->pipeline != NULL) {
		jamrtc_mutex_unlock(&participants_mutex);
		JAMRTC_LOG(LOG_ERR, "[%s][%s] PeerConnection already available\n",
			pc->display, pc->instrument ? pc->instrument : "chat");
		return -4;
	}
	jamrtc_refcount_increase(&pc->ref);
	jamrtc_mutex_unlock(&participants_mutex);

	/* Run this on the loop (maybe wait if we're also publishing mic/webcam) */
	GSource *timeout_source;
	//~ timeout_source = g_timeout_source_new(pc->instrument ? 1000 : 500);
	timeout_source = g_timeout_source_new(0);
	g_source_set_callback(timeout_source, jamrtc_webrtc_subscribe_internal, pc, NULL);
	g_source_attach(timeout_source, NULL);
	g_source_unref(timeout_source);

	return 0;
}


/* Helper method to wrap up and close everything */
static gboolean jamrtc_cleanup(const char *msg, enum jamrtc_state state) {
	if(msg != NULL)
		JAMRTC_LOG(LOG_ERR, "%s\n", msg);
	state = state;

	/* Tear down the WebSocket connection: this will destroy the Janus session */
		/* TODO */

	/* Notify the application */
	cb->server_disconnected();

	/* To allow usage as a GSourceFunc */
	return G_SOURCE_REMOVE;
}

/* Helper method to generate a random transaction string */
static char *jamrtc_random_transaction(char *transaction, size_t trlen) {
	g_snprintf(transaction, trlen, "%"SCNu32, g_random_int());
	return transaction;
}

/* Helper method to serialize a JsonObject to a string */
static char *jamrtc_json_to_string(JsonObject *object) {
	/* Make it the root node */
	JsonNode *root = json_node_init_object(json_node_alloc(), object);
	JsonGenerator *generator = json_generator_new();
	json_generator_set_root(generator, root);
	char *text = json_generator_to_data(generator, NULL);

	/* Release everything */
	g_object_unref(generator);
	json_node_free(root);
	return text;
}

/* Helper method to attach to the VideoRoom plugin */
static gboolean jamrtc_attach_handle(jamrtc_webrtc_pc *pc) {
	if(pc == NULL)
		return FALSE;
	/* Make sure we have a valid Janus session to use as well */
	if(session_id == 0)
		return FALSE;

	JAMRTC_LOG(LOG_INFO, "[%s][%s] Attaching to the VideoRoom plugin\n",
		pc->display, pc->instrument ? pc->instrument : "chat");
	pc->state = JAMRTC_JANUS_ATTACHING_PLUGIN;

	/* Prepare the Janus API request */
	JsonObject *attach = json_object_new();
	json_object_set_string_member(attach, "janus", "attach");
	json_object_set_int_member(attach, "session_id", session_id);
	json_object_set_string_member(attach, "plugin", "janus.plugin.videoroom");
	char transaction[12];
	json_object_set_string_member(attach, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
	char *text = jamrtc_json_to_string(attach);
	json_object_unref(attach);

	/* Track the task */
	jamrtc_mutex_lock(&transactions_mutex);
	g_hash_table_insert(transactions, g_strdup(transaction), pc);
	jamrtc_mutex_unlock(&transactions_mutex);

	/* Send the request: we'll get a response asynchronously */
	JAMRTC_LOG(LOG_VERB, "[%s][%s] Sending message: %s\n",
		pc->display, pc->instrument ? pc->instrument : "chat", text);
	jamrtc_send_message(text);
	return TRUE;
}

/* Callback to be notified about state changes in the pipeline */
static void jamrtc_pipeline_state_changed(GstBus *bus, GstMessage *msg, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	GstState old_state, new_state, pending_state;
	gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
	JAMRTC_LOG(LOG_WARN, "[%s][%s] Pipeline state changed: %s\n",
		pc->display, pc->instrument ? pc->instrument : "chat",
		gst_element_state_get_name(new_state));
	/* TODO Should we use this to refresh the UI? */
}

/* Helper method to setup the webrtcbin pipeline, and trigger the negotiation process */
static volatile gint pc_index = 0;
static gboolean jamrtc_prepare_pipeline(jamrtc_webrtc_pc *pc, gboolean subscription, gboolean do_audio, gboolean do_video) {
	if(pc == NULL)
		return FALSE;
	g_atomic_int_inc(&pc_index);
	char pc_name[10];
	g_snprintf(pc_name, sizeof(pc_name), "pc%d", g_atomic_int_get(&pc_index));
	/* Prepare the pipeline, using the info we got from the command line */
	char stun[255], turn[255], audio[1024], video[1024], gst_pipeline[2048];
	stun[0] = '\0';
	turn[0] = '\0';
	audio[0] = '\0';
	video[0] = '\0';
	if(stun_server != NULL)
		g_snprintf(stun, sizeof(stun), "stun-server=stun://%s", stun_server);
	if(turn_server != NULL)
		g_snprintf(turn, sizeof(turn), "turn-server=turn://%s", turn_server);
	/* The pipeline works differently depending on what the PeerConnection is for */
	if(!subscription) {
		/* We're publishing something: set up the capture nodes */
		if(pc == local_micwebcam) {
			/* We're trying to capture mic and/or webcam */
			if(do_audio) {
				guint32 audio_ssrc = g_random_int();
				if(!no_jack) {
					/* Use jackaudiosrc and name it */
					g_snprintf(audio, sizeof(audio), "jackaudiosrc %s connect=0 client-name=\"JamRTC mic\" ! audio/x-raw,channels=1 ! "
						"audioconvert ! audioresample ! audio/x-raw,channels=1,rate=48000 ! tee name=at ! "
							"queue ! audioconvert ! wavescope style=1 ! videoconvert ! xvimagesink name=\"ampreview\" "
						"at. ! queue ! opusenc bitrate=20000 ! "
						"rtpopuspay pt=111 ssrc=%"SCNu32" ! queue ! application/x-rtp,media=audio,encoding-name=OPUS,payload=111 ! %s.",
							src_opts, audio_ssrc, pc_name);
				} else {
					/* Use autoaudiosrc */
					g_snprintf(audio, sizeof(audio), "autoaudiosrc %s ! audio/x-raw,channels=1 ! "
						"audioconvert ! audioresample ! audio/x-raw,channels=1,rate=48000 ! tee name=at ! "
							"queue ! audioconvert ! wavescope style=1 ! videoconvert ! xvimagesink name=\"ampreview\" "
						"at. ! queue ! opusenc bitrate=20000 ! "
						"rtpopuspay pt=111 ssrc=%"SCNu32" ! queue ! application/x-rtp,media=audio,encoding-name=OPUS,payload=111 ! %s.",
							src_opts, audio_ssrc, pc_name);
				}
			}
			if(do_video) {
				guint32 video_ssrc = g_random_int();
				g_snprintf(video, sizeof(video), "v4l2src device=%s ! videoconvert ! videoscale ! "
					"video/x-raw,width=320,height=180 ! tee name=vt ! queue ! xvimagesink name=\"vpreview\" "
					"vt. ! queue ! vp8enc deadline=1 cpu-used=10 target-bitrate=128000 ! "
					"rtpvp8pay pt=96 ssrc=%"SCNu32" ! queue ! application/x-rtp,media=video,encoding-name=VP8,payload=96 ! %s.",
						video_device, video_ssrc, pc_name);
			}
		} else if(!subscription && pc == local_instrument) {
			/* We're trying to capture an instrument */
			if(do_audio) {
				guint32 audio_ssrc = g_random_int();
				if(!no_jack) {
					/* Use jackaudiosrc and name it */
					g_snprintf(audio, sizeof(audio), "jackaudiosrc %s connect=0 client-name=\"JamRTC %s\" ! audio/x-raw,channels=%d ! "
						"audioconvert ! audioresample ! audio/x-raw,channels=%d,rate=48000 ! tee name=at ! "
							"queue ! audioconvert ! wavescope style=3 ! videoconvert ! xvimagesink name=\"aipreview\" "
						"at. ! queue ! opusenc bitrate=20000 ! "
						"rtpopuspay pt=111 ssrc=%"SCNu32" ! queue ! application/x-rtp,media=audio,encoding-name=OPUS,payload=111 ! %s.",
							src_opts, pc->instrument, stereo ? 2 : 1, stereo ? 2 : 1, audio_ssrc, pc_name);
				} else {
					/* Use autoaudiosrc */
					g_snprintf(audio, sizeof(audio), "autoaudiosrc %s ! audio/x-raw,channels=%d ! "
						"audioconvert ! audioresample ! audio/x-raw,channels=%d,rate=48000 ! tee name=at ! "
							"queue ! audioconvert ! wavescope style=3 ! videoconvert ! xvimagesink name=\"aipreview\" "
						"at. ! queue ! opusenc bitrate=20000 ! "
						"rtpopuspay pt=111 ssrc=%"SCNu32" ! queue ! application/x-rtp,media=audio,encoding-name=OPUS,payload=111 ! %s.",
							src_opts, stereo ? 2 : 1, stereo ? 2 : 1, audio_ssrc, pc_name);
				}
			}
		}
		/* Let's build the pipeline out of the elements we crafted above */
		g_snprintf(gst_pipeline, sizeof(gst_pipeline), "webrtcbin name=%s bundle-policy=%d %s %s %s %s",
			pc_name, (do_audio && do_video ? 3 : 0), stun, turn, video, audio);
		JAMRTC_LOG(LOG_INFO, "[%s][%s] Initializing the GStreamer pipeline:\n  -- %s\n",
			pc->display, pc->instrument ? pc->instrument : "chat", gst_pipeline);
		GError *error = NULL;
		pc->pipeline = gst_parse_launch(gst_pipeline, &error);
		if(error) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Failed to parse/launch the pipeline: %s\n",
				pc->display, pc->instrument ? pc->instrument : "chat", error->message);
			g_error_free(error);
			goto err;
		}
		/* Get a pointer to the PeerConnection object */
		pc->peerconnection = gst_bin_get_by_name(GST_BIN(pc->pipeline), pc_name);
		/* Let's configure the function to be invoked when an SDP offer can be prepared */
		g_signal_connect(pc->peerconnection, "on-negotiation-needed", G_CALLBACK(jamrtc_negotiation_needed), pc);
	} else {
		/* Since this is a subscription, we just create the webrtcbin element for the moment */
		char pipe_name[100];
		g_snprintf(pipe_name, sizeof(pipe_name), "pipe-%s", pc_name);
		pc->pipeline = gst_pipeline_new(pipe_name);
		pc->peerconnection = gst_element_factory_make("webrtcbin", pc_name);
		g_object_set(pc->peerconnection, "bundle-policy", (do_audio && do_video ? 3 : 0), NULL);
		if(stun_server != NULL)
			g_object_set(pc->peerconnection, "stun-server", stun_server, NULL);
		if(turn_server != NULL)
			g_object_set(pc->peerconnection, "turn-server", turn_server, NULL);
		gst_bin_add_many(GST_BIN(pc->pipeline), pc->peerconnection, NULL);
		gst_element_sync_state_with_parent(pc->pipeline);
		/* We'll handle incoming streams, and how to render them, dynamically */
		g_signal_connect(pc->peerconnection, "pad-added", G_CALLBACK(jamrtc_incoming_stream), pc);
	}
	pc->audio = do_audio;
	pc->video = do_video;
	/* We need a different callback to be notified about candidates to trickle to Janus */
	g_signal_connect(pc->peerconnection, "on-ice-candidate", G_CALLBACK(jamrtc_trickle_candidate), pc);

	/* For instruments, replace the jitter buffer size in rtpbin (it's 200ms by default, we definitely want less) */
	if(pc->instrument != NULL) {
		GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(pc->peerconnection), "rtpbin");
		g_object_set(rtpbin,
			"latency", latency,
			"buffer-mode", 0,
			NULL);
		guint rtp_latency = 0;
		g_object_get(rtpbin, "latency", &rtp_latency, NULL);
		JAMRTC_LOG(LOG_INFO, "[%s][%s] Configured jitter-buffer size (latency) for PeerConnection to %ums\n",
			pc->display, pc->instrument ? pc->instrument : "chat", rtp_latency);
	}

	/* Embed the video elements in the UI */
	if(!subscription) {
		if(do_audio) {
			const char *sinkname = (pc == local_micwebcam ? "ampreview" : "aipreview");
			jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_STREAM,
				pc, FALSE, sinkname);
			g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
		}
		if(do_video) {
			const char *sinkname = "vpreview";
			jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_STREAM,
				pc, TRUE, sinkname);
			g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
		}
		/* Save updated pipeline to a dot file, in case we're debugging */
		char dot_name[100];
		g_snprintf(dot_name, sizeof(dot_name), "%s_%s",
			pc->display, pc->instrument ? pc->instrument : "mic");
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pc->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dot_name);
	}
	//~ /* Monitor changes on the pipeline state */
	//~ GstBus *bus = gst_element_get_bus(pc->pipeline);
	//~ gst_bus_add_signal_watch(bus);
	//~ g_signal_connect(G_OBJECT(bus), "message::state-changed", (GCallback)jamrtc_pipeline_state_changed, pc);
	//~ gst_object_unref(bus);
	/* Start the pipeline */
	gst_element_set_state(pc->pipeline, GST_STATE_READY);
	gst_object_unref(pc->peerconnection);
	JAMRTC_LOG(LOG_INFO, "[%s][%s] Starting GStreamer pipeline\n",
		pc->display, pc->instrument ? pc->instrument : "chat");
	GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pc->pipeline), GST_STATE_PLAYING);
	if(ret == GST_STATE_CHANGE_FAILURE) {
		JAMRTC_LOG(LOG_ERR, "[%s][%s] Failed to start the pipeline%s\n",
			pc->display, pc->instrument ? pc->instrument : "chat",
			no_jack ? "" : " (is JACK running?)");
		goto err;
	}

	/* Done */
	return TRUE;

err:
	/* If we got here, something went wrong */
	if(pc->pipeline)
		g_clear_object(&pc->pipeline);
	if(pc->peerconnection)
		pc->peerconnection = NULL;
	return FALSE;
}

/* Callback invoked when we need to prepare an SDP offer */
static void jamrtc_negotiation_needed(GstElement *element, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL) {
		JAMRTC_LOG(LOG_ERR, "Invalid PeerConnection object\n");
		return;
	}
	pc->state = JAMRTC_JANUS_SDP_PREPARED;
	/* Create the offer */
	GstPromise *promise = gst_promise_new_with_change_func(jamrtc_sdp_available, pc, NULL);
	g_signal_emit_by_name(pc->peerconnection, "create-offer", NULL, promise);
}

/* Callback invoked when we have an SDP offer or answer ready to be sent */
static void jamrtc_sdp_available(GstPromise *promise, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL) {
		JAMRTC_LOG(LOG_ERR, "Invalid PeerConnection object\n");
		return;
	}
	/* Make sure we're in the right state */
	if(pc->state < JAMRTC_JANUS_SDP_PREPARED) {
		JAMRTC_LOG(LOG_WARN, "[%s][%s] Can't send offer/answer, not in a PeerConnection\n",
			pc->display, pc->instrument ? pc->instrument : "chat");
		return;
	}
	if(gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED)
		return;
	const char *type = (pc == local_micwebcam || pc == local_instrument) ? "offer" : "answer";
	const GstStructure *reply = gst_promise_get_reply(promise);
	GstWebRTCSessionDescription *offeranswer = NULL;
	gst_structure_get(reply, type, GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offeranswer, NULL);
	gst_promise_unref(promise);

	/* Set the local description locally */
	promise = gst_promise_new();
	g_signal_emit_by_name(pc->peerconnection, "set-local-description", offeranswer, promise);
	gst_promise_interrupt(promise);
	gst_promise_unref(promise);

	/* Convert the SDP object to a string */
	char *text = gst_sdp_message_as_text(offeranswer->sdp);
	/* Fix the SDP, as with max-bundle GStreamer will set 0 in m-line ports */
	const char *old_string = "m=audio 0";
	const char *new_string = "m=audio 9";
	char *pos = strstr(text, old_string), *tmp = NULL;
	int i = 0;
	while(pos) {
		i++;
		memcpy(pos, new_string, strlen(new_string));
		pos += strlen(old_string);
		tmp = strstr(pos, old_string);
		pos = tmp;
	}
	JAMRTC_LOG(LOG_INFO, "[%s][%s] Sending SDP %s\n",
		pc->display, pc->instrument ? pc->instrument : "chat",
		pc->remote ? "answer" : "offer");
	JAMRTC_LOG(LOG_VERB, "%s\n", text);
	/* Prepare a JSEP offer */
	JsonObject *sdp = json_object_new();
	json_object_set_string_member(sdp, "type", pc->remote ? "answer" : "offer");
	json_object_set_string_member(sdp, "sdp", text);
	g_free(text);

	/* Send the SDP to Janus */
	if(pc == local_micwebcam || pc == local_instrument) {
		/* Prepare the request to the VideoRoom plugin: it's a "configure"
		 * for mic/webcam, and "joinandconfigure" for instruments instead */
		JsonObject *req = json_object_new();
		json_object_set_string_member(req, "request", pc == local_micwebcam ? "configure" : "joinandconfigure");
		if(pc == local_instrument) {
			/* For instruments, we also use this request to join the room  */
			JsonObject *info = json_object_new();
			json_object_set_string_member(info, "uuid", local_uuid);
			json_object_set_string_member(info, "display", pc->display);
			json_object_set_string_member(info, "instrument", pc->instrument);
			char *participant = jamrtc_json_to_string(info);
			json_object_unref(info);
			/* Join the room as a participant */
			json_object_set_string_member(req, "ptype", "publisher");
			json_object_set_int_member(req, "room", room_id);
			json_object_set_string_member(req, "display", participant);
			json_object_set_boolean_member(req, "audio", TRUE);
			json_object_set_boolean_member(req, "video", FALSE);
			g_free(participant);
		}
		/* Prepare the Janus API request to send the message to the plugin */
		JsonObject *msg = json_object_new();
		json_object_set_string_member(msg, "janus", "message");
		char transaction[12];
		json_object_set_string_member(msg, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
		json_object_set_int_member(msg, "session_id", session_id);
		json_object_set_int_member(msg, "handle_id", pc->handle_id);
		json_object_set_object_member(msg, "body", req);
		json_object_set_object_member(msg, "jsep", sdp);
		text = jamrtc_json_to_string(msg);
		json_object_unref(msg);
		/* Send the request via WebSockets */
		JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
		jamrtc_send_message(text);
	} else {
		/* Prepare the request to the VideoRoom plugin: it's just "start" */
		JsonObject *req = json_object_new();
		json_object_set_string_member(req, "request", "start");
		/* Prepare the Janus API request to send the message to the plugin */
		JsonObject *msg = json_object_new();
		json_object_set_string_member(msg, "janus", "message");
		char transaction[12];
		json_object_set_string_member(msg, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
		json_object_set_int_member(msg, "session_id", session_id);
		json_object_set_int_member(msg, "handle_id", pc->handle_id);
		json_object_set_object_member(msg, "body", req);
		json_object_set_object_member(msg, "jsep", sdp);
		text = jamrtc_json_to_string(msg);
		json_object_unref(msg);
		/* Send the request via WebSockets */
		JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
		jamrtc_send_message(text);
	}
	gst_webrtc_session_description_free(offeranswer);
}

/* Callback invoked when a candidate to trickle becomes available */
static void jamrtc_trickle_candidate(GstElement *webrtc,
		guint mlineindex, char *candidate, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL) {
		JAMRTC_LOG(LOG_ERR, "Invalid PeerConnection object\n");
		return;
	}
	if(mlineindex != 0)
		return;
	/* Make sure we're in the right state*/
	if(pc->state < JAMRTC_JANUS_SDP_PREPARED) {
		JAMRTC_LOG(LOG_WARN, "[%s][%s] Can't trickle, not in a PeerConnection\n",
			pc->display, pc->instrument ? pc->instrument : "chat");
		return;
	}

	/* Prepare the Janus API request */
	JsonObject *trickle = json_object_new();
	json_object_set_string_member(trickle, "janus", "trickle");
	json_object_set_int_member(trickle, "session_id", session_id);
	json_object_set_int_member(trickle, "handle_id", pc->handle_id);
	char transaction[12];
	json_object_set_string_member(trickle, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
	JsonObject *ice = json_object_new();
	json_object_set_string_member(ice, "candidate", candidate);
	json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
	json_object_set_object_member(trickle, "candidate", ice);
	char *text = jamrtc_json_to_string(trickle);
	json_object_unref(trickle);

	/* Send the request via WebSockets */
	JAMRTC_LOG(LOG_VERB, "[%s][%s] Sending message: %s\n",
		pc->display, pc->instrument ? pc->instrument : "chat", text);
	jamrtc_send_message(text);
}

/* Callbacks invoked when we have a stream from an existing subscription */
static void jamrtc_handle_media_stream(jamrtc_webrtc_pc *pc, GstPad *pad, gboolean video) {
	GstElement *entry = gst_element_factory_make(video ? "queue" : "audioconvert", NULL);
	GstElement *conv = gst_element_factory_make(video ? "videoconvert" : "audioconvert", NULL);
	GstElement *sink = gst_element_factory_make(video ? "xvimagesink" :
		(no_jack ? "autoaudiosink" : "jackaudiosink"), NULL);
	if(!video) {
		/* Create a queue to add after the first audioconvert */
		GstElement *q = gst_element_factory_make("queue", NULL);
		/* Name the jackaudiosink element */
		if(!no_jack) {
			/* Assign a name to the Jack node */
			char name[100];
			g_snprintf(name, sizeof(name), "%s's %s",
				pc->display, pc->instrument ? pc->instrument : "mic");
			g_object_set(sink, "client-name", name, NULL);
			//~ g_object_set(sink, "connect", 0, NULL);
		}
		/* This is audio, add a resampler too and a wavescope visualizer */
		GstElement *resample = gst_element_factory_make("audioresample", NULL);
		GstElement *tee = gst_element_factory_make("tee", NULL);
		GstElement *qa = gst_element_factory_make("queue", NULL);
		GstElement *qv = gst_element_factory_make("queue", NULL);
		GstElement *wav = gst_element_factory_make("wavescope", NULL);
		g_object_set(wav, "style", pc->instrument ? 3 : 1, NULL);
		GstElement *vconv = gst_element_factory_make("videoconvert", NULL);
		GstElement *vsink = gst_element_factory_make("xvimagesink", NULL);
		g_object_set(vsink, "name", pc->instrument ? "aiwave" : "amwave", NULL);
		gst_bin_add_many(GST_BIN(pc->pipeline), entry, q, conv, resample, tee, qa, sink, qv, wav, vconv, vsink, NULL);
		gst_element_sync_state_with_parent(entry);
		gst_element_sync_state_with_parent(q);
		gst_element_sync_state_with_parent(conv);
		gst_element_sync_state_with_parent(resample);
		gst_element_sync_state_with_parent(tee);
		gst_element_sync_state_with_parent(qa);
		gst_element_sync_state_with_parent(sink);
		gst_element_sync_state_with_parent(qv);
		gst_element_sync_state_with_parent(wav);
		gst_element_sync_state_with_parent(vconv);
		gst_element_sync_state_with_parent(vsink);
		if(!gst_element_link_many(entry, q, tee, NULL)) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error linking audio pad to tee...\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		}
		if(!gst_element_link_many(qa, conv, resample, sink, NULL)) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error linking audio to sink...\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		}
		if(!gst_element_link_many(qv, wav, vconv, vsink, NULL)) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error linking audio to visualizer...\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		}
		g_object_set(sink, "sync", FALSE, NULL);
		g_object_set(vsink, "sync", FALSE, NULL);
		if(pc->slot != 0) {
			/* Render the video */
			jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_STREAM,
				pc, FALSE, pc->instrument ? "aiwave" : "amwave");
			g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
		}
		/* Manually connect the tee pads */
		GstPad *tee_audio_pad = gst_element_get_request_pad(tee, "src_%u");
		GstPad *queue_audio_pad = gst_element_get_static_pad(qa, "sink");
		if(gst_pad_link(tee_audio_pad, queue_audio_pad) != GST_PAD_LINK_OK) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error linking audio pads\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		}
		GstPad *tee_video_pad = gst_element_get_request_pad(tee, "src_%u");
		GstPad *queue_video_pad = gst_element_get_static_pad(qv, "sink");
		if(gst_pad_link(tee_video_pad, queue_video_pad) != GST_PAD_LINK_OK) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error linking audio pads\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		}
		/* Save updated pipeline to a dot file, in case we're debugging */
		char dot_name[100];
		g_snprintf(dot_name, sizeof(dot_name), "%s_%s",
			pc->display, pc->instrument ? pc->instrument : "mic");
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pc->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dot_name);
	} else {
		g_object_set(sink, "name", "video", NULL);
		gst_bin_add_many(GST_BIN(pc->pipeline), entry, conv, sink, NULL);
		gst_element_sync_state_with_parent(entry);
		gst_element_sync_state_with_parent(conv);
		gst_element_sync_state_with_parent(sink);
		gst_element_link_many(entry, conv, sink, NULL);
		g_object_set(sink, "sync", FALSE, NULL);
		if(pc->slot != 0) {
			/* Render the video */
			jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_STREAM,
				pc, TRUE, "video");
			g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
		}
	}
	/* Finally, let's connect the webrtcbin pad to our entry queue */
	GstPad *entry_pad = gst_element_get_static_pad(entry, "sink");
	GstPadLinkReturn ret = gst_pad_link(pad, entry_pad);
	if(ret != GST_PAD_LINK_OK) {
		JAMRTC_LOG(LOG_ERR, "[%s][%s] Error feeding %s (%d)...\n",
			pc->display, pc->instrument ? pc->instrument : "chat",
			GST_OBJECT_NAME(gst_element_get_factory(sink)), ret);
	}
	if(!video)
		GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pc->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sink-3");
}
static void jamrtc_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL) {
		JAMRTC_LOG(LOG_ERR, "Invalid PeerConnection object\n");
		return;
	}
	/* The decodebin element has a new stream, render it */
	if(!gst_pad_has_current_caps(pad)) {
		JAMRTC_LOG(LOG_ERR, "[%s][%s] Pad '%s' has no caps, ignoring\n",
			GST_PAD_NAME(pad), pc->display, pc->instrument ? pc->instrument : "chat");
		return;
	}
	GstCaps *caps = gst_pad_get_current_caps(pad);
	const char *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
	if(g_str_has_prefix(name, "video")) {
		jamrtc_handle_media_stream(pc, pad, TRUE);
	} else if(g_str_has_prefix(name, "audio")) {
		jamrtc_handle_media_stream(pc, pad, FALSE);
	} else {
		JAMRTC_LOG(LOG_ERR, "[%s][%s] Unknown pad %s, ignoring",
			pc->display, pc->instrument ? pc->instrument : "chat", GST_PAD_NAME(pad));
	}
}
static void jamrtc_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
	jamrtc_webrtc_pc *pc = (jamrtc_webrtc_pc *)user_data;
	if(pc == NULL) {
		JAMRTC_LOG(LOG_ERR, "Invalid PeerConnection object\n");
		return;
	}
	/* Create an element to decode the stream */
	JAMRTC_LOG(LOG_INFO, "[%s][%s] Creating decodebin element\n",
		pc->display, pc->instrument ? pc->instrument : "chat");
	GstElement *decodebin = gst_element_factory_make("decodebin", NULL);
	g_signal_connect(decodebin, "pad-added", G_CALLBACK(jamrtc_incoming_decodebin_stream), pc);
	gst_bin_add(GST_BIN(pc->pipeline), decodebin);
	gst_element_sync_state_with_parent(decodebin);
	GstPad *sinkpad = gst_element_get_static_pad(decodebin, "sink");
	gst_pad_link(pad, sinkpad);
	gst_object_unref(sinkpad);
}

/* Helper method to send a keep-alive */
static gboolean jamrtc_send_keepalive(gpointer user_data) {
	if(session_id == 0)
		return FALSE;

	/* Prepare the Janus API request */
	JsonObject *keepalive = json_object_new();
	json_object_set_string_member(keepalive, "janus", "keepalive");
	json_object_set_int_member(keepalive, "session_id", session_id);
	char transaction[12];
	json_object_set_string_member(keepalive, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
	char *text = jamrtc_json_to_string(keepalive);
	json_object_unref(keepalive);

	/* Send the request via WebSockets */
	JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
	jamrtc_send_message(text);

	return TRUE;
}

/* Thread to implement the WebSockets loop */
static gpointer jamrtc_ws_thread(gpointer data) {
	JAMRTC_LOG(LOG_VERB, "Joining Jamus WebSocket client thread\n");
	while(!g_atomic_int_get(&stopping)) {
		/* Loop until we have to stop */
		lws_service(context, 50);
	}
	JAMRTC_LOG(LOG_VERB, "Leaving Jamus WebSocket client thread\n");
	return NULL;
}
/* Helper method to connect to the remote Janus backend via WebSockets */
static void jamrtc_connect_websockets(void) {
	/* Connect */
	JAMRTC_LOG(LOG_INFO, "Connecting to Janus: %s\n", server_url);
	gboolean secure = !strcasecmp(protocol, "wss");
	state = JAMRTC_JANUS_CONNECTING;

	struct lws_context_creation_info info = { 0 };
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	if(secure)
		info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	context = lws_create_context(&info);
	if(context == NULL) {
		jamrtc_cleanup("Creating libwebsocket context failed", JAMRTC_JANUS_CONNECTION_ERROR);
		return;
	}
	struct lws_client_connect_info i = { 0 };
	i.host = address;
	i.origin = address;
	i.address = address;
	i.port = port;
	char wspath[256];
	g_snprintf(wspath, sizeof(wspath), "/%s", path);
	i.path = wspath;
	i.context = context;
	if(secure)
		i.ssl_connection = 1;
	i.ietf_version_or_minus_one = -1;
	i.client_exts = exts;
	i.protocol = protocols[0].name;
	wsi = lws_client_connect_via_info(&i);
	if(wsi == NULL) {
		jamrtc_cleanup("Error initializing WebSocket connection", JAMRTC_JANUS_CONNECTION_ERROR);
		return;
	}
	jamrtc_mutex_init(&writable_mutex);

	/* Initialize the message queue */
	messages = g_async_queue_new();

	/* Start a thread to handle the WebSockets event loop */
	GError *error = NULL;
	ws_thread = g_thread_try_new("jamrtc ws", jamrtc_ws_thread, NULL, &error);
	if(error != NULL) {
		JAMRTC_LOG(LOG_FATAL, "Got error %d (%s) trying to launch the Jamus WebSocket client thread...\n",
			error->code, error->message ? error->message : "??");
		jamrtc_cleanup("Thread error", JAMRTC_JANUS_CONNECTION_ERROR);
		g_error_free(error);
		return;
	}
}

/* Helper to send a message via WebSockets */
void jamrtc_send_message(char *text) {
	g_async_queue_push(messages, text);
#if (LWS_LIBRARY_VERSION_MAJOR >= 3)
	if(context != NULL)
		lws_cancel_service(context);
#else
	/* On libwebsockets < 3.x we use lws_callback_on_writable */
	janus_mutex_lock(&writable_mutex);
	if(wsi != NULL)
		lws_callback_on_writable(wsi);
	janus_mutex_unlock(&writable_mutex);
#endif
}

/* Helper method to create a new Janus session */
static gboolean jamrtc_create_session(void) {
	JAMRTC_LOG(LOG_INFO, "Creating a new Janus session\n");
	state = JAMRTC_JANUS_CREATING_SESSION;

	/* Prepare the Janus API request */
	JsonObject *create = json_object_new();
	json_object_set_string_member(create, "janus", "create");
	char transaction[12];
	json_object_set_string_member(create, "transaction", jamrtc_random_transaction(transaction, sizeof(transaction)));
	char *text = jamrtc_json_to_string(create);
	json_object_unref(create);

	/* Send the request, we'll get a response asynchronously */
	JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
	jamrtc_send_message(text);

	return TRUE;
}

/* Helper method to parse an attendee/publisher, in order to
 * figure out if there's a new participant or stream available */
static void jamrtc_parse_participant(JsonObject *p, gboolean publisher) {
	if(p == NULL)
		return;
	guint64 user_id = json_object_get_int_member(p, "id");
	if((local_micwebcam && local_micwebcam->user_id == user_id) ||
			(local_instrument && local_instrument->user_id == user_id)) {
		/* This is us, ignore */
		return;
	}
	/* This display property is actually supposed to be a stringified JSON, parse it */
	const char *display_json = json_object_get_string_member(p, "display");
	const char *uuid = NULL, *display = display_json, *instrument = NULL;
	JsonParser *display_parser = json_parser_new();
	if(json_parser_load_from_data(display_parser, display_json, -1, NULL)) {
		JsonNode *display_root = json_parser_get_root(display_parser);
		if(JSON_NODE_HOLDS_OBJECT(display_root)) {
			JsonObject *display_object = json_node_get_object(display_root);
			if(json_object_has_member(display_object, "uuid"))
				uuid = json_object_get_string_member(display_object, "uuid");
			if(json_object_has_member(display_object, "display"))
				display = json_object_get_string_member(display_object, "display");
			if(json_object_has_member(display_object, "instrument"))
				instrument = json_object_get_string_member(display_object, "instrument");
		}
	}
	if(uuid != NULL && !strcasecmp(uuid, local_uuid)) {
		/* This is us, ignore */
		if(display_parser != NULL)
			g_object_unref(display_parser);
		return;
	}
	gboolean has_audio = json_object_has_member(p, "audio_codec");
	gboolean has_video = json_object_has_member(p, "video_codec");
	gboolean new_participant = FALSE;
	jamrtc_mutex_lock(&participants_mutex);
	jamrtc_webrtc_participant *participant = uuid ? g_hash_table_lookup(participants, uuid) : NULL;
	if(participant == NULL)
		participant = g_hash_table_lookup(participants_byid, &user_id);
	if(participant == NULL) {
		/* Create a new participant instance */
		new_participant = TRUE;
		participant = g_malloc0(sizeof(jamrtc_webrtc_participant));
		participant->uuid = uuid ? g_strdup(uuid) : g_uuid_string_random();
		participant->display = g_strdup(display);
		jamrtc_refcount_init(&participant->ref, jamrtc_webrtc_participant_free);
		/* Find a slot for this participant */
		guint slot = 2;
		for(slot=2; slot <=4; slot++) {
			if(g_hash_table_lookup(participants_byslot, GUINT_TO_POINTER(slot)) == NULL) {
				/* Found */
				participant->slot = slot;
				g_hash_table_insert(participants_byslot, GUINT_TO_POINTER(slot), participant);
				jamrtc_refcount_increase(&participant->ref);
				break;
			}
		}
		if(participant->slot == 0) {
			JAMRTC_LOG(LOG_WARN, "No slot available for this participant, they won't be rendered in the UI\n");
		} else {
			/* Update the UI */
			jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_PARTICIPANT,
				participant, FALSE, NULL);
			g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
		}
		/* Insert into the hashtables */
		g_hash_table_insert(participants, g_strdup(participant->uuid), participant);
		jamrtc_refcount_increase(&participant->ref);
	}
	if(instrument != NULL) {
		participant->instrument_user_id = user_id;
		if(participant->instrument == NULL)
			participant->instrument = jamrtc_webrtc_pc_new(participant->uuid, display, TRUE, instrument);
		participant->instrument->user_id = participant->instrument_user_id;
		participant->instrument->slot = participant->slot;
		if(publisher) {
			participant->instrument->audio = has_audio;
			participant->instrument->video = has_video;
		}
	} else {
		participant->user_id = user_id;
		if(participant->micwebcam == NULL)
			participant->micwebcam = jamrtc_webrtc_pc_new(participant->uuid, display, TRUE, NULL);
		participant->micwebcam->user_id = participant->user_id;
		participant->micwebcam->slot = participant->slot;
		if(publisher) {
			participant->micwebcam->audio = has_audio;
			participant->micwebcam->video = has_video;
		}
	}
	if(g_hash_table_lookup(participants_byid, &user_id) == NULL) {
		jamrtc_refcount_increase(&participant->ref);
		g_hash_table_insert(participants_byid, jamrtc_uint64_dup(user_id), participant);
	}
	jamrtc_mutex_unlock(&participants_mutex);
	/* Notify the application, if needed */
	if(new_participant)
		cb->participant_joined(participant->uuid, display);
	if(publisher)
		cb->stream_started(participant->uuid, display, instrument, has_audio, has_video);
	if(display_parser != NULL)
		g_object_unref(display_parser);
}
/* Helper method to parse a list of attendees/publishers, in order to
 * figure out if there's a new participant or stream available */
static void jamrtc_parse_participants(JsonArray *list, gboolean publishers) {
	guint len = json_array_get_length(list), i = 0;
	JsonNode *node = NULL;
	JsonObject *p = NULL;
	for(i=0; i<len; i++) {
		node = json_array_get_element(list, i);
		if(json_node_get_node_type(node) != JSON_NODE_OBJECT)
			continue;
		p = json_node_get_object(node);
		jamrtc_parse_participant(p, publishers);
	}
}

/* Callback invoked when we receive a message from Janus via WebSockets */
static void jamrtc_server_message(char *text) {
	JAMRTC_LOG(LOG_VERB, "Got message: '%s'\n", text);

	/* Prepare to handle the transaction */
	gboolean remove_transaction = FALSE;
	const char *transaction = NULL;
	jamrtc_webrtc_pc *pc = NULL;

	/* Make sure the text we just received is valid JSON */
	JsonParser *parser = json_parser_new();
	if(!json_parser_load_from_data(parser, text, -1, NULL)) {
		JAMRTC_LOG(LOG_ERR, "Not JSON, ignoring... '%s'\n", text);
		goto done;
	}
	JsonNode *root = json_parser_get_root(parser);
	if(!JSON_NODE_HOLDS_OBJECT(root)) {
		JAMRTC_LOG(LOG_ERR, "Invalid JSON message, ignoring... '%s'\n", text);
		goto done;
	}
	JsonObject *object = json_node_get_object(root);
	if(!json_object_has_member(object, "janus")) {
		JAMRTC_LOG(LOG_ERR, "No 'janus' field in the received message, ignoring...\n");
		goto done;
	}
	const char *response = json_object_get_string_member(object, "janus");

	/* Get the transaction, and check if there's a related object */
	transaction = NULL;
	if(json_object_has_member(object, "transaction")) {
		transaction = json_object_get_string_member(object, "transaction");
		jamrtc_mutex_lock(&transactions_mutex);
		pc = g_hash_table_lookup(transactions, transaction);
		jamrtc_mutex_unlock(&transactions_mutex);
	}
	if(pc != NULL) {
		/* Let's get rid of this transaction when we're done */
		remove_transaction = TRUE;
	} else if(json_object_has_member(object, "sender")) {
		/* Not a transaction we know or originated ourselves, check the
		 * handle ID to see if we know who this event is belongs to */
		jamrtc_mutex_lock(&participants_mutex);
		guint64 handle_id = json_object_get_int_member(object, "sender");
		pc = g_hash_table_lookup(peerconnections, &handle_id);
		jamrtc_mutex_unlock(&participants_mutex);
	}

	/* We handle the incoming message differently, depending on the context and related state */
	if(session_id == 0) {
		/* We don't have a session ID yet, so this must be a response to our "create" */
		JsonObject *child = json_object_get_object_member(object, "data");
		if(!json_object_has_member(child, "id")) {
			jamrtc_cleanup("ERROR: no session ID", JAMRTC_JANUS_API_ERROR);
			goto done;
		}
		session_id = json_object_get_int_member(child, "id");
		state = JAMRTC_JANUS_SESSION_CREATED;
		JAMRTC_LOG(LOG_INFO, "  -- Session created: %"SCNu64"\n", session_id);
		/* Start the keep-alive timer */
		keep_alives = g_timeout_source_new_seconds(15);
		g_source_set_priority(keep_alives, G_PRIORITY_DEFAULT);
		g_source_set_callback(keep_alives, jamrtc_send_keepalive, NULL, NULL);
		g_source_attach(keep_alives, NULL);
		/* Notify the application */
		cb->server_connected();
		goto done;
	}
	/* Did we receive a response to something we needed? */
	if(pc == NULL)
		goto done;
	/* This is related to a (new?) PeerConnection */
	if(pc->handle_id == 0) {
		/* We don't have a handle ID yet, so this must be a response to our "attach" */
		JsonObject *child = json_object_get_object_member(object, "data");
		if(!json_object_has_member(child, "id")) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] No handle ID in response to our attach\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
			pc->state = JAMRTC_JANUS_API_ERROR;
			goto done;
		}
		pc->handle_id = json_object_get_int_member(child, "id");
		pc->state = JAMRTC_JANUS_HANDLE_ATTACHED;
		JAMRTC_LOG(LOG_INFO, "[%s][%s]  -- Handle attached: %"SCNu64"\n",
			pc->display, pc->instrument ? pc->instrument : "chat", pc->handle_id);
		jamrtc_mutex_lock(&participants_mutex);
		jamrtc_refcount_increase(&pc->ref);
		g_hash_table_insert(peerconnections, jamrtc_uint64_dup(pc->handle_id), pc);
		jamrtc_mutex_unlock(&participants_mutex);
		/* Check if we should automatically do something */
		if(pc == local_micwebcam) {
			/* We use a stringified JSON object as our display, to carry more info */
			JsonObject *info = json_object_new();
			json_object_set_string_member(info, "uuid", local_uuid);
			json_object_set_string_member(info, "display", pc->display);
			char *participant = jamrtc_json_to_string(info);
			json_object_unref(info);
			/* Join the room as a participant */
			JsonObject *req = json_object_new();
			json_object_set_string_member(req, "request", "join");
			json_object_set_string_member(req, "ptype", "publisher");
			json_object_set_int_member(req, "room", room_id);
			json_object_set_string_member(req, "display", participant);
			json_object_set_boolean_member(req, "audio", !no_mic);
			json_object_set_boolean_member(req, "video", !no_webcam);
			/* Prepare the Janus API request to send the message to the plugin */
			JsonObject *msg = json_object_new();
			json_object_set_string_member(msg, "janus", "message");
			char tr[12];
			json_object_set_string_member(msg, "transaction", jamrtc_random_transaction(tr, sizeof(tr)));
			json_object_set_int_member(msg, "session_id", session_id);
			json_object_set_int_member(msg, "handle_id", pc->handle_id);
			json_object_set_object_member(msg, "body", req);
			char *text = jamrtc_json_to_string(msg);
			json_object_unref(msg);
			g_free(participant);
			/* Track the task */
			jamrtc_mutex_lock(&transactions_mutex);
			g_hash_table_insert(transactions, g_strdup(tr), pc);
			jamrtc_mutex_unlock(&transactions_mutex);
			/* Send the request via WebSockets */
			JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
			jamrtc_send_message(text);
		} else if(pc == local_instrument) {
			/* Create a GStreamer pipeline for the sendonly PeerConnection */
			jamrtc_prepare_pipeline(pc, FALSE, TRUE, FALSE);
		} else {
			/* Create a GStreamer pipeline for the recvonly subscription */
			if(jamrtc_prepare_pipeline(pc, TRUE, pc->audio, pc->video)) {
				/* Now send a "join" request to subscribe to this stream */
				JsonObject *req = json_object_new();
				json_object_set_string_member(req, "request", "join");
				json_object_set_string_member(req, "ptype", "subscriber");
				json_object_set_int_member(req, "room", room_id);
				json_object_set_int_member(req, "feed", pc->user_id);
				json_object_set_int_member(req, "private_id", private_id);
				/* Prepare the Janus API request to send the message to the plugin */
				JsonObject *msg = json_object_new();
				json_object_set_string_member(msg, "janus", "message");
				char tr[12];
				json_object_set_string_member(msg, "transaction", jamrtc_random_transaction(tr, sizeof(tr)));
				json_object_set_int_member(msg, "session_id", session_id);
				json_object_set_int_member(msg, "handle_id", pc->handle_id);
				json_object_set_object_member(msg, "body", req);
				char *text = jamrtc_json_to_string(msg);
				json_object_unref(msg);
				/* Send the request via WebSockets */
				JAMRTC_LOG(LOG_VERB, "Sending message: %s\n", text);
				jamrtc_send_message(text);
			}
		}
	} else if(json_object_has_member(object, "jsep")) {
		/* This message contains a JSEP SDP, which means it must be an offer or answer from Janus */
		JsonObject *child = json_object_get_object_member(object, "jsep");
		if(!json_object_has_member(child, "type")) {
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Received SDP without 'type'\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
			pc->state = JAMRTC_JANUS_API_ERROR;
			goto done;
		}
		gboolean offer = FALSE;
		const char *sdptype = json_object_get_string_member(child, "type");
		offer = !strcasecmp(sdptype, "offer");
		const char *text = json_object_get_string_member(child, "sdp");
		JAMRTC_LOG(LOG_INFO, "[%s][%s]  -- Received SDP %s\n",
			pc->display, pc->instrument ? pc->instrument : "chat", sdptype);
		JAMRTC_LOG(LOG_VERB, "%s\n", text);

		/* Check if there are any candidates in the SDP: we'll need to fake trickles in case */
		if(strstr(text, "candidate") != NULL) {
			int mlines = 0, i = 0;
			gchar **lines = g_strsplit(text, "\r\n", -1);
			gchar *line = NULL;
			while(lines[i] != NULL) {
				line = lines[i];
				if(strstr(line, "m=") == line) {
					/* New m-line */
					mlines++;
					if(mlines > 1)	/* We only need candidates from the first one */
						break;
				} else if(mlines == 1 && strstr(line, "a=candidate") != NULL) {
					/* Found a candidate, fake a trickle */
					line += 2;
					JAMRTC_LOG(LOG_VERB, "[%s][%s]  -- Found candidate: %s\n",
						pc->display, pc->instrument ? pc->instrument : "chat", line);
					g_signal_emit_by_name(pc->peerconnection, "add-ice-candidate", 0, line);
				}
				i++;
			}
			g_clear_pointer(&lines, g_strfreev);
		}

		/* Convert the SDP to something webrtcbin can digest */
		GstSDPMessage *sdp = NULL;
		int ret = gst_sdp_message_new(&sdp);
		if(ret != GST_SDP_OK) {
			/* Something went wrong */
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error initializing SDP object (%d)\n",
				pc->display, pc->instrument ? pc->instrument : "chat", ret);
		}
		ret = gst_sdp_message_parse_buffer((guint8 *)text, strlen(text), sdp);
		if(ret != GST_SDP_OK) {
			/* Something went wrong */
			JAMRTC_LOG(LOG_ERR, "[%s][%s] Error parsing SDP buffer (%d)\n",
				pc->display, pc->instrument ? pc->instrument : "chat", ret);
		}
		GstWebRTCSessionDescription *gst_sdp = gst_webrtc_session_description_new(
			offer ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER, sdp);

		/* Set remote description on our pipeline */
		GstPromise *promise = gst_promise_new();
		g_signal_emit_by_name(pc->peerconnection, "set-remote-description", gst_sdp, promise);
		gst_promise_interrupt(promise);
		gst_promise_unref(promise);

		pc->state = offer ? JAMRTC_JANUS_SDP_PREPARED : JAMRTC_JANUS_STARTED;
		if(offer && pc->remote) {
			/* We need to prepare an SDP answer */
			promise = gst_promise_new_with_change_func(jamrtc_sdp_available, pc, NULL);
			g_signal_emit_by_name(pc->peerconnection, "create-answer", NULL, promise);
			//~ gst_promise_interrupt(promise);
			//~ gst_promise_unref(promise);
		}
	} else if(json_object_has_member(object, "candidate")) {
		/* This is a trickle candidate */
		const char *candidate = NULL;
		gint sdpmlineindex = 0;
		/* Parse the candidate info */
		JsonObject *child = json_object_get_object_member(object, "candidate");
		if(child != NULL && json_object_has_member(child, "candidate")) {
			candidate = json_object_get_string_member(child, "candidate");
			sdpmlineindex = json_object_get_int_member(child, "sdpMLineIndex");
			JAMRTC_LOG(LOG_INFO, "[%s][%s] Received trickle candidate\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
			JAMRTC_LOG(LOG_VERB, "%s(%d)\n", candidate, sdpmlineindex);
			/* Add ice candidate sent by remote peer */
			g_signal_emit_by_name(pc->peerconnection, "add-ice-candidate", sdpmlineindex, candidate);
		}
	} else {
		/* Other event? Check if it's an error */
		if(json_object_has_member(object, "error")) {
			/* Janus API error */
			JsonObject *error = json_object_get_object_member(object, "error");
			JAMRTC_LOG(LOG_WARN, "[%s][%s] Got a Janus API error: %ld (%s)\n",
				pc->display, pc->instrument ? pc->instrument : "chat",
				json_object_get_int_member(error, "code"),
				json_object_get_string_member(error, "reason"));
		} else if(json_object_has_member(object, "plugindata")) {
			/* Response or event from the plugin */
			JsonObject *plugindata = json_object_get_object_member(object, "plugindata");
			JsonObject *data = json_object_get_object_member(plugindata, "data");
			if(json_object_has_member(data, "error")) {
				/* VideoRoom error */
				JAMRTC_LOG(LOG_WARN, "[%s][%s] Got a VideoRoom error: %ld (%s)\n",
					pc->display, pc->instrument ? pc->instrument : "chat",
					json_object_get_int_member(data, "error_code"),
					json_object_get_string_member(data, "error"));
				jamrtc_cleanup("ERROR: VideoRoom error", JAMRTC_JANUS_API_ERROR);
				goto done;
			}
			/* Check if it's an event we should care about */
			const char *event = json_object_get_string_member(data, "videoroom");
			if(event != NULL && !strcasecmp(event, "joined")) {
				/* This publisher handle just successfully joined the VideoRoom */
				guint64 user_id = json_object_get_int_member(data, "id");
				if(pc == local_micwebcam) {
					local_micwebcam->user_id = user_id;
					private_id = json_object_get_int_member(data, "private_id");
					/* Update the UI */
					jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_ADD_PARTICIPANT,
						NULL, FALSE, NULL);
					g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
					/* Notify the application layer we're in */
					cb->joined_room();
				} else if(pc == local_instrument) {
					local_instrument->user_id = user_id;
				}
			}
			/* Check if there's news on attendees and/or publishers */
			//~ JAMRTC_LOG(LOG_WARN, "  -- TBD: %s\n", text);
			if(pc == local_micwebcam) {
				if(json_object_has_member(data, "joining")) {
					/* Parse the new VideoRoom participant */
					JsonObject *joining = json_object_get_object_member(data, "joining");
					jamrtc_parse_participant(joining, FALSE);
				}
				if(json_object_has_member(data, "attendees")) {
					/* Parse the attendees list */
					JsonArray *attendees = json_object_get_array_member(data, "attendees");
					jamrtc_parse_participants(attendees, FALSE);
				}
				if(json_object_has_member(data, "publishers")) {
					/* Parse the publishers list */
					JsonArray *publishers = json_object_get_array_member(data, "publishers");
					jamrtc_parse_participants(publishers, TRUE);
				}
				if(json_object_has_member(data, "leaving")) {
					/* A VideoRoom participant left, get rid of the PeerConnection instance */
					guint64 user_id = json_object_get_int_member(data, "leaving");
					jamrtc_mutex_lock(&participants_mutex);
					jamrtc_webrtc_participant *participant = g_hash_table_lookup(participants_byid, &user_id);
					if(participant != NULL) {
						if(participant->user_id == user_id) {
							participant->user_id = 0;
							jamrtc_webrtc_pc *oldpc = participant->micwebcam;
							if(oldpc != NULL) {
								/* Update the UI */
								jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_REMOVE_STREAM,
									participant->micwebcam, FALSE, NULL);
								g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
								/* Remove the stream */
								participant->micwebcam = NULL;
								if(oldpc->handle_id != 0)
									g_hash_table_remove(peerconnections, &oldpc->handle_id);
								/* Notify the application */
								cb->stream_stopped(oldpc->uuid, oldpc->display, NULL);
								jamrtc_webrtc_pc_destroy(oldpc);
							}
						} else if(participant->instrument_user_id == user_id) {
							participant->instrument_user_id = 0;
							jamrtc_webrtc_pc *oldpc = participant->instrument;
							if(oldpc != NULL) {
								/* Update the UI */
								jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_REMOVE_STREAM,
									participant->instrument, FALSE, NULL);
								g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
								/* Remove the stream */
								participant->instrument = NULL;
								if(oldpc->handle_id != 0)
									g_hash_table_remove(peerconnections, &oldpc->handle_id);
								/* Notify the application */
								cb->stream_stopped(oldpc->uuid, oldpc->display, oldpc->instrument);
								jamrtc_webrtc_pc_destroy(oldpc);
							}
						}
						g_hash_table_remove(participants_byid, &user_id);
					}
					if(participant && participant->user_id == 0 && participant->instrument_user_id == 0) {
						/* No streams left for this participant */
						cb->participant_left(participant->uuid, participant->display);
						/* Update the UI */
						jamrtc_video_message *msg = jamrtc_video_message_create(JAMRTC_ACTION_REMOVE_PARTICIPANT,
							participant, FALSE, NULL);
						g_main_context_invoke(NULL, jamrtc_video_message_handle, msg);
						/* Remove the participant */
						g_hash_table_remove(participants, participant->uuid);
						g_hash_table_remove(participants_byslot, GUINT_TO_POINTER(participant->slot));
					}
					jamrtc_mutex_unlock(&participants_mutex);
				}
			}
		} else if(!strcasecmp(response, "webrtcup")) {
			/* PeerConnection is up */
			JAMRTC_LOG(LOG_INFO, "[%s][%s] PeerConnection with Janus established\n",
				pc->display, pc->instrument ? pc->instrument : "chat");
		} else if(!strcasecmp(response, "media")) {
			/* Notification about media reception */
			const char *type = json_object_get_string_member(object, "type");
			gboolean receiving = json_object_get_boolean_member(object, "receiving");
			if(receiving) {
				JAMRTC_LOG(LOG_INFO, "[%s][%s] Janus is receiving %s from us\n",
					pc->display, pc->instrument ? pc->instrument : "chat", type);
			} else {
				JAMRTC_LOG(LOG_WARN, "[%s][%s] Janus hasn't received %s from us for a while...\n",
					pc->display, pc->instrument ? pc->instrument : "chat", type);
			}
		} else if(!strcasecmp(response, "hangup")) {
			/* PeerConnection is down, wrap up */
			JAMRTC_LOG(LOG_INFO, "[%s][%s] PeerConnection with Janus is down (%s)\n",
				pc->display, pc->instrument ? pc->instrument : "chat",
				json_object_get_string_member(object, "reason"));
		}
	}

done:
	g_object_unref(parser);
	if(pc != NULL && remove_transaction) {
		jamrtc_mutex_lock(&transactions_mutex);
		g_hash_table_remove(transactions, transaction);
		jamrtc_mutex_unlock(&transactions_mutex);
	}
}

/* Handler for all libwebsockets events */
static int jamrtc_ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
	switch(reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED: {
			/* Prepare the session */
			if(ws_client == NULL)
				ws_client = (jamrtc_ws_client *)user;
			ws_client->wsi = wsi;
			ws_client->buffer = NULL;
			ws_client->buflen = 0;
			ws_client->bufpending = 0;
			ws_client->bufoffset = 0;
			jamrtc_mutex_init(&ws_client->mutex);

			state = JAMRTC_JANUS_CONNECTED;
			JAMRTC_LOG(LOG_INFO, "  -- Connected to Janus\n");
			/* Let's create a Janus session now */
			jamrtc_create_session();
			return 0;
		}
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
			state = JAMRTC_JANUS_DISCONNECTED;
			jamrtc_cleanup("Error connecting to backend", 0);
			cb->server_disconnected();
			return 1;
		}
		case LWS_CALLBACK_CLIENT_RECEIVE: {
			/* Incoming data */
			if(ws_client == NULL) {
				JAMRTC_LOG(LOG_ERR, "Invalid WebSocket client instance...\n");
				return 1;
			}
			/* Is this a new message, or part of a fragmented one? */
			const size_t remaining = lws_remaining_packet_payload(wsi);
			if(ws_client->incoming == NULL) {
				JAMRTC_LOG(LOG_HUGE, "First fragment: %zu bytes, %zu remaining\n",
					len, remaining);
				ws_client->incoming = g_malloc(len+1);
				memcpy(ws_client->incoming, in, len);
				ws_client->incoming[len] = '\0';
				JAMRTC_LOG(LOG_HUGE, "%s\n", ws_client->incoming);
			} else {
				size_t offset = strlen(ws_client->incoming);
				JAMRTC_LOG(LOG_HUGE, "Appending fragment: offset %zu, %zu bytes, %zu remaining\n",
					offset, len, remaining);
				ws_client->incoming = g_realloc(ws_client->incoming, offset+len+1);
				memcpy(ws_client->incoming+offset, in, len);
				ws_client->incoming[offset+len] = '\0';
				JAMRTC_LOG(LOG_HUGE, "%s\n", ws_client->incoming+offset);
			}
			if(remaining > 0 || !lws_is_final_fragment(wsi)) {
				/* Still waiting for some more fragments */
				JAMRTC_LOG(LOG_HUGE, "Waiting for more fragments\n");
				return 0;
			}
			JAMRTC_LOG(LOG_HUGE, "Done, parsing message: %zu bytes\n", strlen(ws_client->incoming));
			/* If we got here, the message is complete: process the message */
			jamrtc_server_message(ws_client->incoming);
			g_free(ws_client->incoming);
			ws_client->incoming = NULL;
			return 0;
		}
#if (LWS_LIBRARY_VERSION_MAJOR >= 3)
		/* On libwebsockets >= 3.x, we use this event to mark connections as writable in the event loop */
		case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
			if(ws_client != NULL && ws_client->wsi != NULL)
				lws_callback_on_writable(ws_client->wsi);
			return 0;
		}
#endif
		case LWS_CALLBACK_CLIENT_WRITEABLE: {
			if(ws_client == NULL || ws_client->wsi == NULL) {
				JAMRTC_LOG(LOG_ERR, "Invalid WebSocket client instance...\n");
				return -1;
			}
			if(!g_atomic_int_get(&stopping)) {
				jamrtc_mutex_lock(&ws_client->mutex);
				/* Check if we have a pending/partial write to complete first */
				if(ws_client->buffer && ws_client->bufpending > 0 && ws_client->bufoffset > 0
						&& !g_atomic_int_get(&stopping)) {
					JAMRTC_LOG(LOG_VERB, "Completing pending WebSocket write (still need to write last %d bytes)...\n",
						ws_client->bufpending);
					int sent = lws_write(wsi, ws_client->buffer + ws_client->bufoffset, ws_client->bufpending, LWS_WRITE_TEXT);
					JAMRTC_LOG(LOG_VERB, "  -- Sent %d/%d bytes\n", sent, ws_client->bufpending);
					if(sent > -1 && sent < ws_client->bufpending) {
						/* We still couldn't send everything that was left, we'll try and complete this in the next round */
						ws_client->bufpending -= sent;
						ws_client->bufoffset += sent;
					} else {
						/* Clear the pending/partial write queue */
						ws_client->bufpending = 0;
						ws_client->bufoffset = 0;
					}
					/* Done for this round, check the next response/notification later */
					lws_callback_on_writable(wsi);
					jamrtc_mutex_unlock(&ws_client->mutex);
					return 0;
				}
				/* Shoot all the pending messages */
				char *event = g_async_queue_try_pop(messages);
				if(event && !g_atomic_int_get(&stopping)) {
					/* Gotcha! */
					int buflen = LWS_PRE + strlen(event);
					if(ws_client->buffer == NULL) {
						/* Let's allocate a shared buffer */
						JAMRTC_LOG(LOG_VERB, "Allocating %d bytes (event is %zu bytes)\n", buflen, strlen(event));
						ws_client->buflen = buflen;
						ws_client->buffer = g_malloc0(buflen);
					} else if(buflen > ws_client->buflen) {
						/* We need a larger shared buffer */
						JAMRTC_LOG(LOG_VERB, "Re-allocating to %d bytes (was %d, event is %zu bytes)\n",
							buflen, ws_client->buflen, strlen(event));
						ws_client->buflen = buflen;
						ws_client->buffer = g_realloc(ws_client->buffer, buflen);
					}
					memcpy(ws_client->buffer + LWS_PRE, event, strlen(event));
					JAMRTC_LOG(LOG_VERB, "Sending WebSocket message (%zu bytes)...\n", strlen(event));
					int sent = lws_write(wsi, ws_client->buffer + LWS_PRE, strlen(event), LWS_WRITE_TEXT);
					JAMRTC_LOG(LOG_VERB, "  -- Sent %d/%zu bytes\n", sent, strlen(event));
					if(sent > -1 && sent < (int)strlen(event)) {
						/* We couldn't send everything in a single write, we'll complete this in the next round */
						ws_client->bufpending = strlen(event) - sent;
						ws_client->bufoffset = LWS_PRE + sent;
						JAMRTC_LOG(LOG_VERB, "  -- Couldn't write all bytes (%d missing), setting offset %d\n",
							ws_client->bufpending, ws_client->bufoffset);
					}
					/* We can get rid of the message */
					g_free(event);
					/* Done for this round, check the next response/notification later */
					lws_callback_on_writable(wsi);
					jamrtc_mutex_unlock(&ws_client->mutex);
					return 0;
				}
				jamrtc_mutex_unlock(&ws_client->mutex);
			}
			return 0;
		}
#if (LWS_LIBRARY_VERSION_MAJOR >= 3)
		case LWS_CALLBACK_CLIENT_CLOSED: {
#else
		case LWS_CALLBACK_CLOSED: {
#endif
			JAMRTC_LOG(LOG_INFO, "Janus connection closed\n");
			if(ws_client != NULL) {
				/* Cleanup */
				jamrtc_mutex_lock(&ws_client->mutex);
				JAMRTC_LOG(LOG_INFO, "Destroying Janus client\n");
				ws_client->wsi = NULL;
				/* Free the shared buffers */
				g_free(ws_client->buffer);
				ws_client->buffer = NULL;
				ws_client->buflen = 0;
				ws_client->bufpending = 0;
				ws_client->bufoffset = 0;
				jamrtc_mutex_unlock(&ws_client->mutex);
			}
			ws_client = NULL;
			wsi = NULL;
			state = JAMRTC_JANUS_DISCONNECTED;
			jamrtc_cleanup("Janus connection closed", 0);
			cb->server_disconnected();
			return 0;
		}
		default:
			//~ if(wsi)
				//~ JAMRTC_LOG(LOG_HUGE, "%d (%s)\n", reason, jamrtc_ws_reason_string(reason));
			break;
	}
	return 0;
}
