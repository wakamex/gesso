// AAC decoding (libavcodec) to stereo floats at a chosen rate, with the same shape as gs_opus: frames
// in, interleaved left and right samples out. Streams at another rate are resampled (SDL's audio
// streams), so a 44.1 kHz stream plays in a 48 kHz mix. Needs gesso built with -Dvideo.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gs_media.h"

typedef struct gs_aac gs_aac;

gs_aac *gs_aac_new(const gs_aac_config *c, int out_rate);
void gs_aac_free(gs_aac *a);
// Decodes one raw AAC frame; returns the stereo frames written to lr (up to max_frames), or < 0
// after an error the decoder cannot get past. A frame the decoder rejects gives 0.
int gs_aac_decode(gs_aac *a, const uint8_t *frame, size_t len, float *lr, int max_frames);
void gs_aac_flush(gs_aac *a);  // forgets the decoder's state (at a discontinuity)
