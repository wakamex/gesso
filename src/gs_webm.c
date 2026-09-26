#include "gs_webm.h"

#include <stdlib.h>
#include <string.h>

enum {
    ID_EBML = 0x1A45DFA3, ID_SEGMENT = 0x18538067, ID_INFO = 0x1549A966, ID_TIMECODE_SCALE = 0x2AD7B1,
    ID_DURATION = 0x4489, ID_TRACKS = 0x1654AE6B, ID_TRACK_ENTRY = 0xAE, ID_TRACK_NUMBER = 0xD7,
    ID_TRACK_TYPE = 0x83, ID_CODEC_ID = 0x86, ID_CODEC_PRIVATE = 0x63A2, ID_AUDIO = 0xE1,
    ID_SAMPLING = 0xB5, ID_CHANNELS = 0x9F, ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7,
    ID_SIMPLE_BLOCK = 0xA3, ID_BLOCK_GROUP = 0xA0, ID_BLOCK = 0xA1,
};

struct gs_webm {
    gs_webm_frame_fn *on_frame;
    void *user;
    uint8_t *buf;
    size_t len, cap;
    uint64_t skip;  // bytes of an uninteresting element still to discard
    bool checked, bad;
    // The track being read, and the chosen audio track.
    struct { uint64_t number, type; char codec[32]; uint8_t *priv; size_t npriv; double rate; int channels; } cur, audio;
    bool have_audio;
    double scale, duration;  // ns per timecode unit; duration in timecode units
    int64_t cluster_time;
};

gs_webm *gs_webm_new(gs_webm_frame_fn *on_frame, void *user) {
    gs_webm *w = calloc(1, sizeof *w);
    w->on_frame = on_frame, w->user = user, w->scale = 1000000;
    return w;
}

void gs_webm_free(gs_webm *w) {
    if (!w) return;
    free(w->buf), free(w->cur.priv), free(w->audio.priv);
    free(w);
}

// An EBML variable-length integer; `strip` removes the length marker (sizes), keeps it (IDs).
static int vint(const uint8_t *p, size_t n, uint64_t *v, bool strip, bool *unknown) {
    if (!n || !p[0]) return 0;
    int len = 1;
    while (!(p[0] & (0x80 >> (len - 1)))) len++;
    if ((size_t)len > n) return 0;
    uint64_t x = strip ? p[0] & (0xFF >> len) : p[0];
    bool all_ones = strip && x == (0xFFu >> len);
    for (int i = 1; i < len; i++) x = x << 8 | p[i], all_ones &= p[i] == 0xFF;
    if (unknown) *unknown = all_ones;
    *v = x;
    return len;
}

static uint64_t uint_be(const uint8_t *p, size_t n) {
    uint64_t x = 0;
    for (size_t i = 0; i < n && i < 8; i++) x = x << 8 | p[i];
    return x;
}

static double float_be(const uint8_t *p, size_t n) {
    uint64_t x = uint_be(p, n);
    if (n == 4) { uint32_t u = (uint32_t)x; float f; memcpy(&f, &u, 4); return f; }
    double d;
    memcpy(&d, &x, 8);
    return n == 8 ? d : 0;
}

static bool is_master(uint64_t id) {
    return id == ID_SEGMENT || id == ID_INFO || id == ID_TRACKS || id == ID_TRACK_ENTRY || id == ID_AUDIO || id == ID_CLUSTER || id == ID_BLOCK_GROUP;
}

static bool wanted(uint64_t id) {
    return id == ID_TIMECODE_SCALE || id == ID_DURATION || id == ID_TRACK_NUMBER || id == ID_TRACK_TYPE || id == ID_CODEC_ID ||
           id == ID_CODEC_PRIVATE || id == ID_SAMPLING || id == ID_CHANNELS || id == ID_TIMECODE || id == ID_SIMPLE_BLOCK || id == ID_BLOCK;
}

static void end_track(gs_webm *w) {
    if (!w->have_audio && w->cur.type == 2 && w->cur.codec[0]) {  // the first audio track
        w->audio = w->cur;
        w->cur.priv = NULL;
        w->have_audio = true;
    }
    free(w->cur.priv);
    memset(&w->cur, 0, sizeof w->cur);
}

