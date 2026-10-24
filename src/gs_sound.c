#include "gs_sound.h"

#include <SDL3/SDL.h>
#include <opus.h>
#include <stdlib.h>
#include <string.h>

#include "gs_opus.h"
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"  // (compiled in gs_mp3.c)
#ifdef GS_HAVE_AAC
#include "gs_aac.h"
#endif
#include "gs_media.h"
#include "gs_mp4.h"

typedef enum { MP3, AAC, OPUS } codec;
typedef struct { uint32_t at, len; } packet;  // a frame's bytes, from the start of the file (or of `copy`)

struct gs_sound {
    const uint8_t *data;  // the packets' bytes: the file, or `copy`
    uint8_t *copy;        // Ogg: the packets taken out of their pages
    codec kind;
    packet *packets;
    int npackets, cap;
    double packet_seconds;  // how long each packet plays, where all are alike (MP3, AAC)
    int64_t *starts;        // Ogg Opus: the sample each packet starts at, at 48 kHz, counted after the pre-skip
    double length;
    double lead;            // seconds of the first packets that come before the sound proper (an encoder's run-in), left out
    int rate, decoded_rate, channels;  // the mix's rate; the rate the decoder hands samples over at
    int preroll;                        // packets decoded before a seek's target, for the decoder to settle
    mp3dec_t mp3;
    gs_opus *opus;
    gs_aac_config aac_config;
#ifdef GS_HAVE_AAC
    gs_aac *aac;
#endif
    SDL_AudioStream *out;  // decoded sound waiting to be played, converted to the mix's rate
    SDL_Mutex *lock;
    int next;              // the packet to decode next
    int64_t drop;          // decoded frames still to be left out, after a seek or at the start
    int64_t played;        // frames handed to the mixer since `base`
    double base;           // where the last seek went
    bool playing, ended, drained;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static bool add_packet(gs_sound *s, size_t at, size_t len) {
    if (s->npackets == s->cap) {
        packet *more = realloc(s->packets, sizeof *more * (size_t)(s->cap = s->cap ? s->cap * 2 : 256));
        if (!more) return false;
        s->packets = more;
    }
    s->packets[s->npackets++] = (packet){ (uint32_t)at, (uint32_t)len };
    return true;
}

// ---- MP3 ----

// Its frames found by their headers, without decoding them. An ID3 tag in front is stepped over.
static bool open_mp3(gs_sound *s, const uint8_t *d, size_t len) {
    size_t at = 0;
    if (len > 10 && !memcmp(d, "ID3", 3)) at = 10 + ((size_t)(d[6] & 0x7F) << 21 | (size_t)(d[7] & 0x7F) << 14 | (size_t)(d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    mp3dec_t scan;
    mp3dec_init(&scan);
    int samples = 0, hz = 0;
    while (at < len) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&scan, d + at, (int)SDL_min(len - at, (size_t)1 << 20), NULL, &info);
        if (!info.frame_bytes) break;
        if (n > 0) {
            if (!samples) samples = n, hz = info.hz, s->channels = info.channels;
            if (info.hz != hz || !add_packet(s, at + (size_t)info.frame_offset, (size_t)(info.frame_bytes - info.frame_offset))) break;
        } else if (!s->npackets && at > 65536) return false;  // (no frame in the first of it: not an MP3)
        at += (size_t)info.frame_bytes;
    }
    if (s->npackets < 2) return false;
    s->kind = MP3, s->decoded_rate = hz, s->packet_seconds = (double)samples / hz, s->preroll = 10;  // (a frame borrows bits from those before it)
    // Most encoders put a frame of their own first, marked Xing or Info, which holds no sound and
    // says how many samples of run-in and of padding surround the sound: both are left out, with the
    // decoder's own delay of 529 samples.
    const uint8_t *f = d + s->packets[0].at, *end = f + s->packets[0].len;
    bool mpeg1 = (f[1] >> 3 & 3) == 3, mono = (f[3] >> 6) == 3;
    const uint8_t *tag = f + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    int64_t total = (int64_t)s->npackets * samples;
    if (tag + 8 <= end && (!memcmp(tag, "Xing", 4) || !memcmp(tag, "Info", 4))) {
        const uint8_t *p = tag + 8 + (tag[7] & 1 ? 4 : 0) + (tag[7] & 2 ? 4 : 0) + (tag[7] & 4 ? 100 : 0) + (tag[7] & 8 ? 4 : 0);
        int delay = 0, padding = 0;
        if (p + 24 <= end) delay = p[21] << 4 | p[22] >> 4, padding = (p[22] & 15) << 8 | p[23];
        memmove(s->packets, s->packets + 1, sizeof *s->packets * (size_t)--s->npackets);
        total -= samples;
        if (delay || padding) s->lead = (delay + 529 + 1) / (double)hz, total -= delay + padding;
    }
    s->length = total > 0 ? (double)total / hz : 0;
    return true;
}

// ---- AAC: a stream of ADTS frames, or a plain MP4 ----

static bool open_adts(gs_sound *s, const uint8_t *d, size_t len) {
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    int rate = 0, channels = 0;
    for (size_t at = 0; at + 7 <= len;) {
        const uint8_t *p = d + at;
        if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) break;  // the sync word, with the layer bits that tell it from MP3
        size_t size = (size_t)(p[3] & 3) << 11 | (size_t)p[4] << 3 | p[5] >> 5, head = p[1] & 1 ? 7 : 9;
        if (size <= head || at + size > len) break;
        if (!rate) rate = rates[p[2] >> 2 & 15], channels = (p[2] & 1) << 2 | p[3] >> 6;
        if (!add_packet(s, at + head, size - head)) break;
        at += size;
    }
    if (s->npackets < 2 || !gs_aac_config_make(&s->aac_config, rate, channels ? channels : 2)) return false;
    s->kind = AAC, s->channels = channels, s->packet_seconds = 1024.0 / rate, s->preroll = 1;
    s->length = s->npackets * s->packet_seconds;
    return true;
}

