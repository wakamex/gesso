#include "gs_live.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gs_aac.h"
#include "gs_http.h"
#include "gs_mix.h"
#include "gs_mp4.h"
#include "gs_stream.h"
#include "gs_ts.h"

#define AUDIO_SECONDS 2.0   // decoded audio held in the stream
#define PREBUFFER 1.0       // decoded audio before playing, and again after running dry

typedef struct packet {
    struct packet *next;
    gs_media_kind kind;
    double pts;
    bool key;
    gs_aac_config aac;
    size_t len;
    uint8_t data[];
} packet;

typedef struct {
    packet *head, *tail;
    int count;
} queue;

struct gs_live {
    gs_live_config c;
    char url[4096];
    char **headers;            // a copy of the config's
    SDL_Mutex *lock;
    SDL_Condition *wake;       // queues gained or lost packets, or the player is stopping
    queue audio, pictures;  // compressed AAC frames and H.264 access units
    bool stopping, loaded_all, failed;
    SDL_Thread *loader, *audio_thread, *video_thread;
    gs_stream *stream;
    gs_video *video;
    // For the info.
    int segments, discontinuities, skips, stalls, renewals;
    double edge;               // seconds of playlist after what has been fetched, at the last poll
    double margin;             // seconds behind the live edge it started at, which a stall refills
    bool held;                 // the sound waits for the first pictures, so both start together
    gs_live_state last_state;
    char message[160];
};

// ---- Queues ----

static double span(const queue *q) { return q->head ? q->tail->pts - q->head->pts : 0; }
static double queued_seconds(gs_live *l) { return l->audio.head ? span(&l->audio) : span(&l->pictures); }

static void push(gs_live *l, queue *q, packet *p) {
    if (q->tail) q->tail->next = p;
    else q->head = p;
    q->tail = p, q->count++;
}

// The next packet, waiting for one; NULL when stopping, or when the loader has finished and the queue is empty.
static packet *pop(gs_live *l, queue *q) {
    SDL_LockMutex(l->lock);
    while (!q->head && !l->stopping && !l->loaded_all && !l->failed) SDL_WaitCondition(l->wake, l->lock);
    packet *p = q->head;
    if (p) {
        q->head = p->next, q->count--;
        if (!q->head) q->tail = NULL;
        SDL_BroadcastCondition(l->wake);  // (the loader may be waiting for room)
    }
    SDL_UnlockMutex(l->lock);
    return p;
}

static void clear(queue *q) {
    while (q->head) {
        packet *p = q->head;
        q->head = p->next;
        free(p);
    }
    q->tail = NULL, q->count = 0;
}

static void say(gs_live *l, const char *message) {
    SDL_LockMutex(l->lock);
    SDL_strlcpy(l->message, message, sizeof l->message);
    SDL_UnlockMutex(l->lock);
    if (l->c.changed) l->c.changed(l->c.user);
}

// Waits up to ms or until stopping; false when stopping.
static bool rest(gs_live *l, int ms) {
    SDL_LockMutex(l->lock);
    if (!l->stopping) SDL_WaitConditionTimeout(l->wake, l->lock, ms);
    bool go = !l->stopping;
    SDL_UnlockMutex(l->lock);
    return go;
}

// ---- The loader: playlists, segments, demuxing, the timeline ----

typedef struct {
    packet *head, *tail;  // one segment's frames, before they are placed on the timeline
    double first;         // the earliest presentation time among them
    bool video;
} segment_frames;

static void collect(void *user, const gs_media_frame *f) {
    segment_frames *s = user;
    if (f->kind == GS_MEDIA_H264 && !s->video) return;
    packet *p = malloc(sizeof *p + f->len);
    if (!p) return;
    *p = (packet){ .kind = f->kind, .pts = f->pts, .key = f->key, .len = f->len };
    if (f->aac) p->aac = *f->aac;
    memcpy(p->data, f->data, f->len);
    if (s->tail) s->tail->next = p;
    else s->head = p;
    s->tail = p;
    if (f->pts < s->first) s->first = f->pts;
}

static int fetch(gs_live *l, const char *url, char **body, size_t *len) {
    gs_http_request r = { .url = url, .agent = l->c.agent, .headers = (const char *const *)l->headers, .follow = true, .timeout_ms = 15000 };
    return gs_http_fetch(&r, body, len);
}

