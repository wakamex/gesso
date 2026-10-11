#include "gs_ts.h"

#include <stdlib.h>
#include <string.h>

#define PIDS 8

typedef struct {
    int pid;
    gs_media_kind kind;
    uint8_t *pes;      // the PES packet being assembled
    size_t len, cap;
    // H.264 with access unit delimiters: the access unit being assembled, which need not end where a
    // PES packet does, its time, and the time between the last two.
    uint8_t *au;
    size_t au_len, au_cap;
    double au_pts, step;
    bool delimited, decided;
    int64_t last;      // the last raw timestamp, for unwrapping
    int64_t wraps;
    gs_aac_config aac;
} track;

struct gs_ts {
    gs_media_fn *fn;
    void *user;
    uint8_t carry[188];  // a packet split between feeds
    size_t ncarry;
    int pmt_pid;
    track tracks[PIDS];
    int ntracks;
    bool bad;
};

gs_ts *gs_ts_new(gs_media_fn *fn, void *user) {
    gs_ts *t = calloc(1, sizeof *t);
    if (t) t->fn = fn, t->user = user, t->pmt_pid = -1;
    return t;
}

void gs_ts_reset(gs_ts *t) {
    for (int i = 0; i < t->ntracks; i++) free(t->tracks[i].pes), free(t->tracks[i].au);
    t->ntracks = 0, t->pmt_pid = -1, t->ncarry = 0;
}

void gs_ts_free(gs_ts *t) {
    if (t) gs_ts_reset(t), free(t);
}

bool gs_aac_config_make(gs_aac_config *c, int rate, int channels) {
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    int index = -1;
    for (int i = 0; i < 13; i++)
        if (rates[i] == rate) index = i;
    if (index < 0 || channels < 1 || channels > 7) return false;
    c->rate = rate, c->channels = channels, c->asc_len = 2;
    c->asc[0] = (uint8_t)(2 << 3 | index >> 1), c->asc[1] = (uint8_t)((index & 1) << 7 | channels << 3);  // AAC-LC
    return true;
}

static int64_t timestamp(const uint8_t *p) {
    return (int64_t)(p[0] & 0x0E) << 29 | (int64_t)p[1] << 22 | (int64_t)(p[2] & 0xFE) << 14 | (int64_t)p[3] << 7 | p[4] >> 1;
}

// Seconds on an unwrapped timeline: a timestamp far below the last one has wrapped past 2^33.
static double unwrap(track *k, int64_t raw) {
    const int64_t span = (int64_t)1 << 33;
    if (k->last >= 0 && raw < k->last - span / 2) k->wraps++;
    else if (k->last >= 0 && raw > k->last + span / 2 && k->wraps > 0) k->wraps--;  // (a frame from before a wrap)
    k->last = raw;
    return (raw + k->wraps * span) / 90000.0;
}

static bool has_idr(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 5) return true;
    return false;
}

// Where the next access unit delimiter's start code begins in p at or after `from`, or n.
static size_t next_delimiter(const uint8_t *p, size_t n, size_t from) {
    for (size_t i = from; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 9) return i && p[i - 1] == 0 ? i - 1 : i;
    return n;
}

static void emit_unit(gs_ts *t, track *k) {
    if (k->au_len) {
        gs_media_frame f = { GS_MEDIA_H264, k->au, k->au_len, k->au_pts, has_idr(k->au, k->au_len), NULL };
        t->fn(t->user, &f);
    }
    k->au_len = 0;
}

// A PES packet's H.264: added to the access unit being assembled, which is handed out each time a
// delimiter starts the next. A packet's time belongs to the first access unit that starts in it,
// and a packet may begin with the end of the one before (Twitch's transcodes do).
static void video(gs_ts *t, track *k, const uint8_t *p, size_t n, double pts) {
    if (k->au_len + n > k->au_cap) {
        size_t cap = (k->au_len + n) * 2;
        uint8_t *grown = realloc(k->au, cap);
        if (!grown) return;
        k->au = grown, k->au_cap = cap;
    }
    // (a start code may straddle two packets; an access unit's own delimiter, in its first bytes, does not end it)
    size_t from = k->au_len > 5 ? k->au_len - 3 : k->au_len ? 2 : 0;
    memcpy(k->au + k->au_len, p, n);
    k->au_len += n;
    for (int started = 0;; started++) {
        size_t at = next_delimiter(k->au, k->au_len, from);
        if (at == k->au_len) break;
        size_t rest = k->au_len - at;
        if (at) k->au_len = at, emit_unit(t, k);
        memmove(k->au, k->au + at, rest);
        k->au_len = rest, from = 2;
        double next = started ? k->au_pts + k->step : pts;  // (a second access unit in one packet has no time of its own)
        if (next > k->au_pts && next - k->au_pts < 1) k->step = next - k->au_pts;
        k->au_pts = next;
    }
}