// The first box of a type among those in [p, end): its body, and where the body ends.
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

// An MP4: the sound track's sample table says where each AAC frame lies in the file, or in a
// fragmented one the moof boxes do (gs_mp4_samples).
static bool open_mp4(gs_sound *s, const uint8_t *d, size_t len) {
    const uint8_t *end = d + len, *moov_end, *moov = box(d, end, "moov", &moov_end);
    for (const uint8_t *p = moov, *trak, *trak_end; p && (trak = box(p, moov_end, "trak", &trak_end)); p = trak_end) {
        const uint8_t *e, *mdia = box(trak, trak_end, "mdia", &e), *mdia_end = e;
        const uint8_t *hdlr = mdia ? box(mdia, mdia_end, "hdlr", &e) : NULL;
        if (!hdlr || e - hdlr < 12 || memcmp(hdlr + 8, "soun", 4)) continue;
        const uint8_t *mdhd = box(mdia, mdia_end, "mdhd", &e);
        if (!mdhd || e - mdhd < 20) continue;
        uint32_t timescale = mdhd[0] == 1 && e - mdhd >= 32 ? be32(mdhd + 20) : be32(mdhd + 12);
        uint64_t duration = mdhd[0] == 1 && e - mdhd >= 32 ? be64(mdhd + 24) : be32(mdhd + 16);
        const uint8_t *minf_end, *minf = box(mdia, mdia_end, "minf", &minf_end), *stbl_end, *stbl = minf ? box(minf, minf_end, "stbl", &stbl_end) : NULL;
        const uint8_t *stsd_end, *stsd = stbl ? box(stbl, stbl_end, "stsd", &stsd_end) : NULL;
        const uint8_t *entry_end, *entry = stsd && stsd_end - stsd > 8 ? box(stsd + 8, stsd_end, "mp4a", &entry_end) : NULL;
        if (!entry || entry_end - entry < 28 || !timescale) continue;
        // An edit list says where in the track the sound proper begins: the encoder's run-in comes before it.
        const uint8_t *edts_end, *edts = box(trak, trak_end, "edts", &edts_end), *elst_end, *elst = edts ? box(edts, edts_end, "elst", &elst_end) : NULL;
        if (elst && elst[0] == 0 && elst_end - elst >= 20 && be32(elst + 4) >= 1 && (int32_t)be32(elst + 12) > 0) s->lead = be32(elst + 12) / (double)timescale;
        // The sound description's own fields (longer in QuickTime's later versions), then the box with the decoder's configuration.
        const uint8_t *more = entry + 28 + (be16(entry + 8) == 1 ? 16 : be16(entry + 8) == 2 ? 36 : 0), *esds_end, *esds = more < entry_end ? box(more, entry_end, "esds", &esds_end) : NULL;
        bool configured = false;
        for (const uint8_t *q = esds ? esds + 4 : NULL; q && q < esds_end && !configured;) {
            int tag = *q++;
            size_t size = 0;
            for (int i = 0; i < 4 && q < esds_end; i++) {
                size = size << 7 | (*q & 0x7F);
                if (!(*q++ & 0x80)) break;
            }
            if (tag == 3) q += 3;        // the stream's descriptor: its id and flags, then its children
            else if (tag == 4) q += 13;  // the decoder's: its type, buffer and bit rates, then its children
            else if (tag == 5 && size >= 2 && (size_t)(esds_end - q) >= size) {  // the AudioSpecificConfig
                static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
                s->aac_config.rate = rates[(q[0] & 7) << 1 | q[1] >> 7], s->aac_config.channels = (q[1] >> 3 & 15) ? q[1] >> 3 & 15 : 2;
                s->aac_config.asc_len = (int)SDL_min(size, sizeof s->aac_config.asc);
                memcpy(s->aac_config.asc, q, (size_t)s->aac_config.asc_len);
                configured = s->aac_config.rate > 0;
            } else q += size;
        }
        // Samples lie in chunks: stsc says how many to a chunk, stco or co64 where each chunk starts, stsz each sample's size.
        const uint8_t *stsz_end, *stsz = box(stbl, stbl_end, "stsz", &stsz_end), *stsc_end, *stsc = box(stbl, stbl_end, "stsc", &stsc_end);
        const uint8_t *stco_end, *stco = box(stbl, stbl_end, "stco", &stco_end), *stts_end, *stts = box(stbl, stbl_end, "stts", &stts_end);
        bool wide = !stco;
        if (wide) stco = box(stbl, stbl_end, "co64", &stco_end);
        if (!configured) continue;
        // A fragmented file's sample table is empty: its frames are in the moof boxes after the moov.
        if (!stsz || stsz_end - stsz < 12 || !be32(stsz + 8)) {
            const uint8_t *tkhd_end, *tkhd = box(trak, trak_end, "tkhd", &tkhd_end);
            gs_mp4_sample *f = NULL;
            const int n = tkhd && tkhd_end - tkhd >= (tkhd[0] == 1 ? 24 : 16) ? gs_mp4_samples(d, len, be32(tkhd + (tkhd[0] == 1 ? 20 : 12)), &f) : 0;
            for (int i = 0; i < n; i++)
                if (!add_packet(s, (size_t)f[i].at, f[i].size)) { free(f); return false; }
            const uint32_t each = n && f[0].duration ? f[0].duration : 1024;
            free(f);
            if (s->npackets < 1) continue;
            s->kind = AAC, s->channels = s->aac_config.channels, s->packet_seconds = (double)each / timescale, s->preroll = 1;
            s->length = s->npackets * s->packet_seconds;
            return true;
        }
        if (!stsc || !stco || stsc_end - stsc < 8 || stco_end - stco < 8) continue;
        uint32_t fixed = be32(stsz + 4), count = be32(stsz + 8), runs = be32(stsc + 4), chunks = be32(stco + 4), delta = stts && stts_end - stts >= 16 ? be32(stts + 12) : 1024;
        if ((!fixed && (uint64_t)(stsz_end - stsz - 12) < 4ull * count) || (uint64_t)(stsc_end - stsc - 8) < 12ull * runs || (uint64_t)(stco_end - stco - 8) < (wide ? 8ull : 4ull) * chunks) continue;
        uint32_t sample = 0;
        for (uint32_t chunk = 0, run = 0; chunk < chunks && sample < count; chunk++) {
            while (run + 1 < runs && be32(stsc + 8 + 12 * (run + 1)) <= chunk + 1) run++;
            uint64_t at = wide ? be64(stco + 8 + 8 * chunk) : be32(stco + 8 + 4 * chunk);
            for (uint32_t i = 0, per = be32(stsc + 8 + 12 * run + 4); i < per && sample < count; i++, sample++) {
                uint32_t size = fixed ? fixed : be32(stsz + 12 + 4 * sample);
                if (at + size > len || !add_packet(s, (size_t)at, size)) return false;
                at += size;
            }
        }
        if (s->npackets < 1) return false;
        s->kind = AAC, s->channels = s->aac_config.channels, s->packet_seconds = (double)(delta ? delta : 1024) / timescale, s->preroll = 1;
        s->length = duration ? (double)duration / timescale : s->npackets * s->packet_seconds;
        return true;
    }
    return false;
}

