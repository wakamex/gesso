#include "gs_mp4.h"

#include <stdlib.h>
#include <string.h>

#define TRACKS 4

typedef struct {
    uint32_t id, timescale;
    gs_media_kind kind;
    // H.264: the SPS and PPS in Annex B form, and the NAL length size (1, 2 or 4).
    uint8_t *params;
    size_t params_len;
    int length_size;
    gs_aac_config aac;
    uint32_t default_duration, default_size;  // from trex
    int64_t edit;  // the edit list's media time: where presentation starts, in the track's timescale
} mp4_track;

struct gs_mp4 {
    gs_media_fn *fn;
    void *user;
    mp4_track tracks[TRACKS];
    int ntracks;
    uint8_t *out;  // an access unit rewritten to Annex B
    size_t out_cap;
};

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) << 32 | u32(p + 4); }

// Finds the next box in [*p, end): its type, and its body in [*body, *body_end). Advances *p past it.
static bool next_box(const uint8_t **p, const uint8_t *end, char type[5], const uint8_t **body, const uint8_t **body_end) {
    if (end - *p < 8) return false;
    uint64_t size = u32(*p);
    const uint8_t *b = *p + 8;
    if (size == 1) {
        if (end - *p < 16) return false;
        size = u64(*p + 8), b += 8;
    } else if (size == 0) {
        size = (uint64_t)(end - *p);
    }
    if (size < (uint64_t)(b - *p) || size > (uint64_t)(end - *p)) return false;
    memcpy(type, *p + 4, 4), type[4] = 0;
    *body = b, *body_end = *p + size;
    *p += size;
    return true;
}

// The first box of `type` among the boxes in [p, end); NULL when absent.
static const uint8_t *child(const uint8_t *p, const uint8_t *end, const char *type, const uint8_t **body_end) {
    char t[5];
    const uint8_t *b, *be;
    while (next_box(&p, end, t, &b, &be))
        if (!strcmp(t, type)) return *body_end = be, b;
    return NULL;
}

gs_mp4 *gs_mp4_new(gs_media_fn *fn, void *user) {
    gs_mp4 *m = calloc(1, sizeof *m);
    if (m) m->fn = fn, m->user = user;
    return m;
}

void gs_mp4_free(gs_mp4 *m) {
    if (!m) return;
    for (int i = 0; i < m->ntracks; i++) free(m->tracks[i].params);
    free(m->out);
    free(m);
}

// avcC: the NAL length size and the SPS and PPS, kept as Annex B.
static bool read_avcc(mp4_track *k, const uint8_t *p, const uint8_t *end) {
    if (end - p < 7) return false;
    k->length_size = (p[4] & 3) + 1;
    size_t cap = (size_t)(end - p) * 2, n = 0;
    uint8_t *out = malloc(cap);
    if (!out) return false;
    const uint8_t *q = p + 5;
    for (int set = 0; set < 2 && q < end; set++) {
        int count = set == 0 ? (*q++ & 0x1F) : *q++;
        for (int i = 0; i < count && end - q >= 2; i++) {
            size_t len = u16(q);
            q += 2;
            if ((size_t)(end - q) < len) break;
            memcpy(out + n, "\0\0\0\1", 4), memcpy(out + n + 4, q, len), n += 4 + len;
            q += len;
        }
    }
    free(k->params);
    k->params = out, k->params_len = n;
    return n > 0;
}

// esds: the DecoderSpecificInfo (tag 5) inside the descriptors is the AudioSpecificConfig.
static bool read_esds(mp4_track *k, const uint8_t *p, const uint8_t *end) {
    for (p += 4; p < end; ) {  // (after the full box's version and flags)
        int tag = *p++;
        size_t len = 0;
        for (int i = 0; i < 4 && p < end; i++) {
            len = len << 7 | (*p & 0x7F);
            if (!(*p++ & 0x80)) break;
        }
        if (tag == 3) p += 3;            // ES_Descriptor: ES_ID and flags, then its children
        else if (tag == 4) p += 13;      // DecoderConfigDescriptor: then its children
        else if (tag == 5) {
            if (len < 2 || (size_t)(end - p) < len) return false;
            int index = (p[0] & 7) << 1 | p[1] >> 7, channels = p[1] >> 3 & 15;
            static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
            k->aac.rate = rates[index], k->aac.channels = channels ? channels : 2;
            k->aac.asc_len = len < sizeof k->aac.asc ? (int)len : (int)sizeof k->aac.asc;
            memcpy(k->aac.asc, p, (size_t)k->aac.asc_len);
            return k->aac.rate > 0;
        } else p += len;
    }
    return false;
}

