// Opus decoding (libopus) for streams: packets in, stereo floats at 48 kHz out.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gs_opus gs_opus;

// From an OpusHead (as in Ogg or a WebM track's codec data): its channels and pre-skip. False if not one.
bool gs_opus_head(const uint8_t *head, size_t len, int *channels, int *preskip);
gs_opus *gs_opus_new(int stream_channels);  // decodes to stereo whatever the stream has
void gs_opus_free(gs_opus *o);
int gs_opus_decode(gs_opus *o, const uint8_t *packet, size_t len, float *lr, int max_frames);  // frames, or < 0
