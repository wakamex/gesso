// gs_loopback's queue (internal): samples recorded on one thread, read on another in the same steps as
// the microphone. Whatever stays queued from one read to the next is delay, which puts the echo
// canceller's reference behind the microphone's echo of it, so the queue keeps only what jitter needs:
// what was recorded before the first read goes, and so does any part of the queue that, over half a
// second of reads, never ran down below SLACK_MS. Callers hold their own lock.
#pragma once
#include <SDL3/SDL.h>

#define GS_LOOPBACK_SLACK_MS 10  // kept queued for the recording thread's uneven delivery
#define GS_LOOPBACK_KEEP_MS 200  // the most kept for a reader that has fallen behind

typedef struct {
    float *ring;
    int cap, start, count, rate;
    int low, read;  // the least left queued after a read, over the reads of the last `read` samples
    bool reading;   // there has been a read
} gs_loopback_queue;

static inline void gs_loopback_drop(gs_loopback_queue *q, int n) {
    n = SDL_min(n, q->count);
    if (n <= 0) return;
    q->start = (q->start + n) % q->cap, q->count -= n;
}

static inline void gs_loopback_put(gs_loopback_queue *q, const float *s, int n) {
    for (int i = 0; i < n; i++) {
        if (q->count == q->cap) gs_loopback_drop(q, 1);  // (the oldest go)
        q->ring[(q->start + q->count++) % q->cap] = s[i];
    }
}

// The next `frames` samples into out, silence for those not recorded yet; returns how many were.
static inline int gs_loopback_take(gs_loopback_queue *q, float *out, int frames) {
    const int slack = q->rate * GS_LOOPBACK_SLACK_MS / 1000;
    if (!q->reading) gs_loopback_drop(q, q->count - frames - slack), q->reading = true, q->low = q->count;  // (recorded before anyone read)
    gs_loopback_drop(q, q->count - (q->rate * GS_LOOPBACK_KEEP_MS / 1000 + frames));
    int got = SDL_min(frames, q->count);
    for (int i = 0; i < got; i++) out[i] = q->ring[(q->start + i) % q->cap];
    gs_loopback_drop(q, got);
    for (int i = got; i < frames; i++) out[i] = 0;
    q->low = SDL_min(q->low, q->count), q->read += frames;
    if (q->read >= q->rate / 2) {  // what never drained over the half second is standing delay
        if (q->low > slack) gs_loopback_drop(q, q->low - slack);
        q->low = q->count, q->read = 0;
    }
    return got;
}
