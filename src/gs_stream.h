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

// Timestamps, for keeping video in step with the audio. gs_stream_write_at gives the presentation time
// of its first frame (on any timeline, in seconds); later writes continue from it unless they say
// otherwise. gs_stream_clock is the presentation time being heard now: frames played, less those still
// waiting for the device (gs_mix_latency_frames), and advanced smoothly between the mixer's calls.
// It is < 0 until timestamped audio is heard, and stands still while the stream is paused or starved.
bool gs_stream_write_at(gs_stream *s, const float *lr, int frames, double pts);
double gs_stream_clock(gs_stream *s);
void gs_stream_flush(gs_stream *s);  // drops the audio waiting to be played; what is written next follows on
// With rebuffering on, a stream that runs dry waits for its prebuffer again before playing on, rather
// than playing each scrap as it arrives.
void gs_stream_rebuffer(gs_stream *s, bool on);
bool gs_stream_starved(gs_stream *s);  // it played, ran dry and is waiting for its prebuffer again
