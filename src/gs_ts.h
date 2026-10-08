// An MPEG transport stream demuxer for HLS segments: bytes in (whole segments or any pieces of them),
// H.264 access units and AAC frames out with their presentation times (gs_media.h). It follows the
// programme map, unwraps the 33-bit timestamps, and skips other streams such as ID3 metadata.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gs_media.h"

typedef struct gs_ts gs_ts;

gs_ts *gs_ts_new(gs_media_fn *fn, void *user);
void gs_ts_free(gs_ts *t);
bool gs_ts_feed(gs_ts *t, const uint8_t *data, size_t len);  // false once the bytes are not a transport stream
void gs_ts_end(gs_ts *t);    // hands out the frames still being assembled (at the end of a segment)
void gs_ts_reset(gs_ts *t);  // forgets the programme map and timestamps, for a stream that restarts
