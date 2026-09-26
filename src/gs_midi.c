#include "gs_midi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t tick, seq;  // seq keeps file order among events on the same tick
    uint8_t status, a, b, meta;
    uint32_t tempo;  // for tempo events: microseconds per quarter note
} raw_event;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint32_t vlq(const uint8_t **p, const uint8_t *end) {
    uint32_t v = 0;
    while (*p < end) {
        uint8_t c = *(*p)++;
        v = v << 7 | (c & 127);
        if (!(c & 128)) break;
    }
    return v;
}

static int by_tick(const void *a, const void *b) {
    const raw_event *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

bool gs_song_load(gs_song *song, const void *data, size_t len) {
    const uint8_t *p = data, *end = p + len;
    memset(song, 0, sizeof *song);
    if (len < 14 || memcmp(p, "MThd", 4)) return false;
    int ntracks = p[10] << 8 | p[11], division = p[12] << 8 | p[13];
    if (division & 0x8000 || !division) return false;  // SMPTE timing is not supported
    p += 8 + be32(p + 4);

    int cap = 256, n = 0;
    raw_event *ev = malloc(sizeof *ev * (size_t)cap);
    uint32_t last_tick = 0, seq = 0;
    for (int t = 0; t < ntracks && p + 8 <= end; t++) {
        uint32_t size = be32(p + 4);
        const uint8_t *q = p + 8, *te = q + size > end ? end : q + size;
        bool is_track = !memcmp(p, "MTrk", 4);
        p = te;
        if (!is_track) continue;
        uint32_t tick = 0;
        uint8_t running = 0;
        while (q < te) {
            tick += vlq(&q, te);
            if (q >= te) break;
            uint8_t st = *q;
            if (st & 0x80) q++, running = st < 0xf0 ? st : running;
            else st = running;
            raw_event e = { tick, seq++, st, 0, 0, 0, 0 };
            if (st == 0xff) {
                if (q >= te) break;
                uint8_t type = *q++;
                uint32_t l = vlq(&q, te);
                if (type == 0x51 && l == 3 && q + 3 <= te) e.meta = 1, e.tempo = (uint32_t)q[0] << 16 | q[1] << 8 | q[2];
                if (type == 0x2f && tick > last_tick) last_tick = tick;
                q += l;
                if (!e.meta) continue;
            } else if (st == 0xf0 || st == 0xf7) {
                q += vlq(&q, te);
                continue;
            } else {
                int data = (st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0 ? 1 : 2;
                if (q + data > te) break;
                e.a = q[0];
                e.b = data > 1 ? q[1] : 0;
                q += data;
            }
            if (tick > last_tick) last_tick = tick;
            if (n == cap) ev = realloc(ev, sizeof *ev * (size_t)(cap *= 2));
            ev[n++] = e;
        }
    }
    qsort(ev, (size_t)n, sizeof *ev, by_tick);

    // Ticks to seconds through the tempo map.
    song->ev = malloc(sizeof *song->ev * (size_t)(n ? n : 1));
    double sec = 0, per_tick = 500000.0 / 1e6 / division;
    uint32_t at = 0;
    for (int i = 0; i < n; i++) {
        sec += (ev[i].tick - at) * per_tick;
        at = ev[i].tick;
        if (ev[i].meta) per_tick = ev[i].tempo / 1e6 / division;
        else song->ev[song->n++] = (gs_midi_event){ sec, ev[i].status, ev[i].a, ev[i].b };
    }
    song->length = sec + (last_tick - at) * per_tick;
    free(ev);
    return true;
}

void gs_song_free(gs_song *song) {
    free(song->ev);
    memset(song, 0, sizeof *song);
}

void gs_player_init(gs_player *p, const gs_song *song, gs_synth *synth, int rate, bool loop) {
    *p = (gs_player){ song, synth, rate, loop, true, 0, 0 };
}

void gs_player_render(void *player, float *lr, int frames) {
    gs_player *p = player;
    const gs_song *s = p->song;
    while (frames > 0) {
        if (p->playing) {
            while (p->next < s->n && s->ev[p->next].time <= p->t + 0.5 / p->rate) {
                const gs_midi_event *e = &s->ev[p->next++];
                gs_synth_midi(p->synth, e->status, e->a, e->b);
            }
            if (p->next >= s->n && p->t >= s->length - 0.5 / p->rate) {
                if (p->loop && s->length > 0) p->t -= s->length, p->next = 0;
                else p->playing = false;
                continue;
            }
        }
        int k = frames;
        if (p->playing) {
            double until = p->next < s->n ? s->ev[p->next].time : s->length;
            double need = ceil((until - p->t) * p->rate - 0.5);
            if (need < 1) need = 1;
            if (need < k) k = (int)need;
        }
        gs_synth_render(p->synth, lr, k);
        p->t += (double)k / p->rate;
        lr += 2 * k;
        frames -= k;
    }
}