bool gs_mp4_init(gs_mp4 *m, const uint8_t *data, size_t len) {
    const uint8_t *end = data + len, *moov_end, *moov = child(data, end, "moov", &moov_end);
    if (!moov) return false;
    for (int i = 0; i < m->ntracks; i++) free(m->tracks[i].params);
    m->ntracks = 0;
    const uint8_t *p = moov, *b, *be;
    char type[5];
    while (next_box(&p, moov_end, type, &b, &be)) {
        if (!strcmp(type, "mvex")) {  // trex: the defaults for fragments
            const uint8_t *q = b, *tb, *tbe;
            char tt[5];
            while (next_box(&q, be, tt, &tb, &tbe))
                for (int i = 0; !strcmp(tt, "trex") && tbe - tb >= 24 && i < m->ntracks; i++)
                    if (m->tracks[i].id == u32(tb + 4)) m->tracks[i].default_duration = u32(tb + 12), m->tracks[i].default_size = u32(tb + 16);
            continue;
        }
        if (strcmp(type, "trak") || m->ntracks == TRACKS) continue;
        mp4_track k = { 0 };
        const uint8_t *tkhd_end, *tkhd = child(b, be, "tkhd", &tkhd_end);
        const uint8_t *mdia_end, *mdia = child(b, be, "mdia", &mdia_end);
        if (!tkhd || !mdia || tkhd_end - tkhd < 24) continue;
        k.id = u32(tkhd + (tkhd[0] == 1 ? 20 : 12));
        // An edit list shifts the track's presentation: by its decoding delay for H.264 with B frames,
        // by the encoder's priming samples for AAC. Its first non-empty edit gives the shift.
        const uint8_t *edts_end, *edts = child(b, be, "edts", &edts_end);
        const uint8_t *elst_end, *elst = edts ? child(edts, edts_end, "elst", &elst_end) : NULL;
        if (elst && elst_end - elst >= 8) {
            bool v1 = elst[0] == 1;
            const uint8_t *e = elst + 8, *e_end = elst + 8 + (size_t)u32(elst + 4) * (v1 ? 20 : 12);
            for (; e + (v1 ? 20 : 12) <= elst_end && e < e_end; e += v1 ? 20 : 12) {
                int64_t media_time = v1 ? (int64_t)u64(e + 8) : (int64_t)(int32_t)u32(e + 4);
                if (media_time >= 0) { k.edit = media_time; break; }
            }
        }
        const uint8_t *mdhd_end, *mdhd = child(mdia, mdia_end, "mdhd", &mdhd_end);
        const uint8_t *hdlr_end, *hdlr = child(mdia, mdia_end, "hdlr", &hdlr_end);
        const uint8_t *minf_end, *minf = child(mdia, mdia_end, "minf", &minf_end);
        if (!mdhd || !hdlr || !minf || hdlr_end - hdlr < 12) continue;
        k.timescale = u32(mdhd + (mdhd[0] == 1 ? 20 : 12));
        const uint8_t *stbl_end, *stbl = child(minf, minf_end, "stbl", &stbl_end);
        const uint8_t *stsd_end, *stsd = stbl ? child(stbl, stbl_end, "stsd", &stsd_end) : NULL;
        if (!stsd || stsd_end - stsd < 16 || !k.timescale) continue;
        const uint8_t *entry = stsd + 8, *ee;  // (after version, flags and the entry count)
        char fmt[5];
        const uint8_t *eb;
        if (!next_box(&entry, stsd_end, fmt, &eb, &ee)) continue;
        bool ok = false;
        if (!memcmp(hdlr + 8, "vide", 4) && (!strcmp(fmt, "avc1") || !strcmp(fmt, "avc3"))) {
            k.kind = GS_MEDIA_H264;
            const uint8_t *avcc_end, *avcc = ee - eb > 78 ? child(eb + 78, ee, "avcC", &avcc_end) : NULL;
            ok = avcc && read_avcc(&k, avcc, avcc_end);
        } else if (!memcmp(hdlr + 8, "soun", 4) && !strcmp(fmt, "mp4a")) {
            k.kind = GS_MEDIA_AAC;
            const uint8_t *esds_end, *esds = ee - eb > 28 ? child(eb + 28, ee, "esds", &esds_end) : NULL;
            ok = esds && read_esds(&k, esds, esds_end);
        }
        if (ok) m->tracks[m->ntracks++] = k;
        else free(k.params);
    }
    return m->ntracks > 0;
}

