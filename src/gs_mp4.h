// A fragmented MP4 demuxer for HLS segments (CMAF): the initialization segment's tracks first, then
// each media segment's samples, handed out as gs_ts does (gs_media.h): H.264 rewritten from MP4's
// length-prefixed form to Annex B, with the parameter sets before each keyframe, and raw AAC frames.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gs_media.h"

typedef struct gs_mp4 gs_mp4;

gs_mp4 *gs_mp4_new(gs_media_fn *fn, void *user);
void gs_mp4_free(gs_mp4 *m);
bool gs_mp4_init(gs_mp4 *m, const uint8_t *data, size_t len);     // an initialization segment; false when unusable
bool gs_mp4_segment(gs_mp4 *m, const uint8_t *data, size_t len);  // a whole media segment

// Where each sample of track `track` lies in a fragmented MP4 held whole (a moov whose sample table is
// empty, then moof and mdat pairs), in decoding order: its bytes, its decoding time and its
// presentation offset in the track's timescale, its duration, and whether decoding can start at it.
// The defaults come from the moov's trex. Returns how many, with *out to free(); 0 for none.
typedef struct { uint64_t at, decode; uint32_t size, duration; int64_t offset; bool sync; } gs_mp4_sample;
int gs_mp4_samples(const uint8_t *data, size_t len, uint32_t track, gs_mp4_sample **out);
