#include "gs_stream.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

struct gs_stream {
    SDL_Mutex *lock;
    SDL_Condition *space;
    float *ring;
    int cap, head, count, rate, prebuffer;  // in frames
    bool ended, stopped, paused, started;
    long long played;
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

bool gs_stream_write(gs_stream *s, const float *lr, int frames) {
    SDL_LockMutex(s->lock);
    while (frames > 0 && !s->stopped) {
        while (s->count == s->cap && !s->stopped) SDL_WaitCondition(s->space, s->lock);
        if (s->stopped) break;
        int k = s->cap - s->count < frames ? s->cap - s->count : frames;
        for (int i = 0; i < k; i++) {
            int at = (s->head + s->count + i) % s->cap;
            s->ring[2 * at] = lr[2 * i], s->ring[2 * at + 1] = lr[2 * i + 1];
        }
        s->count += k, lr += 2 * k, frames -= k;
    }
    bool ok = !s->stopped;
    SDL_UnlockMutex(s->lock);
    return ok;
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
    if (!s->started && (s->count >= s->prebuffer || s->ended)) s->started = true;
    if (s->started && !s->paused && !s->stopped) {
        int k = s->count < frames ? s->count : frames;  // an underrun leaves the rest silent
        for (int i = 0; i < k; i++) {
            int at = (s->head + i) % s->cap;
            lr[2 * i] += s->ring[2 * at], lr[2 * i + 1] += s->ring[2 * at + 1];
        }
        s->head = (s->head + k) % s->cap, s->count -= k, s->played += k;
        if (k) SDL_SignalCondition(s->space);
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