// ---- Ogg Opus ----

// Ogg cuts packets into segments of up to 255 bytes and gathers segments into pages, so a packet can
// run across pages: the packets are copied out end to end. The first is the stream's head, the
// second its comments, and the rest are sound.
static bool open_ogg(gs_sound *s, const uint8_t *d, size_t len) {
    s->copy = malloc(len ? len : 1);
    if (!s->copy) return false;
    size_t used = 0, start = 0;
    int index = 0, preskip = 0, channels = 0;
    uint64_t last_granule = 0;
    for (size_t at = 0; at + 27 <= len && !memcmp(d + at, "OggS", 4);) {
        const uint8_t *page = d + at;
        int segments = page[26];
        if (at + 27 + (size_t)segments > len) break;
        uint64_t granule = 0;
        for (int i = 7; i >= 0; i--) granule = granule << 8 | page[6 + i];
        if (granule != UINT64_MAX) last_granule = granule;
        size_t body = at + 27 + (size_t)segments;
        for (int i = 0; i < segments; i++) {
            size_t n = page[27 + i];
            if (body + n > len) return false;
            memcpy(s->copy + used, d + body, n), used += n, body += n;
            if (n == 255) continue;  // the packet goes on in the next segment
            if (index == 0 && !gs_opus_head(s->copy + start, used - start, &channels, &preskip)) return false;
            if (index >= 2 && used > start && !add_packet(s, start, used - start)) return false;
            if (index < 2) used = start;  // (the head and the comments are not kept)
            start = used, index++;
        }
        at = body;
    }
    if (s->npackets < 1) return false;
    s->starts = malloc(sizeof *s->starts * (size_t)(s->npackets + 1));
    if (!s->starts) return false;
    int64_t total = -preskip;
    for (int i = 0; i < s->npackets; i++) {
        s->starts[i] = total;
        int n = opus_packet_get_nb_samples(s->copy + s->packets[i].at, (opus_int32)s->packets[i].len, 48000);
        total += n > 0 ? n : 0;
    }
    s->starts[s->npackets] = total;
    // The last page says where the sound ends, which may be short of its last packet's end.
    if (last_granule && (int64_t)last_granule - preskip < total) total = (int64_t)last_granule - preskip;
    s->kind = OPUS, s->channels = channels, s->decoded_rate = 48000, s->preroll = 4;  // (80 ms for the decoder to converge)
    s->length = total > 0 ? total / 48000.0 : 0;
    return true;
}