// The index of the segment to start at: about `delay` seconds of segments before the live edge, or
// the first segment of a playlist that is complete (EXT-X-ENDLIST: a recording, not a live stream).
static int start_index(const gs_hls_media *m, double delay) {
    if (m->ended) return 0;
    if (delay <= 0) delay = 0;
    double sum = 0;
    int i = m->count;
    for (int n = 0; i > 0; n++) {
        if (delay ? sum >= delay : n >= 3) break;
        sum += m->segments[--i].duration;
    }
    return i;
}

static int load(void *user) {
    gs_live *l = user;
    long long next = -1;          // the media sequence number to fetch next
    double timeline = 0, offset = 0;  // where the next segment starts on the player's timeline; the shift onto it
    double end = 0;               // where the media queued so far ends on the timeline (audio can run past its segment)
    bool shift = true;            // the next segment starts a new run of times (the first, a discontinuity, a skip)
    char map[2048] = "";
    gs_mp4 *mp4 = NULL;
    gs_ts *ts = NULL;
    segment_frames frames = { 0 };
    int failures = 0;
    while (rest(l, 0)) {
        char *text = NULL;
        size_t len = 0;
        int status = fetch(l, l->url, &text, &len);
        gs_hls_media *m = status == 200 ? gs_hls_media_parse(text, len, l->url) : NULL;
        SDL_free(text);
        if (!m) {
            bool expired = status == 403 || status == 404 || status == 410;
            if (expired && l->c.renew) {
                char fresh[sizeof l->url];
                if (l->c.renew(l->c.user, fresh, sizeof fresh)) {
                    SDL_strlcpy(l->url, fresh, sizeof l->url);
                    l->renewals++;
                    continue;
                }
            }
            if (expired) {
                say(l, status == 404 ? "The stream has ended" : "The stream's playlist is no longer available");
                SDL_LockMutex(l->lock);
                l->failed = true;
                SDL_BroadcastCondition(l->wake);
                SDL_UnlockMutex(l->lock);
                break;
            }
            failures++;
            say(l, status ? "The stream's playlist could not be read; trying again" : "No answer from the stream's server; trying again");
            if (!rest(l, failures < 4 ? 1000 << failures : 8000)) break;
            continue;
        }
        failures = 0;
        // Where to go on from: the start, or ahead when the playlist has moved past what we fetched.
        int from = 0;
        if (next < 0 || next < m->sequence || next > m->sequence + m->count + 30) {
            if (next >= 0) l->skips++, shift = true;
            from = start_index(m, l->c.delay);
            double margin = 0;
            for (int k = from; k < m->count; k++) margin += m->segments[k].duration;
            SDL_LockMutex(l->lock);
            l->margin = margin;
            SDL_UnlockMutex(l->lock);
        } else {
            from = (int)(next - m->sequence);
        }
        for (int i = from; i < m->count && rest(l, 0); i++) {
            const gs_hls_segment *s = &m->segments[i];
            if (l->c.segment) l->c.segment(l->c.user, s);
            if (s->discontinuity && next >= 0) shift = true, l->discontinuities++;
            uint8_t *data = NULL;
            size_t dlen = 0;
            int got = 0;
            for (int attempt = 0; attempt < 3 && got != 200 && rest(l, 0); attempt++) {
                SDL_free(data), data = NULL;
                got = fetch(l, s->url, (char **)&data, &dlen);
                if (got != 200 && !rest(l, 500)) break;
            }
            next = s->sequence + 1;
            if (got != 200) {  // a segment lost: the next one starts a new run of times
                SDL_free(data);
                timeline += s->duration, shift = true;
                say(l, "A segment could not be fetched; skipping it");
                continue;
            }
            frames = (segment_frames){ .first = INFINITY, .video = l->c.video };
            if (s->map_url) {  // fragmented MP4, with its initialization segment
                if (strcmp(map, s->map_url)) {
                    char *init = NULL;
                    size_t ilen = 0;
                    if (fetch(l, s->map_url, &init, &ilen) == 200) {
                        if (!mp4) mp4 = gs_mp4_new(collect, &frames);
                        if (mp4 && gs_mp4_init(mp4, (const uint8_t *)init, ilen)) SDL_strlcpy(map, s->map_url, sizeof map);
                    }
                    SDL_free(init);
                }
                if (mp4 && !strcmp(map, s->map_url)) gs_mp4_segment(mp4, data, dlen);
            } else {
                if (!ts) ts = gs_ts_new(collect, &frames);
                if (shift) gs_ts_reset(ts);
                gs_ts_feed(ts, data, dlen);
                gs_ts_end(ts);
            }
            SDL_free(data);
            // A new run of times continues from where the media so far ended, not before it.
            if (shift && isfinite(frames.first)) offset = fmax(timeline, end) - frames.first, shift = false;
            // Place the frames on the timeline and queue them, waiting while the queues are full.
            double cap = l->c.buffer > 0 ? l->c.buffer : 8;
            SDL_LockMutex(l->lock);
            for (packet *p = frames.head, *n; p; p = n) {
                n = p->next, p->next = NULL;
                p->pts += offset;
                double until = p->pts + (p->kind == GS_MEDIA_AAC && p->aac.rate ? 1024.0 / p->aac.rate : 0);
                if (until > end) end = until;
                while (!l->stopping && queued_seconds(l) > cap) SDL_WaitCondition(l->wake, l->lock);
                if (l->stopping) { free(p); continue; }
                push(l, p->kind == GS_MEDIA_AAC ? &l->audio : &l->pictures, p);
                SDL_BroadcastCondition(l->wake);
            }
            l->segments++;
            double rest_of_playlist = 0;
            for (int k = i + 1; k < m->count; k++) rest_of_playlist += m->segments[k].duration;
            l->edge = rest_of_playlist;
            SDL_UnlockMutex(l->lock);
            frames.head = frames.tail = NULL;
            timeline += s->duration;
        }
        bool ended = m->ended && next >= m->sequence + m->count;
        double wait = m->count ? m->segments[m->count - 1].duration / 2 : 1;
        gs_hls_media_free(m);
        if (ended) {
            SDL_LockMutex(l->lock);
            l->loaded_all = true;
            SDL_BroadcastCondition(l->wake);
            SDL_UnlockMutex(l->lock);
            break;
        }
        if (!rest(l, (int)(SDL_max(wait, 0.5) * 1000))) break;
    }
    gs_mp4_free(mp4);
    gs_ts_free(ts);
    if (l->c.changed) l->c.changed(l->c.user);
    return 0;
}