// One H.264 sample: its length-prefixed NAL units rewritten as Annex B, after the parameter sets
// when it is a keyframe.
static const uint8_t *annex_b(gs_mp4 *m, const mp4_track *k, const uint8_t *p, size_t n, bool key, size_t *out_len) {
    size_t need = n * 2 + k->params_len + 64;
    if (need > m->out_cap) {
        uint8_t *grown = realloc(m->out, need);
        if (!grown) return NULL;
        m->out = grown, m->out_cap = need;
    }
    size_t o = 0;
    if (key) memcpy(m->out, k->params, k->params_len), o = k->params_len;
    for (size_t at = 0; at + (size_t)k->length_size <= n;) {
        size_t len = 0;
        for (int i = 0; i < k->length_size; i++) len = len << 8 | p[at + i];
        at += (size_t)k->length_size;
        if (len > n - at) break;
        memcpy(m->out + o, "\0\0\0\1", 4), memcpy(m->out + o + 4, p + at, len), o += 4 + len;
        at += len;
    }
    *out_len = o;
    return m->out;
}

// One track fragment: tfhd, tfdt and trun, with the samples' data in the segment.
static void fragment(gs_mp4 *m, const uint8_t *moof, const uint8_t *traf, const uint8_t *traf_end, const uint8_t *data, size_t len) {
    const uint8_t *tfhd_end, *tfhd = child(traf, traf_end, "tfhd", &tfhd_end);
    if (!tfhd || tfhd_end - tfhd < 8) return;
    uint32_t flags = u32(tfhd) & 0xFFFFFF, id = u32(tfhd + 4);
    mp4_track *k = NULL;
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].id == id) k = &m->tracks[i];
    if (!k) return;
    const uint8_t *q = tfhd + 8;
    uint64_t base = (uint64_t)(moof - 8 - data);  // the moof's start, by default (default-base-is-moof)
    uint32_t def_duration = k->default_duration, def_size = k->default_size;
    if (flags & 0x01) base = u64(q), q += 8;
    if (flags & 0x02) q += 4;
    if (flags & 0x08) def_duration = u32(q), q += 4;
    if (flags & 0x10) def_size = u32(q), q += 4;
    const uint8_t *tfdt_end, *tfdt = child(traf, traf_end, "tfdt", &tfdt_end);
    uint64_t dts = tfdt ? (tfdt[0] == 1 ? u64(tfdt + 4) : u32(tfdt + 4)) : 0;
    const uint8_t *p = traf, *b, *be;
    char type[5];
    while (next_box(&p, traf_end, type, &b, &be)) {
        if (strcmp(type, "trun") || be - b < 8) continue;
        uint32_t tf = u32(b) & 0xFFFFFF, count = u32(b + 4);
        bool v1 = b[0] == 1;
        const uint8_t *r = b + 8;
        uint64_t offset = base;
        if (tf & 0x001) offset = base + (uint64_t)(int64_t)(int32_t)u32(r), r += 4;
        uint32_t first_flags = 0;
        bool has_first = tf & 0x004;
        if (has_first) first_flags = u32(r), r += 4;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t duration = def_duration, size = def_size, sflags = 0;
            int64_t cts = 0;
            if (tf & 0x100) duration = u32(r), r += 4;
            if (tf & 0x200) size = u32(r), r += 4;
            if (tf & 0x400) sflags = u32(r), r += 4;
            if (tf & 0x800) cts = v1 ? (int32_t)u32(r) : (int64_t)u32(r), r += 4;
            if (r > be) return;
            if (i == 0 && has_first) sflags = first_flags;
            if (offset + size > len) return;
            bool key = !(sflags & 0x10000);  // sample_is_non_sync_sample clear
            double pts = ((double)(dts + (uint64_t)cts) - (double)k->edit) / k->timescale;
            if (k->kind == GS_MEDIA_H264) {
                size_t n;
                const uint8_t *au = annex_b(m, k, data + offset, size, key, &n);
                if (au) {
                    gs_media_frame f = { GS_MEDIA_H264, au, n, pts, key, NULL };
                    m->fn(m->user, &f);
                }
            } else {
                gs_media_frame f = { GS_MEDIA_AAC, data + offset, size, pts, true, &k->aac };
                m->fn(m->user, &f);
            }
            offset += size, dts += duration;
        }
        base = offset;  // (a second trun continues where the first ended)
    }
}

bool gs_mp4_segment(gs_mp4 *m, const uint8_t *data, size_t len) {
    const uint8_t *p = data, *end = data + len, *b, *be;
    char type[5];
    bool any = false;
    while (next_box(&p, end, type, &b, &be)) {
        if (strcmp(type, "moof")) continue;
        const uint8_t *q = b, *tb, *tbe;
        char tt[5];
        while (next_box(&q, be, tt, &tb, &tbe))
            if (!strcmp(tt, "traf")) fragment(m, b, tb, tbe, data, len), any = true;
    }
    return any;
}