// ---- Playing ----

static double packet_time(const gs_sound *s, int i) { return s->starts ? s->starts[i] / 48000.0 : i * s->packet_seconds - s->lead; }

// Makes the decoders anew and sets the next packet and what to drop so that playing resumes at `seconds`.
static void go_to(gs_sound *s, double seconds) {
    seconds = SDL_clamp(seconds, 0, s->length);
    int target = 0;
    if (s->starts) {
        for (int lo = 0, hi = s->npackets - 1; lo <= hi;) {  // the last packet that starts at or before it
            int mid = (lo + hi) / 2;
            if (packet_time(s, mid) <= seconds) target = mid, lo = mid + 1; else hi = mid - 1;
        }
    } else target = SDL_min(s->npackets - 1, (int)((seconds + s->lead) / s->packet_seconds));
    s->next = SDL_max(0, target - s->preroll);
    s->drop = (int64_t)((seconds - packet_time(s, s->next)) * s->decoded_rate + 0.5);
    if (s->drop < 0) s->drop = 0;
    if (s->kind == MP3) mp3dec_init(&s->mp3);
    if (s->kind == OPUS) gs_opus_free(s->opus), s->opus = gs_opus_new(s->channels);
#ifdef GS_HAVE_AAC
    if (s->kind == AAC && s->aac) gs_aac_flush(s->aac);
#endif
    SDL_ClearAudioStream(s->out);
    s->base = seconds, s->played = 0, s->ended = s->drained = false;
}

// Decodes the next packet into the waiting sound, less whatever a seek said to drop.
static void decode_next(gs_sound *s) {
    float lr[5760 * 2];  // (Opus's longest packet, 120 ms at 48 kHz, is the most any of them gives)
    const uint8_t *p = s->data + s->packets[s->next].at;
    int len = (int)s->packets[s->next].len, frames = 0;
    s->next++;
    if (s->kind == MP3) {
        mp3dec_frame_info_t info;
        float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        frames = mp3dec_decode_frame(&s->mp3, p, len, pcm, &info);
        for (int i = 0; i < frames; i++) lr[2 * i] = pcm[i * info.channels], lr[2 * i + 1] = pcm[i * info.channels + (info.channels > 1)];
        if (!frames && info.frame_bytes) {  // a frame whose borrowed bits are not there yet, just after a seek: its time passes in silence
            frames = (int)(s->packet_seconds * s->decoded_rate + 0.5);
            memset(lr, 0, sizeof(float) * 2 * (size_t)frames);
        }
    } else if (s->kind == OPUS) {
        frames = s->opus ? gs_opus_decode(s->opus, p, (size_t)len, lr, 5760) : 0;
    } else {
#ifdef GS_HAVE_AAC
        frames = gs_aac_decode(s->aac, p, (size_t)len, lr, 5760);
#endif
    }
    if (frames <= 0) return;
    int64_t skip = SDL_min(s->drop, (int64_t)frames);
    s->drop -= skip;
    if (frames > skip) SDL_PutAudioStreamData(s->out, lr + 2 * skip, (int)((frames - skip) * 2 * sizeof(float)));
}