// ---- Decoding ----

static int decode_audio(void *user) {
    gs_live *l = user;
    gs_aac *aac = NULL;
    gs_aac_config config = { 0 };
    int rate = gs_mix_rate();
    float *lr = malloc(sizeof *lr * 2 * 8192);
    double cap = l->c.buffer > 0 ? l->c.buffer : 8;
    bool refilled = false;
    for (packet *p; lr; ) {
        // Ran dry at the live edge (a sound card a little faster than the stream, a slow network): wait
        // once until the margin it started with is queued again, so the next stall is far off rather
        // than a moment away, then decode freely until the stream plays again.
        bool starved = gs_stream_starved(l->stream);
        if (starved && !refilled) {
            SDL_LockMutex(l->lock);
            while (!l->stopping && !l->loaded_all && !l->failed && queued_seconds(l) < fmin(l->margin, cap - 1)) SDL_WaitCondition(l->wake, l->lock);
            SDL_UnlockMutex(l->lock);
        }
        refilled = starved;
        if (!(p = pop(l, &l->audio))) break;
        if (!aac || memcmp(&config, &p->aac, sizeof config)) {  // a new stream (after an ad, say)
            gs_aac_free(aac);
            config = p->aac;
            aac = gs_aac_new(&config, rate);
        }
        int n = aac ? gs_aac_decode(aac, p->data, p->len, lr, 8192) : 0;
        double pts = p->pts;
        free(p);
        if (n > 0 && !gs_stream_write_at(l->stream, lr, n, pts)) break;
    }
    // Played out: once the loader has everything and the queue is empty, the stream ends.
    gs_stream_end(l->stream);
    gs_aac_free(aac);
    free(lr);
    return 0;
}

