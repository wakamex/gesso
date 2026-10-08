// What gesso's demuxers (gs_ts for MPEG transport stream, gs_mp4 for fragmented MP4) hand out: H.264
// access units in Annex B form and raw AAC frames, each with its presentation time, and the AAC
// configuration they need.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { GS_MEDIA_H264, GS_MEDIA_AAC } gs_media_kind;

typedef struct {
    int rate, channels;
    uint8_t asc[16];  // the AudioSpecificConfig (ISO 14496-3) a decoder is opened with
    int asc_len;
} gs_aac_config;

typedef struct {
    gs_media_kind kind;
    const uint8_t *data;
    size_t len;
    double pts;                // seconds, on the stream's own timeline (unwrapped)
    bool key;                  // an H.264 access unit a decoder can start from
    const gs_aac_config *aac;  // for AAC frames
} gs_media_frame;

typedef void gs_media_fn(void *user, const gs_media_frame *f);

// An AudioSpecificConfig for AAC-LC at a sample rate and channel count; false for a rate it cannot name.
bool gs_aac_config_make(gs_aac_config *c, int rate, int channels);
