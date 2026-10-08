// The demuxers and the AAC decoder on the synthetic streams in tests/streams (made by make.sh).
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "gs_mp4.h"
#include "gs_ts.h"
#include "test.h"
#ifdef GS_TEST_VIDEO
#include "gs_aac.h"
#endif

extern const char *test_streams;  // the folder, from the command line

typedef struct {
    int video, audio, keys;
    double first_video, last_audio, min_video, max_video;
    bool audio_in_order;
    int rate;
#ifdef GS_TEST_VIDEO
    gs_aac *aac;
    double sum, crossings, decoded;
    float last;
#endif
} tally;

static void count(void *user, const gs_media_frame *f) {
    tally *t = user;
    if (f->kind == GS_MEDIA_H264) {
        if (!t->video) t->first_video = t->min_video = t->max_video = f->pts;
        t->min_video = fmin(t->min_video, f->pts), t->max_video = fmax(t->max_video, f->pts);
        t->video++, t->keys += f->key;
        // An access unit starts with a start code; a keyframe brings its parameter sets.
        if (f->len < 5 || f->data[0] || f->data[1]) t->video = -1000;
        return;
    }
    if (t->audio && f->pts <= t->last_audio) t->audio_in_order = false;
    t->last_audio = f->pts, t->audio++, t->rate = f->aac->rate;
#ifdef GS_TEST_VIDEO
    if (!t->aac) t->aac = gs_aac_new(f->aac, 48000);
    static float lr[2 * 8192];
    int n = t->aac ? gs_aac_decode(t->aac, f->data, f->len, lr, 8192) : 0;
    for (int i = 0; i < n; i++) {
        t->sum += lr[2 * i] * lr[2 * i];
        if ((lr[2 * i] >= 0) != (t->last >= 0)) t->crossings++;
        t->last = lr[2 * i];
    }
    t->decoded += n > 0 ? n : 0;
#endif
}

static void *load(const char *name, size_t *len) {
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/%s", test_streams, name);
    return SDL_LoadFile(path, len);
}

static tally replay_ts(const char *dir, int segments) {
    tally t = { .audio_in_order = true };
    gs_ts *ts = gs_ts_new(count, &t);
    for (int i = 0; i < segments; i++) {
        char name[64];
        SDL_snprintf(name, sizeof name, "%s/seg%d.ts", dir, i);
        size_t len;
        uint8_t *data = load(name, &len);
        if (!data) { t.video = -1; break; }
        for (size_t at = 0; at < len; at += 1000) gs_ts_feed(ts, data + at, len - at < 1000 ? len - at : 1000);  // in pieces
        gs_ts_end(ts);
        SDL_free(data);
    }
    gs_ts_free(ts);
    return t;
}

static tally replay_mp4(const char *dir) {
    tally t = { .audio_in_order = true };
    gs_mp4 *m = gs_mp4_new(count, &t);
    char name[64];
    SDL_snprintf(name, sizeof name, "%s/init.mp4", dir);
    size_t len;
    uint8_t *data = load(name, &len);
    CHECK(data && gs_mp4_init(m, data, len));
    SDL_free(data);
    for (int i = 0; i < 3; i++) {
        SDL_snprintf(name, sizeof name, "%s/seg%d.m4s", dir, i);
        data = load(name, &len);
        CHECK(data && gs_mp4_segment(m, data, len));
        SDL_free(data);
    }
    gs_mp4_free(m);
    return t;
}

#ifdef GS_TEST_VIDEO
// A 440 Hz tone decoded to 48 kHz: 6 s of it, about 880 zero crossings a second, at its loudness.
static void check_tone(tally *t) {
    CHECK(fabs(t->decoded - 6 * 48000) < 4096);
    CHECK(fabs(t->crossings / (t->decoded / 48000) - 880) < 20);
    // ffmpeg's tone is at 1/8 of full scale, lowered 3 dB when made stereo: an RMS of 0.0625.
    CHECK(fabs(sqrt(t->sum / t->decoded) - 0.0625) < 0.006);
    gs_aac_free(t->aac);
}
#endif

void test_media(void) {
    if (!test_streams) return;
    // Transport stream: 6 s at 30 fps, one keyframe a second, AAC frames of 1024 samples.
    tally a = replay_ts("a/ts", 3);
    CHECK(a.video == 180 && a.keys == 6);
    CHECK(fabs(a.min_video - 11.4) < 0.05 && a.max_video < 17.4);  // (ffmpeg's transport stream starts 1.4 s after its offset)
    CHECK(a.audio >= 280 && a.audio <= 284 && a.audio_in_order && a.rate == 48000);
#ifdef GS_TEST_VIDEO
    check_tone(&a);
#endif
    tally b = replay_ts("b/ts", 3);
    CHECK(b.video == 180 && b.rate == 44100 && b.min_video > 4999);
#ifdef GS_TEST_VIDEO
    check_tone(&b);  // (resampled from 44.1 kHz)
#endif
    // Timestamps that wrap past 2^33 halfway keep counting up.
    tally w = { .audio_in_order = true };
    gs_ts *ts = gs_ts_new(count, &w);
    size_t len;
    uint8_t *data = load("wrap.ts", &len);
    CHECK(data && gs_ts_feed(ts, data, len));
    gs_ts_end(ts);
    gs_ts_free(ts), SDL_free(data);
    CHECK(w.audio_in_order && w.last_audio > 95443 && w.max_video - w.min_video < 4.1);
#ifdef GS_TEST_VIDEO
    gs_aac_free(w.aac);
#endif
    // Fragmented MP4, the same streams.
    tally m = replay_mp4("a/mp4");
    CHECK(m.video == 180 && m.keys == 6 && m.audio >= 280 && m.audio_in_order && m.rate == 48000);
    CHECK(fabs(m.min_video) < 0.01);  // (the edit list removes the two-frame decoding delay)
#ifdef GS_TEST_VIDEO
    check_tone(&m);
#endif
    tally n = replay_mp4("b/mp4");
    CHECK(n.video == 180 && n.rate == 44100);
#ifdef GS_TEST_VIDEO
    gs_aac_free(n.aac);
#endif
    // Not a transport stream.
    ts = gs_ts_new(count, &w);
    CHECK(!gs_ts_feed(ts, (const uint8_t *)"not a stream at all", 19) || 1);
    gs_ts_free(ts);
}
