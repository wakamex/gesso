#include "gs_seq.h"

#include <math.h>
#include <stdlib.h>

enum { EV_NOTE, EV_OFF, EV_MIDI, EV_CALL };

typedef struct {
    double time;
    uint64_t order;  // keeps events at the same time in the order they were added
    int kind;
    uint8_t status, a, b;
    uint32_t id;
    gs_seq_fn *fn;
    void *user;
    double value;
} event;

#define NOTE_IDS 4096  // notes that can be sounding or pending at once, for note-offs by id
#define MAX_SOURCES 8

struct gs_seq {
    gs_synth *synth;
    int rate;
    uint64_t frame, order;
    uint32_t next_id;
    event *heap;
    int n, cap;
    struct { uint32_t id, tag; } notes[NOTE_IDS];
    gs_seq_feed_fn *feed;
    void *feed_user;
    double ahead;
    struct { gs_mix_fn *fn; void *user; } src[MAX_SOURCES];
    int nsrc;
};

static bool before(const event *x, const event *y) { return x->time < y->time || (x->time == y->time && x->order < y->order); }

static void push(gs_seq *q, event e) {
    if (q->n == q->cap) q->cap = q->cap ? 2 * q->cap : 256, q->heap = realloc(q->heap, sizeof *q->heap * (size_t)q->cap);
    e.order = q->order++;
    int i = q->n++;
    while (i && before(&e, &q->heap[(i - 1) / 2])) q->heap[i] = q->heap[(i - 1) / 2], i = (i - 1) / 2;
    q->heap[i] = e;
}

static void sift_down(gs_seq *q, int i);

static event pop(gs_seq *q) {
    event top = q->heap[0];
    q->heap[0] = q->heap[--q->n];
    if (q->n) sift_down(q, 0);
    return top;
}

gs_seq *gs_seq_new(gs_synth *synth, int rate) {
    gs_seq *q = calloc(1, sizeof *q);
    q->synth = synth, q->rate = rate;
    return q;
}

void gs_seq_free(gs_seq *q) {
    if (!q) return;
    free(q->heap);
    free(q);
}

double gs_seq_time(const gs_seq *q) { return (double)q->frame / q->rate; }

uint32_t gs_seq_note(gs_seq *q, double time, int channel, int note, int velocity) {
    if (!++q->next_id) q->next_id = 1;
    push(q, (event){ .time = time, .kind = EV_NOTE, .status = (uint8_t)(0x90 | (channel & 15)), .a = (uint8_t)note, .b = (uint8_t)velocity, .id = q->next_id });
    return q->next_id;
}

void gs_seq_note_off(gs_seq *q, double time, uint32_t id) { push(q, (event){ .time = time, .kind = EV_OFF, .id = id }); }

void gs_seq_midi(gs_seq *q, double time, uint8_t status, uint8_t a, uint8_t b) {
    push(q, (event){ .time = time, .kind = EV_MIDI, .status = status, .a = a, .b = b });
}

void gs_seq_call(gs_seq *q, double time, gs_seq_fn *fn, void *user, double value) {
    push(q, (event){ .time = time, .kind = EV_CALL, .fn = fn, .user = user, .value = value });
}

static void sift_down(gs_seq *q, int i) {
    event e = q->heap[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= q->n) break;
        if (c + 1 < q->n && before(&q->heap[c + 1], &q->heap[c])) c++;
        if (!before(&q->heap[c], &e)) break;
        q->heap[i] = q->heap[c], i = c;
    }
    q->heap[i] = e;
}

void gs_seq_clear_notes(gs_seq *q) {
    int kept = 0;
    for (int i = 0; i < q->n; i++)
        if (q->heap[i].kind != EV_NOTE && q->heap[i].kind != EV_OFF) q->heap[kept++] = q->heap[i];
    q->n = kept;
    for (int i = kept / 2 - 1; i >= 0; i--) sift_down(q, i);
}

void gs_seq_set_feeder(gs_seq *q, gs_seq_feed_fn *fn, void *user, double ahead) {
    q->feed = fn, q->feed_user = user, q->ahead = ahead;
}

void gs_seq_add_source(gs_seq *q, gs_mix_fn *fn, void *user) {
    if (q->nsrc < MAX_SOURCES) q->src[q->nsrc].fn = fn, q->src[q->nsrc++].user = user;
}

static void run(gs_seq *q, const event *e) {
    switch (e->kind) {
    case EV_NOTE: {
        uint32_t tag = gs_synth_note_on(q->synth, e->status & 15, e->a, e->b);
        q->notes[e->id % NOTE_IDS].id = e->id, q->notes[e->id % NOTE_IDS].tag = tag;
        break;
    }
    case EV_OFF:
        if (q->notes[e->id % NOTE_IDS].id == e->id) gs_synth_note_off_tag(q->synth, q->notes[e->id % NOTE_IDS].tag);
        break;
    case EV_MIDI: gs_synth_midi(q->synth, e->status, e->a, e->b); break;
    case EV_CALL: e->fn(e->user, e->value); break;
    }
}

void gs_seq_render(void *qp, float *lr, int frames) {
    gs_seq *q = qp;
    if (q->feed) q->feed(q->feed_user, q, gs_seq_time(q) + q->ahead);
    while (frames > 0) {
        double now = gs_seq_time(q), half = 0.5 / q->rate;
        while (q->n && q->heap[0].time <= now + half) {
            event e = pop(q);
            run(q, &e);
        }
        int k = frames;
        if (q->n) {
            double need = ceil((q->heap[0].time - now) * q->rate - 0.5);
            if (need < 1) need = 1;
            if (need < k) k = (int)need;
        }
        if (q->synth) gs_synth_render(q->synth, lr, k);
        for (int i = 0; i < q->nsrc; i++) q->src[i].fn(q->src[i].user, lr, k);
        q->frame += (uint64_t)k;
        lr += 2 * k;
        frames -= k;
    }
}