static void block(gs_webm *w, const uint8_t *p, size_t n) {
    uint64_t track;
    int k = vint(p, n, &track, true, NULL);
    if (!k || n < (size_t)k + 3 || !w->have_audio || track != w->audio.number) return;
    int16_t rel = (int16_t)(p[k] << 8 | p[k + 1]);
    uint8_t flags = p[k + 2];
    p += k + 3, n -= (size_t)k + 3;
    double seconds = (w->cluster_time + rel) * w->scale / 1e9;
    int lacing = flags >> 1 & 3;
    if (lacing == 0) w->on_frame(w->user, p, n, seconds);
    else if (lacing == 2 && n) {  // fixed-size lacing
        int frames = p[0] + 1;
        size_t each = (n - 1) / (size_t)frames;
        for (int i = 0; i < frames; i++) w->on_frame(w->user, p + 1 + (size_t)i * each, each, seconds);
    }
}

static void element(gs_webm *w, uint64_t id, const uint8_t *p, size_t n) {
    switch (id) {
    case ID_TIMECODE_SCALE: w->scale = (double)uint_be(p, n); break;
    case ID_DURATION: w->duration = float_be(p, n); break;
    case ID_TRACK_NUMBER: w->cur.number = uint_be(p, n); break;
    case ID_TRACK_TYPE: w->cur.type = uint_be(p, n); break;
    case ID_CODEC_ID: memcpy(w->cur.codec, p, n < 31 ? n : 31), w->cur.codec[n < 31 ? n : 31] = 0; break;
    case ID_CODEC_PRIVATE: free(w->cur.priv), w->cur.priv = malloc(n ? n : 1), memcpy(w->cur.priv, p, n), w->cur.npriv = n; break;
    case ID_SAMPLING: w->cur.rate = float_be(p, n); break;
    case ID_CHANNELS: w->cur.channels = (int)uint_be(p, n); break;
    case ID_TIMECODE: w->cluster_time = (int64_t)uint_be(p, n); break;
    case ID_SIMPLE_BLOCK: case ID_BLOCK: block(w, p, n); break;
    }
}

bool gs_webm_feed(gs_webm *w, const void *data, size_t len) {
    if (w->bad) return false;
    const uint8_t *in = data;
    if (w->skip) {  // still discarding an element we do not need
        size_t k = w->skip < len ? (size_t)w->skip : len;
        w->skip -= k, in += k, len -= k;
    }
    if (w->len + len > w->cap) w->cap = (w->len + len) * 2 + 4096, w->buf = realloc(w->buf, w->cap);
    memcpy(w->buf + w->len, in, len);
    w->len += len;
    size_t at = 0;
    for (;;) {
        if (w->skip) {
            size_t k = w->skip < w->len - at ? (size_t)w->skip : w->len - at;
            w->skip -= k, at += k;
            if (w->skip) break;
        }
        uint64_t id, size;
        bool unknown = false;
        int a = vint(w->buf + at, w->len - at, &id, false, NULL);
        if (!a) break;
        int b = vint(w->buf + at + a, w->len - at - a, &size, true, &unknown);
        if (!b) break;
        if (!w->checked) {
            w->checked = true;
            if (id != ID_EBML) { w->bad = true; return false; }
        }
        size_t head = (size_t)(a + b);
        if (id == ID_TRACK_ENTRY && w->cur.codec[0]) end_track(w);  // a new entry: the last one is complete
        if (id == ID_CLUSTER || id == ID_TRACKS) end_track(w);
        if (is_master(id)) { at += head; continue; }  // step inside
        if (!wanted(id) || unknown) { at += head; w->skip = unknown ? 0 : size; continue; }
        if (w->len - at - head < size) {
            if (size > (64u << 20)) { w->bad = true; return false; }  // not an audio stream we can buffer
            break;
        }
        element(w, id, w->buf + at + head, (size_t)size);
        at += head + (size_t)size;
    }
    memmove(w->buf, w->buf + at, w->len - at);
    w->len -= at;
    return true;
}

const char *gs_webm_codec(const gs_webm *w) { return w->have_audio ? w->audio.codec : NULL; }
const uint8_t *gs_webm_codec_private(const gs_webm *w, size_t *len) { *len = w->audio.npriv; return w->audio.priv; }
int gs_webm_channels(const gs_webm *w) { return w->audio.channels ? w->audio.channels : 2; }
double gs_webm_rate(const gs_webm *w) { return w->audio.rate ? w->audio.rate : 48000; }
double gs_webm_duration(const gs_webm *w) { return w->duration * w->scale / 1e9; }
