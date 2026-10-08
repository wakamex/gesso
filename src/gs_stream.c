#include "gs_stream.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>

#include "gs_mix.h"

#define ANCHORS 32

struct gs_stream {
    SDL_Mutex *lock;
    SDL_Condition *space;
    float *ring;
    int cap, head, count, rate, prebuffer;  // in frames
    bool ended, stopped, paused, started, rebuffer;
    long long played, written;  // frames in all (written counts from the first, flushed ones included)
    // Timestamps: frame `index` of the stream is presented at `pts`; frames after it follow on.
    struct { long long index; double pts; } anchors[ANCHORS];
    int nanchors;
    // The clock: the frame being heard at the last mixing call, and when that was.
    double heard_at;
    uint64_t heard_ns;
};

gs_stream *gs_stream_new(int rate, double buffer_seconds, double prebuffer_seconds) {
    gs_stream *s = calloc(1, sizeof *s);
    s->rate = rate;
    s->cap = (int)(buffer_seconds * rate);
    s->prebuffer = (int)(prebuffer_seconds * rate);
    s->ring = calloc((size_t)s->cap * 2, sizeof *s->ring);
    s->lock = SDL_CreateMutex();
    s->space = SDL_CreateCondition();
    return s;
}

void gs_stream_free(gs_stream *s) {
    if (!s) return;
    SDL_DestroyCondition(s->space);
    SDL_DestroyMutex(s->lock);
    free(s->ring);
    free(s);
}

static bool write_locked(gs_stream *s, const float *lr, int frames) {
    while (frames > 0 && !s->stopped) {
        while (s->count == s->cap && !s->stopped) SDL_WaitCondition(s->space, s->lock);
        if (s->stopped) break;
        int k = s->cap - s->count < frames ? s->cap - s->count : frames;
        for (int i = 0; i < k; i++) {
            int at = (s->head + s->count + i) % s->cap;
            s->ring[2 * at] = lr[2 * i], s->ring[2 * at + 1] = lr[2 * i + 1];
        }
        s->count += k, s->written += k, lr += 2 * k, frames -= k;
    }
    return !s->stopped;
}

bool gs_stream_write(gs_stream *s, const float *lr, int frames) {
    SDL_LockMutex(s->lock);
    bool ok = write_locked(s, lr, frames);
    SDL_UnlockMutex(s->lock);
    return ok;
}

bool gs_stream_write_at(gs_stream *s, const float *lr, int frames, double pts) {
    SDL_LockMutex(s->lock);
    // A new anchor only where the time does not follow on from the last (a discontinuity, a gap).
    bool follows = false;
    if (s->nanchors) {
        double expect = s->anchors[s->nanchors - 1].pts + (double)(s->written - s->anchors[s->nanchors - 1].index) / s->rate;
        follows = fabs(expect - pts) < 0.002;
    }
    if (!follows) {
        if (s->nanchors == ANCHORS) SDL_memmove(s->anchors, s->anchors + 1, sizeof *s->anchors * (ANCHORS - 1)), s->nanchors--;
        s->anchors[s->nanchors].index = s->written, s->anchors[s->nanchors++].pts = pts;
    }
    bool ok = write_locked(s, lr, frames);
    SDL_UnlockMutex(s->lock);
    return ok;
}

void gs_stream_flush(gs_stream *s) {
    SDL_LockMutex(s->lock);
    s->written -= s->count, s->count = 0;
    while (s->nanchors && s->anchors[s->nanchors - 1].index >= s->written) s->nanchors--;  // (for audio never played)
    SDL_BroadcastCondition(s->space);
    SDL_UnlockMutex(s->lock);
}

void gs_stream_rebuffer(gs_stream *s, bool on) {
    SDL_LockMutex(s->lock);
    s->rebuffer = on;
    SDL_UnlockMutex(s->lock);
}

double gs_stream_clock(gs_stream *s) {
    SDL_LockMutex(s->lock);
    double heard = s->heard_at;
    if (s->started && !s->paused && s->heard_ns) heard += (double)(SDL_GetTicksNS() - s->heard_ns) * s->rate / 1e9;
    if (heard > (double)s->played) heard = (double)s->played;  // (nothing beyond what was mixed is heard)
    double pts = -1;
    for (int i = s->nanchors - 1; i >= 0; i--)
        if (s->anchors[i].index <= heard) { pts = s->anchors[i].pts + (heard - (double)s->anchors[i].index) / s->rate; break; }
    SDL_UnlockMutex(s->lock);
    return pts;
}

void gs_stream_end(gs_stream *s) {
    SDL_LockMutex(s->lock);
    s->ended = true;
    SDL_UnlockMutex(s->lock);
}

void gs_stream_stop(gs_stream *s) {
    SDL_LockMutex(s->lock);
    s->stopped = true;
    SDL_BroadcastCondition(s->space);
    SDL_UnlockMutex(s->lock);
}

void gs_stream_render(void *p, float *lr, int frames) {
    gs_stream *s = p;
    SDL_LockMutex(s->lock);
    // What is heard now: the frames played so far, less those mixed but still on their way out.
    s->heard_at = (double)s->played - gs_mix_latency_frames(), s->heard_ns = SDL_GetTicksNS();
    if (s->heard_at < 0) s->heard_at = 0;
    if (!s->started && (s->count >= s->prebuffer || s->ended)) s->started = true;
    if (s->started && !s->paused && !s->stopped) {
        int k = s->count < frames ? s->count : frames;  // an underrun leaves the rest silent
        for (int i = 0; i < k; i++) {
            int at = (s->head + i) % s->cap;
            lr[2 * i] += s->ring[2 * at], lr[2 * i + 1] += s->ring[2 * at + 1];
        }
        s->head = (s->head + k) % s->cap, s->count -= k, s->played += k;
        if (k) SDL_SignalCondition(s->space);
        if (s->rebuffer && !s->count && !s->ended) s->started = false;  // ran dry: wait for the prebuffer again
    }
    SDL_UnlockMutex(s->lock);
}

void gs_stream_pause(gs_stream *s, bool paused) {
    SDL_LockMutex(s->lock);
    s->paused = paused;
    SDL_UnlockMutex(s->lock);
}

bool gs_stream_paused(const gs_stream *s) { return s->paused; }
double gs_stream_position(const gs_stream *s) { return (double)s->played / s->rate; }
double gs_stream_buffered(const gs_stream *s) { return (double)s->count / s->rate; }
bool gs_stream_finished(const gs_stream *s) { return s->ended && s->count == 0; }
