#include "gs_opus.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

struct gs_opus { OpusDecoder *dec; };

bool gs_opus_head(const uint8_t *h, size_t len, int *channels, int *preskip) {
    if (len < 19 || memcmp(h, "OpusHead", 8)) return false;
    *channels = h[9];
    *preskip = h[10] | h[11] << 8;
    return true;
}

gs_opus *gs_opus_new(int stream_channels) {
    (void)stream_channels;  // libopus mixes any stream to the channels asked for
    int err;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    if (!dec) return NULL;
    gs_opus *o = malloc(sizeof *o);
    o->dec = dec;
    return o;
}

void gs_opus_free(gs_opus *o) {
    if (!o) return;
    opus_decoder_destroy(o->dec);
    free(o);
}

int gs_opus_decode(gs_opus *o, const uint8_t *packet, size_t len, float *lr, int max_frames) {
    return opus_decode_float(o->dec, packet, (opus_int32)len, lr, max_frames, 0);
}