void gs_sound_render(void *sound, float *lr, int frames) {
    gs_sound *s = sound;
    SDL_LockMutex(s->lock);
    // No more than its length is played, whatever the decoder gives past the end.
    int64_t most = (int64_t)((s->length - s->base) * s->rate + 0.5) - s->played;
    for (int done = 0; s->playing && done < frames;) {
        float buf[1024 * 2];
        int want = (int)SDL_min((int64_t)SDL_min(frames - done, 1024), most);
        if (want <= 0) { s->ended = true, s->playing = false; break; }
        while (s->next < s->npackets && SDL_GetAudioStreamAvailable(s->out) < (int)(want * 2 * sizeof(float))) decode_next(s);
        if (s->next >= s->npackets && !s->drained) SDL_FlushAudioStream(s->out), s->drained = true;
        int got = SDL_GetAudioStreamData(s->out, buf, (int)(want * 2 * sizeof(float))) / (int)(2 * sizeof(float));
        if (got <= 0) { s->ended = true, s->playing = false; break; }
        for (int i = 0; i < got * 2; i++) lr[2 * done + i] += buf[i];
        done += got, most -= got, s->played += got;
    }
    SDL_UnlockMutex(s->lock);
}

gs_sound *gs_sound_open(const void *data, size_t len, int rate) {
    gs_sound *s = calloc(1, sizeof *s);
    const uint8_t *d = data;
    if (!s || !d || len < 16) return free(s), NULL;
    s->data = d, s->rate = rate > 0 ? rate : 48000, s->decoded_rate = s->rate;
    bool ok = false, aac = false;
#ifdef GS_HAVE_AAC
    aac = true;
#endif
    if (!memcmp(d + 4, "ftyp", 4)) ok = aac && open_mp4(s, d, len);
    else if (!memcmp(d, "OggS", 4)) ok = open_ogg(s, d, len);
    else if (d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) ok = aac && open_adts(s, d, len);
    else ok = open_mp3(s, d, len);
    if (ok && s->kind == OPUS) s->data = s->copy;
#ifdef GS_HAVE_AAC
    if (ok && s->kind == AAC) ok = (s->aac = gs_aac_new(&s->aac_config, s->rate)) != NULL;  // (it hands samples over at the mix's rate)
#endif
    SDL_AudioSpec from = { SDL_AUDIO_F32, 2, s->decoded_rate }, to = { SDL_AUDIO_F32, 2, s->rate };
    if (ok) ok = (s->out = SDL_CreateAudioStream(&from, &to)) && (s->lock = SDL_CreateMutex());
    if (!ok) return gs_sound_free(s), NULL;
    go_to(s, 0);
    return s;
}

void gs_sound_free(gs_sound *s) {
    if (!s) return;
#ifdef GS_HAVE_AAC
    gs_aac_free(s->aac);
#endif
    gs_opus_free(s->opus);
    if (s->out) SDL_DestroyAudioStream(s->out);
    if (s->lock) SDL_DestroyMutex(s->lock);
    free(s->packets), free(s->starts), free(s->copy), free(s);
}

double gs_sound_length(const gs_sound *s) { return s->length; }

void gs_sound_play(gs_sound *s, bool playing) {
    SDL_LockMutex(s->lock);
    if (playing && s->ended) go_to(s, 0);
    s->playing = playing;
    SDL_UnlockMutex(s->lock);
}

bool gs_sound_playing(gs_sound *s) {
    SDL_LockMutex(s->lock);
    bool playing = s->playing;
    SDL_UnlockMutex(s->lock);
    return playing;
}

void gs_sound_seek(gs_sound *s, double seconds) {
    SDL_LockMutex(s->lock);
    go_to(s, seconds);
    SDL_UnlockMutex(s->lock);
}

double gs_sound_position(gs_sound *s) {
    SDL_LockMutex(s->lock);
    double at = SDL_min(s->length, s->base + (double)s->played / s->rate);
    SDL_UnlockMutex(s->lock);
    return at;
}

bool gs_sound_ended(gs_sound *s) {
    SDL_LockMutex(s->lock);
    bool ended = s->ended;
    SDL_UnlockMutex(s->lock);
    return ended;
}