// A complete PES packet: its timestamp, then its payload as one access unit or a run of ADTS frames.
static void emit(gs_ts *t, track *k) {
    const uint8_t *p = k->pes;
    size_t n = k->len;
    k->len = 0;
    if (n < 9 || p[0] || p[1] || p[2] != 1) return;
    size_t header = 9 + p[8];
    if (header > n || !(p[7] & 0x80)) return;  // (no timestamp: nothing to place it with)
    double pts = unwrap(k, timestamp(p + 9));
    p += header, n -= header;
    if (k->kind == GS_MEDIA_H264) {
        if (!k->decided) k->decided = true, k->delimited = next_delimiter(p, n, 0) < n;
        if (k->delimited) return video(t, k, p, n, pts);
        gs_media_frame f = { GS_MEDIA_H264, p, n, pts, has_idr(p, n), NULL };  // no delimiters: a packet is an access unit
        t->fn(t->user, &f);
        return;
    }
    for (int i = 0; n >= 7 && p[0] == 0xFF && (p[1] & 0xF0) == 0xF0; i++) {  // ADTS frames
        size_t len = (size_t)(p[3] & 3) << 11 | (size_t)p[4] << 3 | p[5] >> 5, head = p[1] & 1 ? 7 : 9;
        if (len < head || len > n) break;
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
        int rate = rates[p[2] >> 2 & 15], channels = (p[2] & 1) << 2 | p[3] >> 6;
        if (rate && (rate != k->aac.rate || channels != k->aac.channels)) gs_aac_config_make(&k->aac, rate, channels ? channels : 2);
        if (k->aac.rate) {
            gs_media_frame f = { GS_MEDIA_AAC, p + head, len - head, pts + i * 1024.0 / k->aac.rate, true, &k->aac };
            t->fn(t->user, &f);
        }
        p += len, n -= len;
    }
}

static track *find(gs_ts *t, int pid) {
    for (int i = 0; i < t->ntracks; i++)
        if (t->tracks[i].pid == pid) return &t->tracks[i];
    return NULL;
}

// The programme map: H.264 (stream type 0x1B) and AAC in ADTS (0x0F) streams; others are skipped.
static void read_pmt(gs_ts *t, const uint8_t *p, size_t n) {
    if (n < 12 || p[0] != 2) return;
    size_t section = (size_t)(p[1] & 0x0F) << 8 | p[2], end = 3 + section - 4, at = 12 + ((size_t)(p[10] & 0x0F) << 8 | p[11]);
    if (end > n) return;
    for (; at + 5 <= end; at += 5 + ((size_t)(p[at + 3] & 0x0F) << 8 | p[at + 4])) {
        int type = p[at], pid = (p[at + 1] & 0x1F) << 8 | p[at + 2];
        if ((type != 0x1B && type != 0x0F) || find(t, pid) || t->ntracks == PIDS) continue;
        t->tracks[t->ntracks++] = (track){ .pid = pid, .kind = type == 0x1B ? GS_MEDIA_H264 : GS_MEDIA_AAC, .last = -1 };
    }
}

static void packet(gs_ts *t, const uint8_t *p) {
    bool start = p[1] & 0x40;
    int pid = (p[1] & 0x1F) << 8 | p[2], afc = p[3] >> 4 & 3;
    size_t at = 4;
    if (afc & 2) at += 1 + p[4];  // the adaptation field
    if (!(afc & 1) || at >= 188) return;
    const uint8_t *payload = p + at;
    size_t n = 188 - at;
    if (pid == 0 || pid == t->pmt_pid) {  // tables start after their pointer field
        if (!start || n < 1 + payload[0]) return;
        const uint8_t *table = payload + 1 + payload[0];
        size_t tn = n - 1 - payload[0];
        if (pid == 0 && tn >= 12 && table[0] == 0) t->pmt_pid = (table[10] & 0x1F) << 8 | table[11];  // the first programme
        else if (pid == t->pmt_pid) read_pmt(t, table, tn);
        return;
    }
    track *k = find(t, pid);
    if (!k) return;
    if (start && k->len) emit(t, k);
    if (!start && !k->len) return;  // (the middle of a packet whose start was missed)
    if (k->len + n > k->cap) {
        size_t cap = (k->len + n) * 2;
        uint8_t *grown = realloc(k->pes, cap);
        if (!grown) return;
        k->pes = grown, k->cap = cap;
    }
    memcpy(k->pes + k->len, payload, n);
    k->len += n;
}

bool gs_ts_feed(gs_ts *t, const uint8_t *data, size_t len) {
    if (t->bad) return false;
    while (len) {
        if (t->ncarry) {  // finish a packet split between feeds
            size_t k = 188 - t->ncarry < len ? 188 - t->ncarry : len;
            memcpy(t->carry + t->ncarry, data, k), t->ncarry += k, data += k, len -= k;
            if (t->ncarry < 188) return true;
            t->ncarry = 0;
            if (t->carry[0] != 0x47) return !(t->bad = true);
            packet(t, t->carry);
            continue;
        }
        if (len < 188) {
            memcpy(t->carry, data, len), t->ncarry = len;
            return true;
        }
        if (data[0] != 0x47) return !(t->bad = true);
        packet(t, data);
        data += 188, len -= 188;
    }
    return true;
}

void gs_ts_end(gs_ts *t) {
    for (int i = 0; i < t->ntracks; i++) {
        if (t->tracks[i].len) emit(t, &t->tracks[i]);
        emit_unit(t, &t->tracks[i]);
    }
}
