#include "gs_movie.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gs_sound.h"
#include "gs_video.h"

#define POSTER 640  // the longest side of the stills kept, in pixels

typedef struct { uint64_t at; uint32_t size; double time; bool sync; } sample;  // one access unit: where it lies in the file, and when it shows

struct gs_movie {
    const uint8_t *data;
    size_t len;
    SDL_Renderer *r;
    sample *samples;
    int count;
    uint8_t *sets;        // the parameter sets, in Annex B form, to go before each key frame
    size_t sets_len;
    int nal_size;         // bytes of each unit's length in the file
    int width, height, turns;
    double length;
    gs_sound *sound;
    bool muted, loop, playing, ended;
    // The clock: `base` seconds at `since` (and from there on while playing). It counts through
    // loops, so the decoder's times never go back; the position is what is left of it in one length.
    double base, heard;
    uint64_t since;
    int loops;            // whole lengths the clock has passed, for starting the sound again
    // The decoder, only while it is wanted.
    gs_video *video;
    SDL_Thread *thread;
    SDL_AtomicInt looping, fed;  // fed: the thread has given the decoder everything
    gs_video_info info;   // the decoder's, as of the last frame asked for
    int first;            // the sample the thread starts at
    bool fresh;           // the decoder has shown nothing since it started
    bool settling;        // playing, but held where it is until the decoder has the frame for that place
    SDL_Texture *poster;  // the first frame, kept
    SDL_Texture *hold;    // the last frame of a decoder that was replaced, until the new one shows
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// The body of the first box of `type` in [p, end), and where it ends.
static const uint8_t *box(const uint8_t *p, const uint8_t *end, const char *type, const uint8_t **body_end) {
    while (end - p >= 8) {
        uint64_t size = be32(p);
        const uint8_t *body = p + 8;
        if (size == 1 && end - p >= 16) size = be64(p + 8), body = p + 16;
        else if (size == 0) size = (uint64_t)(end - p);
        if (size < (uint64_t)(body - p) || size > (uint64_t)(end - p)) return NULL;
        if (!memcmp(p + 4, type, 4)) return *body_end = p + size, body;
        p += size;
    }
    return NULL;
}

// The H.264 track of a plain MP4: its parameter sets from the avcC box, and from its sample table
// where each access unit lies, when it shows and whether decoding can start at it.
static bool read_track(gs_movie *m) {
    const uint8_t *end = m->data + m->len, *moov_end, *moov = box(m->data, end, "moov", &moov_end);
    for (const uint8_t *p = moov, *trak, *trak_end; p && (trak = box(p, moov_end, "trak", &trak_end)); p = trak_end) {
        const uint8_t *mdia_end, *mdia = box(trak, trak_end, "mdia", &mdia_end), *hdlr_end, *hdlr = mdia ? box(mdia, mdia_end, "hdlr", &hdlr_end) : NULL;
        if (!hdlr || hdlr_end - hdlr < 12 || memcmp(hdlr + 8, "vide", 4)) continue;
        const uint8_t *mdhd_end, *mdhd = box(mdia, mdia_end, "mdhd", &mdhd_end);
        if (!mdhd || mdhd_end - mdhd < 24) continue;
        const uint32_t scale = be32(mdhd + (mdhd[0] == 1 ? 20 : 12));
        const uint8_t *minf_end, *minf = box(mdia, mdia_end, "minf", &minf_end), *stbl_end, *stbl = minf ? box(minf, minf_end, "stbl", &stbl_end) : NULL;
        const uint8_t *stsd_end, *stsd = stbl ? box(stbl, stbl_end, "stsd", &stsd_end) : NULL;
        if (!scale || !stsd || stsd_end - stsd < 16 + 78) continue;
        const uint8_t *entry = stsd + 16, *entry_end = stsd + 8 + be32(stsd + 8);
        if ((memcmp(stsd + 12, "avc1", 4) && memcmp(stsd + 12, "avc3", 4)) || entry_end > stsd_end) continue;
        m->width = be16(entry + 24), m->height = be16(entry + 26);
        const uint8_t *avcc_end, *avcc = box(entry + 78, entry_end, "avcC", &avcc_end);
        if (!avcc || avcc_end - avcc < 7 || !m->width || !m->height) continue;
        m->nal_size = (avcc[4] & 3) + 1;
        m->sets = SDL_malloc((size_t)(avcc_end - avcc) * 2 + 8);
        const uint8_t *q = avcc + 5;
        for (int kind = 0; kind < 2 && m->sets && q < avcc_end; kind++)  // the sequence sets, then the picture sets
            for (int n = *q++ & (kind ? 0xff : 0x1f); n > 0 && avcc_end - q >= 2; n--) {
                const size_t size = be16(q);
                if ((size_t)(avcc_end - q - 2) < size) break;
                memcpy(m->sets + m->sets_len, "\0\0\0\1", 4), memcpy(m->sets + m->sets_len + 4, q + 2, size);
                m->sets_len += 4 + size, q += 2 + size;
            }
        // The way it is to be turned, from the track header's matrix.
        const uint8_t *tkhd_end, *tkhd = box(trak, trak_end, "tkhd", &tkhd_end);
        const int matrix = tkhd ? (tkhd[0] == 1 ? 52 : 40) : 0;
        if (tkhd && tkhd_end - tkhd >= matrix + 36) {
            const int32_t a = (int32_t)be32(tkhd + matrix), b = (int32_t)be32(tkhd + matrix + 4);
            m->turns = a > 0 ? 0 : a < 0 ? 2 : b > 0 ? 1 : 3;
        }
        // Samples lie in chunks: stsc says how many to a chunk, stco or co64 where each chunk starts,
        // stsz each sample's size; stts gives their durations in decoding order, ctts how much later
        // than that each shows, and stss which are key frames (all of them, without it).
        const uint8_t *stsz_end, *stsz = box(stbl, stbl_end, "stsz", &stsz_end), *stsc_end, *stsc = box(stbl, stbl_end, "stsc", &stsc_end);
        const uint8_t *stco_end, *stco = box(stbl, stbl_end, "stco", &stco_end), *stts_end, *stts = box(stbl, stbl_end, "stts", &stts_end);
        const uint8_t *ctts_end, *ctts = box(stbl, stbl_end, "ctts", &ctts_end), *stss_end, *stss = box(stbl, stbl_end, "stss", &stss_end);
        const bool wide = !stco;
        if (wide) stco = box(stbl, stbl_end, "co64", &stco_end);
        if (!m->sets_len || !stsz || !stsc || !stco || !stts || stsz_end - stsz < 12 || stsc_end - stsc < 8 || stco_end - stco < 8 || stts_end - stts < 8) continue;
        const uint32_t fixed = be32(stsz + 4), count = be32(stsz + 8), runs = be32(stsc + 4), chunks = be32(stco + 4);
        if (!count || count > 10000000 || (!fixed && (uint64_t)(stsz_end - stsz - 12) < 4ull * count) || (uint64_t)(stsc_end - stsc - 8) < 12ull * runs ||
            (uint64_t)(stco_end - stco - 8) < (wide ? 8ull : 4ull) * chunks) continue;
        m->samples = SDL_calloc(count, sizeof *m->samples);
        if (!m->samples) return false;
        uint32_t sample_at = 0;
        for (uint32_t chunk = 0, run = 0; chunk < chunks && sample_at < count && runs; chunk++) {
            while (run + 1 < runs && be32(stsc + 8 + 12 * (run + 1)) <= chunk + 1) run++;
            uint64_t at = wide ? be64(stco + 8 + 8 * chunk) : be32(stco + 8 + 4 * chunk);
            for (uint32_t i = 0, per = be32(stsc + 8 + 12 * run + 4); i < per && sample_at < count; i++, sample_at++) {
                const uint32_t size = fixed ? fixed : be32(stsz + 12 + 4 * sample_at);
                if (at > m->len || size > m->len - at) return false;
                m->samples[sample_at] = (sample){ at, size, 0, !stss };
                at += size;
            }
        }
        if (sample_at != count) return false;
        m->count = (int)count;
        // Times: decoding times run on from zero; showing times add each sample's offset, and the
        // earliest of them is the movie's start.
        uint64_t decode = 0, last = 0;
        int64_t earliest = INT64_MAX;
        const uint32_t durations = be32(stts + 4), offsets = ctts && ctts_end - ctts >= 8 ? be32(ctts + 4) : 0;
        uint32_t d = 0, d_left = 0, d_each = 0, o = 0, o_left = 0;
        int32_t o_each = 0;
        int64_t *shows = SDL_malloc(sizeof *shows * count);
        if (!shows) return false;
        for (uint32_t i = 0; i < count; i++) {
            while (!d_left && d < durations && (uint64_t)(stts_end - stts - 8) >= 8ull * (d + 1)) d_left = be32(stts + 8 + 8 * d), d_each = be32(stts + 12 + 8 * d), d++;
            while (!o_left && o < offsets && (uint64_t)(ctts_end - ctts - 8) >= 8ull * (o + 1)) o_left = be32(ctts + 8 + 8 * o), o_each = (int32_t)be32(ctts + 12 + 8 * o), o++;
            shows[i] = (int64_t)decode + (o_left ? o_each : 0);
            if (shows[i] < earliest) earliest = shows[i];
            if ((uint64_t)shows[i] + d_each > last) last = (uint64_t)shows[i] + d_each;
            decode += d_each;
            if (d_left) d_left--;
            if (o_left) o_left--;
        }
        for (uint32_t i = 0; i < count; i++) m->samples[i].time = (double)(shows[i] - earliest) / scale;
        SDL_free(shows);
        m->length = (double)((int64_t)last - earliest) / scale;
        for (uint32_t i = 0, n = stss && stss_end - stss >= 8 ? be32(stss + 4) : 0; i < n && (uint64_t)(stss_end - stss - 8) >= 4ull * (i + 1); i++) {
            const uint32_t key = be32(stss + 8 + 4 * i);
            if (key >= 1 && key <= count) m->samples[key - 1].sync = true;
        }
        m->samples[0].sync = true;
        return m->length > 0;
    }
    return false;
}

gs_movie *gs_movie_open(const void *data, size_t len, SDL_Renderer *r, int rate) {
    gs_movie *m = SDL_calloc(1, sizeof *m);
    if (!m) return NULL;
    m->data = data, m->len = len, m->r = r;
    if (!data || !read_track(m)) return gs_movie_free(m), NULL;
    m->sound = gs_sound_open(data, len, rate);  // (its AAC track, if it has one)
    return m;
}

// ---- The decoder ----

// Feeds the decoder from m->first on, each access unit turned from the file's form (units with
// their lengths in front) to the stream's (start codes), with the parameter sets before key frames.
// At the end it goes round again while the movie loops, on times that keep growing.
static int feed(void *movie) {
    gs_movie *m = movie;
    uint8_t *unit = NULL;
    size_t cap = 0;
    double round = 0;
    for (int i = m->first;; i++) {
        if (i == m->count) {
            if (!SDL_GetAtomicInt(&m->looping)) { gs_video_drain(m->video); SDL_SetAtomicInt(&m->fed, 1); break; }
            i = 0, round += m->length;
        }
        const sample *s = &m->samples[i];
        const size_t need = m->sets_len + (size_t)s->size + (size_t)s->size / 2 + 16;  // (a start code is at most three bytes longer than a length)
        if (need > cap) {
            uint8_t *grown = SDL_realloc(unit, cap = need + 65536);
            if (!grown) break;
            unit = grown;
        }
        size_t n = 0;
        if (s->sync) memcpy(unit, m->sets, m->sets_len), n = m->sets_len;
        for (const uint8_t *p = m->data + s->at, *end = p + s->size; end - p > m->nal_size;) {
            size_t size = 0;
            for (int b = 0; b < m->nal_size; b++) size = size << 8 | *p++;
            if (size > (size_t)(end - p) || n + 4 + size > cap) break;
            memcpy(unit + n, "\0\0\0\1", 4), memcpy(unit + n + 4, p, size);
            n += 4 + size, p += size;
        }
        if (!gs_video_decode(m->video, unit, n, round + s->time)) break;
    }
    SDL_free(unit);
    return 0;
}

static void end_decoder(gs_movie *m) {
    if (!m->video) return;
    gs_video_stop(m->video);
    SDL_WaitThread(m->thread, NULL);
    gs_video_free(m->video);
    m->video = NULL, m->thread = NULL;
}

// Starts decoding at the key frame at or before `seconds`.
static void begin_decoder(gs_movie *m, double seconds) {
    m->first = 0;
    for (int i = 0; i < m->count; i++)
        if (m->samples[i].sync && m->samples[i].time <= seconds + 1e-6 && m->samples[i].time >= m->samples[m->first].time) m->first = i;
    m->video = gs_video_new(m->r, 4, 0);
    if (!m->video) return;
    m->fresh = true, m->settling = m->playing;
    SDL_SetAtomicInt(&m->looping, m->loop), SDL_SetAtomicInt(&m->fed, 0);
    m->thread = SDL_CreateThread(feed, "gs_movie", m);
    if (!m->thread) gs_video_free(m->video), m->video = NULL;
}

// A copy of a frame at a still's size, to outlast the decoder it came from.
static SDL_Texture *keep(gs_movie *m, SDL_Texture *frame, const SDL_FRect *src) {
    const float scale = fminf(1, POSTER / fmaxf(src->w, src->h));
    SDL_Texture *copy = SDL_CreateTexture(m->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, SDL_max(1, (int)(src->w * scale)), SDL_max(1, (int)(src->h * scale)));
    if (!copy) return NULL;
    SDL_Texture *target = SDL_GetRenderTarget(m->r);
    SDL_SetRenderTarget(m->r, copy);
    SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(m->r, frame, src, NULL);
    SDL_SetRenderTarget(m->r, target);
    return copy;
}

// ---- The clock ----

static double clock_now(const gs_movie *m) { return m->playing && !m->settling ? m->base + (double)(SDL_GetTicksNS() - m->since) / 1e9 : m->base; }

static void clock_set(gs_movie *m, double seconds) { m->base = seconds, m->since = SDL_GetTicksNS(); }

static bool sounding(const gs_movie *m) { return m->sound && !m->muted; }

double gs_movie_position(const gs_movie *m) {
    const double t = clock_now(m);
    return m->loop ? fmod(t, m->length) : fmin(t, m->length);
}

void gs_movie_seek(gs_movie *m, double seconds) {
    seconds = SDL_clamp(seconds, 0, m->length);
    // What is showing stays up until the decoder, started again at the new place, has a frame.
    SDL_FRect src;
    SDL_Texture *frame = m->video && !m->fresh ? gs_video_frame(m->video, -1, &src) : NULL;
    if (frame) {
        if (m->hold) SDL_DestroyTexture(m->hold);
        m->hold = keep(m, frame, &src);
    }
    const bool decoding = m->video != NULL;
    end_decoder(m);
    clock_set(m, seconds), m->loops = 0, m->ended = false;
    if (m->sound) gs_sound_seek(m->sound, seconds), gs_sound_play(m->sound, false), m->heard = seconds;
    if (decoding || m->playing) begin_decoder(m, seconds);
}

void gs_movie_play(gs_movie *m, bool playing) {
    if (playing == m->playing) return;
    if (playing && m->ended) m->ended = false, clock_set(m, 0);
    if (playing) m->since = SDL_GetTicksNS();
    else m->base = clock_now(m);
    m->playing = playing;
    // (it waits, the sound with it, until the decoder shows the frame for where it starts; at once
    // when that frame is already up)
    if (playing) m->settling = true;
    if (m->sound) {
        if (playing) gs_sound_seek(m->sound, gs_movie_position(m)), m->heard = gs_movie_position(m);
        gs_sound_play(m->sound, playing && sounding(m) && !m->settling);
    }
    if (playing && !m->video) begin_decoder(m, gs_movie_position(m));
}

void gs_movie_rest(gs_movie *m) {
    end_decoder(m);
    if (m->hold) SDL_DestroyTexture(m->hold), m->hold = NULL;
    m->playing = false, m->settling = false, m->loops = 0;
    clock_set(m, 0);
    if (m->sound) gs_sound_play(m->sound, false), gs_sound_seek(m->sound, 0);
}

bool gs_movie_playing(const gs_movie *m) { return m->playing; }

bool gs_movie_waiting(const gs_movie *m) {
    if (!m->video) return !m->poster && !m->playing;  // (its first frame has not been asked for, or its decoder could not start)
    if (m->fresh) return true;
    // Behind its clock by more than a frame or two, with frames still to come: after a seek the
    // decoder works forward from the key frame before the place, and each call shows how far it is.
    const double behind = clock_now(m) - m->info.shown_pts, frame = m->length / m->count;
    return behind > 1.5 * frame && (!SDL_GetAtomicInt((SDL_AtomicInt *)&m->fed) || m->info.queued > 0);
}

gs_video_info gs_movie_get_info(const gs_movie *m) {
    gs_video_info info = m->info;
    if (!info.path) info.path = "none";
    if (!m->video) info.queued = 0, info.next_pts = -1;
    return info;
}
bool gs_movie_ended(const gs_movie *m) { return m->ended; }
double gs_movie_length(const gs_movie *m) { return m->length; }
bool gs_movie_has_sound(const gs_movie *m) { return m->sound != NULL; }

void gs_movie_size(const gs_movie *m, int *width, int *height) {
    if (width) *width = m->turns & 1 ? m->height : m->width;
    if (height) *height = m->turns & 1 ? m->width : m->height;
}

void gs_movie_set_loop(gs_movie *m, bool loop) {
    m->loop = loop;
    SDL_SetAtomicInt(&m->looping, loop);
}

void gs_movie_set_muted(gs_movie *m, bool muted) {
    if (muted == m->muted) return;
    m->muted = muted;
    if (m->sound && m->playing && !m->settling) {
        if (!muted) gs_sound_seek(m->sound, gs_movie_position(m)), m->heard = gs_movie_position(m);
        gs_sound_play(m->sound, !muted);
    }
}

void gs_movie_render(void *movie, float *lr, int frames) {
    gs_movie *m = movie;
    if (m->sound) gs_sound_render(m->sound, lr, frames);
}

// ---- The picture ----

SDL_Texture *gs_movie_frame(gs_movie *m, SDL_FRect *src, int *turns) {
    if (turns) *turns = m->turns;
    double t = clock_now(m);
    if (m->playing && sounding(m) && !m->settling) {
        if (m->loop) {
            // Each time round, the sound starts again with the picture.
            const int loops = (int)(t / m->length);
            if (loops != m->loops) m->loops = loops, gs_sound_seek(m->sound, 0), gs_sound_play(m->sound, true);
        } else {
            // The picture follows the sound where the sound is being heard: the clock is set by the
            // sound's place when it has moved and the two are apart by more than a frame or two.
            const double heard = gs_sound_position(m->sound);
            if (heard != m->heard && fabs(heard - t) > 0.06 && !gs_sound_ended(m->sound)) clock_set(m, heard), t = heard;
            m->heard = heard;
        }
    }
    if (m->playing && !m->loop && t >= m->length) {  // its end: back to the first frame, at rest
        gs_movie_rest(m);
        m->ended = true;
    }
    if (!m->video && (m->playing || !m->poster)) begin_decoder(m, gs_movie_position(m));
    SDL_Texture *frame = m->video ? gs_video_frame(m->video, clock_now(m), src) : NULL;
    if (m->video) m->info = gs_video_get_info(m->video);
    // Held after a seek or a start until the decoder has reached the place: then the clock runs from
    // there, and the sound with it, so that the picture never trails them.
    if (m->settling && m->video && !m->info.unshown && frame &&
        (m->base - m->info.shown_pts <= 1.5 * m->length / m->count || (SDL_GetAtomicInt(&m->fed) && m->info.queued == 0))) {
        m->settling = false, m->since = SDL_GetTicksNS();
        if (m->sound && sounding(m)) gs_sound_seek(m->sound, gs_movie_position(m)), m->heard = gs_movie_position(m), gs_sound_play(m->sound, true);
    }
    if (m->video && m->info.unshown) {  // the hardware's frames cannot be shown here: again, in software
        end_decoder(m);
        begin_decoder(m, gs_movie_position(m));
        frame = NULL;
    }
    if (frame) {
        m->fresh = false;
        if (m->hold) SDL_DestroyTexture(m->hold), m->hold = NULL;
        if (!m->poster && m->info.shown_pts < 0.5 / 24) m->poster = keep(m, frame, src);
        // At rest with its first frame kept, the decoder has done what it was started for.
        if (!m->playing && m->poster && clock_now(m) == 0) {
            end_decoder(m);
            frame = NULL;
        } else return frame;
    }
    SDL_Texture *still = m->hold ? m->hold : m->poster;
    if (still && src) {
        float w, h;
        SDL_GetTextureSize(still, &w, &h);
        *src = (SDL_FRect){ 0, 0, w, h };
    }
    return still;
}

bool gs_movie_draw(gs_movie *m, SDL_FRect dst) {
    SDL_FRect src;
    int turns;
    SDL_Texture *frame = gs_movie_frame(m, &src, &turns);
    if (!frame) return false;
    // Turned about the middle of where it goes: a quarter turn swaps the sides it is drawn with.
    if (turns & 1) dst = (SDL_FRect){ dst.x + (dst.w - dst.h) / 2, dst.y + (dst.h - dst.w) / 2, dst.h, dst.w };
    SDL_RenderTextureRotated(m->r, frame, &src, &dst, 90.0 * turns, NULL, SDL_FLIP_NONE);
    return true;
}

void gs_movie_free(gs_movie *m) {
    if (!m) return;
    end_decoder(m);
    if (m->poster) SDL_DestroyTexture(m->poster);
    if (m->hold) SDL_DestroyTexture(m->hold);
    if (m->sound) gs_sound_free(m->sound);
    SDL_free(m->samples), SDL_free(m->sets);
    SDL_free(m);
}
