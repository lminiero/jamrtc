/*
 * JamRTC -- Jam sessions on Janus!
 *
 * Ugly prototype, just to use as a proof of concept
 *
 * Developed by Lorenzo Miniero: lorenzo@meetecho.com
 * License: GPLv3
 *
 */

#ifndef JAMRTC_ICE_H
#define JAMRTC_ICE_H

/* GTK */
#include <gtk/gtk.h>

/* GLib */
#include <glib.h>


/* Callbacks */
typedef struct jamrtc_callbacks {
	void (* const server_connected)(void);
	void (* const server_disconnected)(void);
	void (* const joined_room)(void);
	void (* const participant_joined)(const char *uuid, const char *display);
	void (* const stream_started)(const char *uuid, const char *display, const char *instrument,
		gboolean has_audio, gboolean has_video);
	void (* const stream_stopped)(const char *uuid, const char *display, const char *instrument);
	void (* const participant_left)(const char *uuid, const char *display);
} jamrtc_callbacks;


/* Janus stack initialization */
int jamrtc_webrtc_init(const jamrtc_callbacks *callbacks, GtkBuilder *builder, GMainLoop *mainloop,
	const char *ws, const char *stun, const char *turn, const char *src_opts, guint latency, gboolean no_jack);
/* Janus stack cleanup */
void jamrtc_webrtc_cleanup(void);

/* Join the room as a participant */
void jamrtc_join_room(guint64 room_id, const char *display);
/* Publish mic/webcam for the chat part */
void jamrtc_webrtc_publish_micwebcam(gboolean no_mic, gboolean no_webcam, const char *video_device);
/* Publish the instrument */
void jamrtc_webrtc_publish_instrument(const char *instrument, gboolean stereo);
/* Subscribe to a remote stream */
int jamrtc_webrtc_subscribe(const char *uuid, gboolean instrument);


#endif