static int decode_video(void *user) {
    gs_live *l = user;
    for (packet *p; (p = pop(l, &l->pictures));) {
        bool ok = gs_video_decode(l->video, p->data, p->len, p->pts);
        free(p);
        if (!ok) break;
    }
    return 0;
}

// ---- The player ----

gs_live *gs_live_start(const gs_live_config *c) {
    gs_live *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->c = *c;
    SDL_strlcpy(l->url, c->url, sizeof l->url);
    int nh = 0;
    while (c->headers && c->headers[nh]) nh++;
    l->headers = calloc((size_t)nh + 1, sizeof *l->headers);
    for (int i = 0; l->headers && i < nh; i++) l->headers[i] = SDL_strdup(c->headers[i]);
    l->lock = SDL_CreateMutex();
    l->wake = SDL_CreateCondition();
    l->stream = gs_stream_new(gs_mix_rate(), AUDIO_SECONDS, PREBUFFER);
    if (l->stream) gs_stream_rebuffer(l->stream, true);
    if (c->video && c->renderer) l->video = gs_video_new(c->renderer, 6, 0);
    if (!l->lock || !l->wake || !l->stream || !l->headers || (c->video && !l->video)) return gs_live_stop(l), NULL;
    if (l->video) gs_stream_pause(l->stream, l->held = true);
    gs_mix_add(gs_stream_render, l->stream);
    l->audio_thread = SDL_CreateThread(decode_audio, "live audio", l);
    if (l->video) l->video_thread = SDL_CreateThread(decode_video, "live video", l);
    l->loader = SDL_CreateThread(load, "live loader", l);
    return l;
}

void gs_live_stop(gs_live *l) {
    if (!l) return;
    if (l->lock) {
        SDL_LockMutex(l->lock);
        l->stopping = true;
        SDL_BroadcastCondition(l->wake);
        SDL_UnlockMutex(l->lock);
    }
    if (l->stream) gs_stream_stop(l->stream);
    if (l->video) gs_video_stop(l->video);
    SDL_WaitThread(l->loader, NULL);
    SDL_WaitThread(l->audio_thread, NULL);
    SDL_WaitThread(l->video_thread, NULL);
    if (l->stream) gs_mix_remove(gs_stream_render, l->stream), gs_stream_free(l->stream);
    gs_video_free(l->video);
    clear(&l->audio), clear(&l->pictures);
    for (int i = 0; l->headers && l->headers[i]; i++) SDL_free(l->headers[i]);
    free(l->headers);
    SDL_DestroyCondition(l->wake);
    SDL_DestroyMutex(l->lock);
    free(l);
}

gs_live_info gs_live_get_info(gs_live *l) {
    gs_live_info i = { .clock = gs_stream_clock(l->stream), .buffered = gs_stream_buffered(l->stream) };
    SDL_LockMutex(l->lock);
    i.queued = queued_seconds(l);
    i.behind = l->edge + i.queued + i.buffered;
    i.segments = l->segments, i.discontinuities = l->discontinuities, i.skips = l->skips, i.renewals = l->renewals;
    SDL_strlcpy(i.message, l->message, sizeof i.message);
    bool drained = l->loaded_all && !l->audio.head;
    gs_live_state s = l->failed ? GS_LIVE_FAILED
                    : drained && gs_stream_finished(l->stream) ? GS_LIVE_ENDED
                    : i.clock < 0 ? GS_LIVE_STARTING
                    : i.buffered <= 0 ? GS_LIVE_BUFFERING : GS_LIVE_PLAYING;
    if (s == GS_LIVE_BUFFERING && l->last_state == GS_LIVE_PLAYING) l->stalls++;
    l->last_state = s;
    i.state = s, i.stalls = l->stalls;
    SDL_UnlockMutex(l->lock);
    if (l->video) i.video = gs_video_get_info(l->video);
    return i;
}

SDL_Texture *gs_live_frame(gs_live *l, SDL_FRect *src) {
    if (!l->video) return NULL;
    if (l->held && (gs_video_get_info(l->video).queued >= 3 || gs_stream_buffered(l->stream) >= AUDIO_SECONDS - 0.05))
        gs_stream_pause(l->stream, l->held = false);  // pictures are ready (or the sound would wait in vain)
    double clock = gs_stream_clock(l->stream);
    return gs_video_frame(l->video, clock, src);
}
