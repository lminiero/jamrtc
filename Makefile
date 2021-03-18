CC = gcc
STUFF = $(shell pkg-config --cflags gdk-3.0 gtk+-3.0 "gstreamer-webrtc-1.0 >= 1.16" "gstreamer-sdp-1.0 >= 1.16" gstreamer-video-1.0 libwebsockets json-glib-1.0) -D_GNU_SOURCE
STUFF_LIBS = $(shell pkg-config --libs gdk-3.0 gtk+-3.0 "gstreamer-webrtc-1.0 >= 1.16" "gstreamer-sdp-1.0 >= 1.16" gstreamer-video-1.0 libwebsockets json-glib-1.0)
OPTS = -Wall -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wunused #-Werror #-O2
GDB = -g -ggdb
OBJS = src/jamrtc.o src/webrtc.o

all: jamrtc

%.o: %.c
	$(CC) $(ASAN) $(STUFF) -fPIC $(GDB) -c $< -o $@ $(OPTS)

jamrtc: $(OBJS)
	$(CC) $(GDB) -o JamRTC $(OBJS) $(ASAN_LIBS) $(STUFF_LIBS)

clean:
	rm -f JamRTC src/*.o
