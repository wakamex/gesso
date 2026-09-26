// A streaming WebM (Matroska) reader for audio: feed it bytes as they arrive (from a pipe or a
// download) and it hands back each frame of the first audio track, with the track's codec and its
// setup data (for Opus, the OpusHead). Unknown-size elements, as in live and piped streams, are fine.
// Frames without lacing and with fixed-size lacing are read; other lacing is skipped.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gs_webm gs_webm;
typedef void gs_webm_frame_fn(void *user, const uint8_t *data, size_t len, double seconds);

gs_webm *gs_webm_new(gs_webm_frame_fn *on_frame, void *user);
void gs_webm_free(gs_webm *w);
bool gs_webm_feed(gs_webm *w, const void *data, size_t len);  // false if the stream is not WebM

// Known once the track headers have been read (codec NULL until then).
const char *gs_webm_codec(const gs_webm *w);  // such as "A_OPUS"
const uint8_t *gs_webm_codec_private(const gs_webm *w, size_t *len);
int gs_webm_channels(const gs_webm *w);
double gs_webm_rate(const gs_webm *w);
double gs_webm_duration(const gs_webm *w);  // seconds, 0 if not given
