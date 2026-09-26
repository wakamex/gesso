// A streaming audio source: a decoder (on any thread) writes stereo frames in, the mixer plays them
// out. Writing blocks while the buffer is full, so a slow listener slows the decoder rather than
// growing memory. Playback starts once `prebuffer` seconds are in (or the stream has ended), and an
// underrun plays silence until more arrives. gs_stream_render is a gs_mix_fn.
#pragma once
#include <stdbool.h>

typedef struct gs_stream gs_stream;

gs_stream *gs_stream_new(int rate, double buffer_seconds, double prebuffer_seconds);
void gs_stream_free(gs_stream *s);

bool gs_stream_write(gs_stream *s, const float *lr, int frames);  // false once stopped
void gs_stream_end(gs_stream *s);   // the writer is done
void gs_stream_stop(gs_stream *s);  // unblocks and refuses the writer; plays nothing more

void gs_stream_render(void *s, float *lr, int frames);  // adds
void gs_stream_pause(gs_stream *s, bool paused);
bool gs_stream_paused(const gs_stream *s);
double gs_stream_position(const gs_stream *s);  // seconds played
double gs_stream_buffered(const gs_stream *s);  // seconds waiting
bool gs_stream_finished(const gs_stream *s);    // ended and played out
