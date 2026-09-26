#include "gs_synth.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define PI 3.14159265358979323846

// ---- Generators and modulators (numbers from the SoundFont 2.04 specification) ----

enum {
    G_START_OFFSET = 0, G_START_LOOP_OFFSET = 2, G_END_LOOP_OFFSET = 3, G_START_COARSE = 4,
    G_VIB_TO_PITCH = 6, G_MOD_ENV_TO_PITCH = 7, G_FILTER_FC = 8, G_FILTER_Q = 9, G_MOD_ENV_TO_FILTER = 11,
    G_END_COARSE = 12, G_REVERB_SEND = 16, G_PAN = 17, G_DELAY_VIB = 23, G_FREQ_VIB = 24,
    G_DELAY_MOD = 25, G_ATTACK_MOD = 26, G_HOLD_MOD = 27, G_DECAY_MOD = 28, G_SUSTAIN_MOD = 29,
    G_RELEASE_MOD = 30, G_KEY_TO_MOD_HOLD = 31, G_KEY_TO_MOD_DECAY = 32, G_DELAY_VOL = 33,
    G_ATTACK_VOL = 34, G_HOLD_VOL = 35, G_DECAY_VOL = 36, G_SUSTAIN_VOL = 37, G_RELEASE_VOL = 38,
    G_KEY_TO_HOLD = 39, G_KEY_TO_DECAY = 40, G_INSTRUMENT = 41, G_KEY_RANGE = 43, G_VEL_RANGE = 44,
    G_START_LOOP_COARSE = 45, G_KEYNUM = 46, G_VELOCITY = 47, G_ATTENUATION = 48,
    G_END_LOOP_COARSE = 50, G_COARSE_TUNE = 51, G_FINE_TUNE = 52, G_SAMPLE_ID = 53,
    G_SAMPLE_MODES = 54, G_SCALE_TUNING = 56, G_EXCLUSIVE_CLASS = 57, G_ROOT_KEY = 58, G_COUNT = 61,
};

// Preset generators are offsets added to the instrument's, except these, which select zones.
static bool additive(int g) {
    switch (g) {
    case G_START_OFFSET: case 1: case G_START_LOOP_OFFSET: case G_END_LOOP_OFFSET: case G_START_COARSE:
    case G_END_COARSE: case G_END_LOOP_COARSE: case G_START_LOOP_COARSE: case G_INSTRUMENT: case G_KEY_RANGE:
    case G_VEL_RANGE: case G_KEYNUM: case G_VELOCITY: case G_SAMPLE_ID: case G_SAMPLE_MODES:
    case G_EXCLUSIVE_CLASS: case G_ROOT_KEY:
        return false;
    }
    return true;
}

typedef struct { uint16_t src, dest; int amount; uint16_t amt_src, trans; } mod_t;

// SF2 2.04 default modulators as SpessaSynth uses them (velocity and CC7/CC11 to attenuation, CC10
// to pan, CC91/93 to the sends) plus its CC73 attack and CC72 release, neutral at 64.
static const mod_t DEFAULT_MODS[] = {
    { 0x0502, 48, 960, 0, 0 }, { 0x0587, 48, 960, 0, 0 }, { 0x058b, 48, 960, 0, 0 },
    { 0x028a, 17, 500, 0, 0 }, { 0x00db, 16, 200, 0, 0 }, { 0x00dd, 15, 200, 0, 0 },
    { 0x0ac9, 34, 6000, 0, 0 }, { 0x02c8, 38, 3600, 0, 0 },
};
#define NDEFAULT_MODS (int)(sizeof DEFAULT_MODS / sizeof *DEFAULT_MODS)
#define MAX_MODS 64

static bool same_mod(const mod_t *a, const mod_t *b) { return a->src == b->src && a->dest == b->dest && a->amt_src == b->amt_src; }

static double curve(int type, double x) {
    if (type == 1) return x >= 1 ? 1 : fmin(1, -400.0 / 960 * log10(1 - x));      // concave
    if (type == 2) return x <= 0 ? 0 : 1 - fmin(1, -400.0 / 960 * log10(x));      // convex
    if (type == 3) return x >= 0.5 ? 1 : 0;                                         // switch
    return x;
}

// Value of a modulator source in [-1, 1] from the note and the channel's controllers.
static double source_value(int src, int note, int vel, const uint8_t *cc) {
    int index = src & 127;
    double x;
    if (src & 128) x = cc[index] / 127.0;
    else if (index == 0) return 1;  // no source
    else if (index == 2) x = vel / 127.0;
    else if (index == 3) x = note / 127.0;
    else return 0;  // pressure, pitch wheel and links are not tracked
    if (src & 0x100) x = 1 - x;
    int type = src >> 10;
    if (!(src & 0x200)) return curve(type, x);
    double v = 2 * x - 1;
    return (v < 0 ? -1 : 1) * curve(type, fabs(v));
}

// ---- Bank ----

typedef struct {
    int16_t gen[G_COUNT];
    uint64_t has;  // bit g set when the zone sets generator g
    const mod_t *mods;
    int nmods;
    int ref;  // instrument (preset zone) or sample (instrument zone); -1 in a global zone
} zone_t;

typedef struct {
    zone_t *global;
    zone_t *zones;
    int n;
} zoneset;

typedef struct {
    int program, bank;
    zoneset z;
} preset_t;

typedef struct {
    float *data;
    int len, rate, root, correction;
    int loop_start, loop_end;  // in frames from the start of data
} sample_t;

struct gs_bank {
    preset_t *presets;
    int npresets;
    zoneset *inst;
    int ninst;
    sample_t *samples;
    int nsamples;
    zone_t *zones;
    mod_t *mods;
};

typedef struct { const uint8_t *p; uint32_t size; } chunk;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Finds a sub-chunk by id (or by list type, for LIST chunks) among the chunks in [p, end).
static chunk find(const uint8_t *p, const uint8_t *end, const char *id) {
    while (p + 8 <= end) {
        uint32_t size = u32(p + 4);
        if (p + 8 + size > end) break;
        if (!memcmp(p, id, 4) || (!memcmp(p, "LIST", 4) && size >= 4 && !memcmp(p + 8, id, 4)))
            return memcmp(p, "LIST", 4) ? (chunk){ p + 8, size } : (chunk){ p + 12, size - 4 };
        p += 8 + size + (size & 1);
    }
    return (chunk){ NULL, 0 };
}

// Builds zones from bag records [first, next) and appends them to *out.
static void build_zones(const chunk *bag, const chunk *gen, const mod_t *mods, int first, int next, int ref_gen,
                        zone_t *out, int *nout) {
    int nbags = (int)(bag->size / 4), ngens = (int)(gen->size / 4);
    for (int b = first; b < next && b + 1 < nbags; b++) {
        zone_t *z = &out[(*nout)++];
        memset(z, 0, sizeof *z);
        int g0 = u16(bag->p + 4 * b), g1 = u16(bag->p + 4 * (b + 1));
        int m0 = u16(bag->p + 4 * b + 2), m1 = u16(bag->p + 4 * (b + 1) + 2);
        for (int i = g0; i < g1 && i < ngens; i++) {
            int oper = u16(gen->p + 4 * i);
            if (oper < G_COUNT) z->gen[oper] = (int16_t)u16(gen->p + 4 * i + 2), z->has |= 1ull << oper;
        }
        z->mods = mods + m0;
        z->nmods = m1 - m0;
        z->ref = z->has >> ref_gen & 1 ? (uint16_t)z->gen[ref_gen] : -1;
    }
}

// Reads the preset and instrument modulator lists into one array: presets first, then instruments.
static mod_t *read_mods(chunk pmod, chunk imod, int *npm) {
    *npm = (int)(pmod.size / 10);
    int n = *npm + (int)(imod.size / 10);
    mod_t *m = calloc((size_t)n + 1, sizeof *m);
    for (int i = 0; i < n; i++) {
        const uint8_t *p = i < *npm ? pmod.p + 10 * i : imod.p + 10 * (i - *npm);
        m[i] = (mod_t){ u16(p), u16(p + 2), (int16_t)u16(p + 4), u16(p + 6), u16(p + 8) };
    }
    return m;
}

// Splits [zones, zones + n) into an optional global zone (the first, without a reference) and the rest.
static zoneset with_global(zone_t *zones, int n) {
    if (n && zones[0].ref < 0) return (zoneset){ zones, zones + 1, n - 1 };
    return (zoneset){ NULL, zones, n };
}

static bool decode_sample(sample_t *s, const uint8_t *smpl, uint32_t smpl_size, const uint8_t *h) {
    uint32_t start = u32(h + 20), end = u32(h + 24), ls = u32(h + 28), le = u32(h + 32);
    s->rate = (int)u32(h + 36);
    s->root = h[40];
    s->correction = (int8_t)h[41];
    if (u16(h + 44) & 0x10) {  // compressed: an Ogg Vorbis file at byte offsets, loops relative to it
        if (end > smpl_size || start >= end) return false;
        int channels = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_memory(smpl + start, (int)(end - start), &channels, &rate, &pcm);
        if (frames <= 0) return false;
        s->data = malloc(sizeof(float) * ((size_t)frames + 1));
        for (int i = 0; i < frames; i++) s->data[i] = pcm[(size_t)i * channels] / 32768.0f;
        free(pcm);
        s->len = frames;
        s->loop_start = (int)ls, s->loop_end = (int)le;
    } else {  // 16-bit PCM frames in smpl
        if ((uint64_t)end * 2 > smpl_size || start > end) return false;
        s->len = (int)(end - start);
        s->data = malloc(sizeof(float) * ((size_t)s->len + 1));
        for (int i = 0; i < s->len; i++) s->data[i] = (int16_t)u16(smpl + 2 * ((size_t)start + i)) / 32768.0f;
        s->loop_start = (int)(ls - start), s->loop_end = (int)(le - start);
    }
    s->data[s->len] = 0;  // lets interpolation read one past the end
    return true;
}

gs_bank *gs_bank_load(const void *data, size_t len) {
    const uint8_t *p = data, *end = p + len;
    if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "sfbk", 4)) return NULL;
    chunk sdta = find(p + 12, end, "sdta"), pdta = find(p + 12, end, "pdta");
    if (!sdta.p || !pdta.p) return NULL;
    chunk smpl = find(sdta.p, sdta.p + sdta.size, "smpl");
    const uint8_t *pe = pdta.p + pdta.size;
    chunk phdr = find(pdta.p, pe, "phdr"), pbag = find(pdta.p, pe, "pbag"), pmod = find(pdta.p, pe, "pmod");
    chunk pgen = find(pdta.p, pe, "pgen"), inst = find(pdta.p, pe, "inst"), ibag = find(pdta.p, pe, "ibag");
    chunk imod = find(pdta.p, pe, "imod"), igen = find(pdta.p, pe, "igen"), shdr = find(pdta.p, pe, "shdr");
    if (!smpl.p || !phdr.p || !pbag.p || !pgen.p || !inst.p || !ibag.p || !igen.p || !shdr.p) return NULL;

    gs_bank *b = calloc(1, sizeof *b);
    int npm;
    b->mods = read_mods(pmod, imod, &npm);
    mod_t *pmods = b->mods, *imods = b->mods + npm;
    b->zones = calloc(pbag.size / 4 + ibag.size / 4 + 2, sizeof *b->zones);
    int nz = 0;

    b->ninst = (int)(inst.size / 22) - 1;
    b->inst = calloc((size_t)(b->ninst > 0 ? b->ninst : 1), sizeof *b->inst);
    for (int i = 0; i < b->ninst; i++) {
        int first = nz;
        build_zones(&ibag, &igen, imods, u16(inst.p + 22 * i + 20), u16(inst.p + 22 * (i + 1) + 20), G_SAMPLE_ID, b->zones, &nz);
        b->inst[i] = with_global(b->zones + first, nz - first);
    }
    b->npresets = (int)(phdr.size / 38) - 1;
    b->presets = calloc((size_t)(b->npresets > 0 ? b->npresets : 1), sizeof *b->presets);
    for (int i = 0; i < b->npresets; i++) {
        const uint8_t *h = phdr.p + 38 * i;
        int first = nz;
        build_zones(&pbag, &pgen, pmods, u16(h + 24), u16(h + 38 + 24), G_INSTRUMENT, b->zones, &nz);
        b->presets[i] = (preset_t){ u16(h + 20), u16(h + 22), with_global(b->zones + first, nz - first) };
    }
    b->nsamples = (int)(shdr.size / 46) - 1;
    b->samples = calloc((size_t)(b->nsamples > 0 ? b->nsamples : 1), sizeof *b->samples);
    for (int i = 0; i < b->nsamples; i++)
        if (!decode_sample(&b->samples[i], smpl.p, smpl.size, shdr.p + 46 * i)) b->samples[i] = (sample_t){ 0 };
    return b;
}

void gs_bank_free(gs_bank *b) {
    if (!b) return;
    for (int i = 0; i < b->nsamples; i++) free(b->samples[i].data);
    free(b->samples);
    free(b->presets);
    free(b->inst);
    free(b->zones);
    free(b->mods);
    free(b);
}

// ---- Voices ----

static double secs(double tc) { return pow(2, tc / 1200); }
static double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

typedef struct {
    bool on, released, filtered;
    int channel, note;
    uint64_t serial;
    const sample_t *s;
    double pos, step, t, dt;  // position in frames, base step, time since note-on, seconds per frame
    bool loop;
    double loop_start, loop_end;
    float gl, gr, send;
    // Volume envelope.
    double peak, sustain, t0, t_peak, t_decay, decay_time, release;
    double rel_t, rel_from, rel_end;
    // Modulation envelope, to pitch and filter cutoff in cents.
    bool mod_env;
    double m0, m_peak, m_decay, m_end, m_sustain, m_release, m_rel_level, to_pitch, to_filter;
    // Vibrato.
    double vib_depth, vib_freq, vib_delay;
    // Low-pass filter (biquad, direct form I) and its settings.
    double fc_hz, q_db, b0, b1, b2, a1, a2, x1, x2, y1, y2;
    double detune;  // cents, updated at control rate
} voice_t;

#define MAX_VOICES 64
#define CONTROL 32  // frames between pitch and filter updates

// Freeverb: parallel combs into series allpasses, per channel, with a small stereo offset.
#define NCOMB 8
#define NALL 4
typedef struct {
    float *buf;
    int len, i;
    float store;
} delay_t;

struct gs_synth {
    const gs_bank *bank;
    int rate;
    uint64_t serial;
    voice_t v[MAX_VOICES];
    struct { uint8_t cc[128]; int bank, program; } ch[16];
    delay_t comb[2][NCOMB], all[2][NALL];
    float feedback, damp, wet, mix;
    float *rev_in;  // stereo scratch for the reverb send
    int rev_cap;
};

// Envelope level at voice time t before release: linear attack, hold, decay exponential in gain.
static double level(const voice_t *v, double t) {
    if (t <= v->t0) return 0;
    if (t < v->t_peak) return v->peak * (t - v->t0) / (v->t_peak - v->t0);
    if (t <= v->t_decay || v->decay_time <= 0) return v->peak;
    double f = fmin(1, (t - v->t_decay) / v->decay_time);
    return v->peak * pow(v->sustain / v->peak, f);
}

static double envelope(const voice_t *v, double t) {
    if (!v->released || t < v->rel_t) return level(v, t);
    if (t >= v->rel_end) return 0;
    return v->rel_from * pow(1e-5 / v->rel_from, (t - v->rel_t) / (v->rel_end - v->rel_t));
}

static double mod_level(const voice_t *v, double t) {
    if (t <= v->m0) return 0;
    if (t < v->m_peak) return (t - v->m0) / (v->m_peak - v->m0);
    if (t <= v->m_decay) return 1;
    return t >= v->m_end ? v->m_sustain : 1 - (1 - v->m_sustain) * (t - v->m_decay) / (v->m_end - v->m_decay);
}

static double mod_envelope(const voice_t *v, double t) {
    if (!v->released || t < v->rel_t) return mod_level(v, t);
    double span = v->m_release * v->m_rel_level;
    if (span <= 0) return 0;
    return fmax(0, v->m_rel_level * (1 - (t - v->rel_t) / span));
}

// Low-pass biquad as Web Audio defines it (Q in decibels).
static void set_filter(voice_t *v, double hz, int rate) {
    hz = clampd(hz, 10, rate / 2.0 - 1);
    double w0 = 2 * PI * hz / rate, cw = cos(w0), alpha = sin(w0) / (2 * pow(10, v->q_db / 20));
    double a0 = 1 + alpha;
    v->b0 = (1 - cw) / 2 / a0, v->b1 = (1 - cw) / a0, v->b2 = v->b0;
    v->a1 = -2 * cw / a0, v->a2 = (1 - alpha) / a0;
}

static void release(voice_t *v) {
    if (v->released) return;
    double t = v->t;
    v->rel_from = fmax(level(v, t), 1e-5);
    double drop_db = 100 + 20 * log10(v->rel_from / v->peak);
    v->rel_t = t;
    v->rel_end = t + v->release * clampd(drop_db / 100, 0.01, 1);
    if (v->mod_env) v->m_rel_level = mod_level(v, t);
    v->released = true;
}

static bool in_range(const zone_t *z, int note, int vel) {
    uint16_t k = z->has >> G_KEY_RANGE & 1 ? (uint16_t)z->gen[G_KEY_RANGE] : 0x7f00;
    uint16_t r = z->has >> G_VEL_RANGE & 1 ? (uint16_t)z->gen[G_VEL_RANGE] : 0x7f00;
    return note >= (k & 255) && note <= k >> 8 && vel >= (r & 255) && vel <= r >> 8;
}

// Zone generators with local entries overriding global ones.
static void merge_gens(const zone_t *global, const zone_t *local, int16_t gen[G_COUNT], uint64_t *has) {
    *has = 0;
    const zone_t *zs[2] = { global, local };
    for (int k = 0; k < 2; k++) {
        if (!zs[k]) continue;
        for (int g = 0; g < G_COUNT; g++)
            if (zs[k]->has >> g & 1) gen[g] = zs[k]->gen[g], *has |= 1ull << g;
    }
}

// Zone modulators: the global list with identical local ones replaced, and new local ones added.
static int merge_mods(const zone_t *global, const zone_t *local, mod_t *out) {
    int n = 0;
    if (global) for (int i = 0; i < global->nmods && n < MAX_MODS; i++) out[n++] = global->mods[i];
    for (int i = 0; i < local->nmods; i++) {
        int j = 0;
        while (j < n && !same_mod(&out[j], &local->mods[i])) j++;
        if (j < n) out[j] = local->mods[i];
        else if (n < MAX_MODS) out[n++] = local->mods[i];
    }
    return n;
}

static void start_voice(gs_synth *sy, int ch, int note, int vel, const preset_t *p, const zone_t *pz, const zone_t *iz) {
    const gs_bank *b = sy->bank;
    if (iz->ref < 0 || iz->ref >= b->nsamples || !b->samples[iz->ref].data) return;
    const sample_t *s = &b->samples[iz->ref];

    double g[G_COUNT] = { 0 };
    g[G_FILTER_FC] = 13500;
    int neg[] = { G_DELAY_VOL, G_ATTACK_VOL, G_HOLD_VOL, G_DECAY_VOL, G_RELEASE_VOL, G_DELAY_VIB,
                  G_DELAY_MOD, G_ATTACK_MOD, G_HOLD_MOD, G_DECAY_MOD, G_RELEASE_MOD };
    for (size_t i = 0; i < sizeof neg / sizeof *neg; i++) g[neg[i]] = -12000;
    g[G_SCALE_TUNING] = 100;
    g[G_ROOT_KEY] = g[G_KEYNUM] = g[G_VELOCITY] = -1;

    int16_t gen[G_COUNT];
    uint64_t has;
    merge_gens(b->inst[pz->ref].global, iz, gen, &has);
    for (int k = 0; k < G_COUNT; k++) if (has >> k & 1) g[k] = gen[k];
    merge_gens(p->z.global, pz, gen, &has);
    for (int k = 0; k < G_COUNT; k++) if (has >> k & 1 && additive(k)) g[k] += gen[k];
    g[G_ATTENUATION] *= 0.4;

    // Instrument modulators replace defaults; preset modulators add to identical ones.
    mod_t mods[MAX_MODS + NDEFAULT_MODS], tmp[MAX_MODS];
    int n = NDEFAULT_MODS;
    memcpy(mods, DEFAULT_MODS, sizeof DEFAULT_MODS);
    int ni = merge_mods(b->inst[pz->ref].global, iz, tmp);
    for (int i = 0; i < ni; i++) {
        int j = 0;
        while (j < n && !same_mod(&mods[j], &tmp[i])) j++;
        if (j < n) mods[j] = tmp[i]; else mods[n++] = tmp[i];
    }
    int np = merge_mods(p->z.global, pz, tmp);
    for (int i = 0; i < np; i++) {
        int j = 0;
        while (j < n && !same_mod(&mods[j], &tmp[i])) j++;
        if (j < n) mods[j].amount += tmp[i].amount; else mods[n++] = tmp[i];
    }
    int key = g[G_KEYNUM] >= 0 ? (int)g[G_KEYNUM] : note, v = g[G_VELOCITY] >= 0 ? (int)g[G_VELOCITY] : vel;
    const uint8_t *cc = sy->ch[ch].cc;
    for (int i = 0; i < n; i++) {
        double x = source_value(mods[i].src, key, v, cc) * source_value(mods[i].amt_src, key, v, cc) * mods[i].amount;
        if (mods[i].trans == 2) x = fabs(x);
        if (mods[i].dest < G_COUNT) g[mods[i].dest] += x;
    }

    // A free voice, or the oldest (released first).
    voice_t *vo = NULL;
    for (int i = 0; i < MAX_VOICES && !vo; i++) if (!sy->v[i].on) vo = &sy->v[i];
    for (int pass = 0; pass < 2 && !vo; pass++)
        for (int i = 0; i < MAX_VOICES; i++)
            if ((pass || sy->v[i].released) && (!vo || sy->v[i].serial < vo->serial)) vo = &sy->v[i];
    memset(vo, 0, sizeof *vo);
    vo->on = true;
    vo->channel = ch, vo->note = note, vo->serial = ++sy->serial;
    vo->s = s;
    vo->dt = 1.0 / sy->rate;

    int root = g[G_ROOT_KEY] >= 0 ? (int)g[G_ROOT_KEY] : s->root;
    double cents = (key - root) * g[G_SCALE_TUNING] + g[G_COARSE_TUNE] * 100 + g[G_FINE_TUNE] + s->correction;
    vo->step = pow(2, cents / 1200) * s->rate / sy->rate;
    vo->pos = fmax(0, g[G_START_OFFSET] + 32768 * g[G_START_COARSE]);
    if ((int)g[G_SAMPLE_MODES] & 1) {
        vo->loop = true;
        vo->loop_start = s->loop_start + g[G_START_LOOP_OFFSET] + 32768 * g[G_START_LOOP_COARSE];
        vo->loop_end = s->loop_end + g[G_END_LOOP_OFFSET] + 32768 * g[G_END_LOOP_COARSE];
        if (!(vo->loop_start >= 0 && vo->loop_start < vo->loop_end && vo->loop_end <= s->len))
            vo->loop_start = 0, vo->loop_end = s->len;  // as Web Audio does with invalid loop points
    }

    double fc = g[G_FILTER_FC];
    vo->filtered = fc < 13500 || g[G_MOD_ENV_TO_FILTER];
    vo->fc_hz = fmin(8.176 * secs(fc), sy->rate / 2.0 - 1);
    vo->q_db = fmax(0, g[G_FILTER_Q]) / 10;
    if (vo->filtered) set_filter(vo, vo->fc_hz, sy->rate);

    if (g[G_VIB_TO_PITCH]) {
        vo->vib_depth = g[G_VIB_TO_PITCH];
        vo->vib_freq = 8.176 * secs(g[G_FREQ_VIB]);
        vo->vib_delay = secs(g[G_DELAY_VIB]);
    }
    vo->to_pitch = g[G_MOD_ENV_TO_PITCH];
    vo->to_filter = vo->filtered ? g[G_MOD_ENV_TO_FILTER] : 0;
    if (vo->to_pitch || vo->to_filter) {
        vo->mod_env = true;
        vo->m0 = secs(g[G_DELAY_MOD]);
        vo->m_peak = vo->m0 + secs(g[G_ATTACK_MOD]);
        vo->m_decay = vo->m_peak + secs(g[G_HOLD_MOD] + g[G_KEY_TO_MOD_HOLD] * (60 - key));
        vo->m_sustain = 1 - clampd(g[G_SUSTAIN_MOD], 0, 1000) / 1000;
        vo->m_end = vo->m_decay + secs(g[G_DECAY_MOD] + g[G_KEY_TO_MOD_DECAY] * (60 - key)) * (1 - vo->m_sustain);
        vo->m_release = secs(g[G_RELEASE_MOD]);
    }

    double pan = clampd(g[G_PAN] / 500, -1, 1), x = (pan + 1) / 2;
    vo->gl = (float)cos(x * PI / 2), vo->gr = (float)sin(x * PI / 2);
    vo->send = (float)clampd(g[G_REVERB_SEND] / 1000, 0, 1);

    vo->peak = pow(10, -fmax(0, g[G_ATTENUATION]) / 200);
    double sustain_db = clampd(g[G_SUSTAIN_VOL] / 10, 0, 144);
    vo->sustain = vo->peak * pow(10, -sustain_db / 20);
    vo->t0 = secs(g[G_DELAY_VOL]);
    vo->t_peak = vo->t0 + secs(g[G_ATTACK_VOL]);
    vo->t_decay = vo->t_peak + secs(g[G_HOLD_VOL] + g[G_KEY_TO_HOLD] * (60 - key));
    vo->decay_time = secs(g[G_DECAY_VOL] + g[G_KEY_TO_DECAY] * (60 - key)) * sustain_db / 100;
    vo->release = secs(g[G_RELEASE_VOL]);
}

static const preset_t *find_preset(const gs_synth *sy, int ch) {
    const gs_bank *b = sy->bank;
    int bank = sy->ch[ch].bank, program = sy->ch[ch].program;
    for (int i = 0; i < b->npresets; i++)
        if (b->presets[i].bank == bank && b->presets[i].program == program) return &b->presets[i];
    int fallback = bank == 128 ? 128 : 0;
    for (int i = 0; i < b->npresets; i++)
        if (b->presets[i].bank == fallback && b->presets[i].program == program) return &b->presets[i];
    return NULL;
}

static void note_on(gs_synth *sy, int ch, int note, int vel) {
    const preset_t *p = find_preset(sy, ch);
    if (!p) return;
    for (int i = 0; i < p->z.n; i++) {
        const zone_t *pz = &p->z.zones[i];
        if (!in_range(pz, note, vel) || pz->ref < 0 || pz->ref >= sy->bank->ninst) continue;
        const zoneset *inst = &sy->bank->inst[pz->ref];
        for (int j = 0; j < inst->n; j++)
            if (in_range(&inst->zones[j], note, vel)) start_voice(sy, ch, note, vel, p, pz, &inst->zones[j]);
    }
}

static void reset_channels(gs_synth *sy) {
    for (int i = 0; i < 16; i++) {
        memset(sy->ch[i].cc, 0, 128);
        sy->ch[i].cc[7] = 100, sy->ch[i].cc[10] = 64, sy->ch[i].cc[11] = 127;
        sy->ch[i].cc[72] = 64, sy->ch[i].cc[73] = 64;
        sy->ch[i].bank = i == 9 ? 128 : 0;
        sy->ch[i].program = 0;
    }
}

void gs_synth_midi(gs_synth *sy, uint8_t status, uint8_t a, uint8_t b) {
    int ch = status & 15;
    switch (status & 0xf0) {
    case 0x80: break;
    case 0x90: if (b) { note_on(sy, ch, a, b); return; } break;
    case 0xb0: if (a == 0) sy->ch[ch].bank = ch == 9 ? 128 : b; else sy->ch[ch].cc[a & 127] = b; return;
    case 0xc0: sy->ch[ch].program = a; return;
    default: return;
    }
    for (int i = 0; i < MAX_VOICES; i++)
        if (sy->v[i].on && sy->v[i].channel == ch && sy->v[i].note == a) release(&sy->v[i]);
}

void gs_synth_all_off(gs_synth *sy) {
    for (int i = 0; i < MAX_VOICES; i++) sy->v[i].on = false;
    reset_channels(sy);
}

void gs_synth_set_reverb(gs_synth *sy, float mix) { sy->mix = mix; }

int gs_synth_active_voices(const gs_synth *sy) {
    int n = 0;
    for (int i = 0; i < MAX_VOICES; i++) n += sy->v[i].on;
    return n;
}

// Renders one voice, adding into lr and the reverb send. Returns false when the voice has ended.
static bool render_voice(gs_synth *sy, voice_t *v, float *lr, float *rev, int frames) {
    const float *d = v->s->data;
    int len = v->s->len;
    for (int i = 0; i < frames; i++) {
        if (i % CONTROL == 0) {
            double cents = 0, me = v->mod_env ? mod_envelope(v, v->t) : 0;
            if (v->vib_depth && v->t >= v->vib_delay) {
                double ph = fmod((v->t - v->vib_delay) * v->vib_freq, 1.0);
                cents += v->vib_depth * (ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4);
            }
            cents += me * v->to_pitch;
            v->detune = pow(2, cents / 1200);
            if (v->to_filter) set_filter(v, v->fc_hz * pow(2, me * v->to_filter / 1200), sy->rate);
        }
        if (v->released && v->t >= v->rel_end) return false;
        if (!v->loop && v->pos >= len) return false;
        int k = (int)v->pos;
        double frac = v->pos - k;
        double next = v->loop && k + 1 >= v->loop_end ? d[(int)v->loop_start] : d[k + 1];
        double x = d[k] + (next - d[k]) * frac;
        if (v->filtered) {
            double y = v->b0 * x + v->b1 * v->x1 + v->b2 * v->x2 - v->a1 * v->y1 - v->a2 * v->y2;
            v->x2 = v->x1, v->x1 = x, v->y2 = v->y1, v->y1 = y;
            x = y;
        }
        float out = (float)(x * envelope(v, v->t));
        lr[2 * i] += out * v->gl;
        lr[2 * i + 1] += out * v->gr;
        if (v->send > 0) rev[2 * i] += out * v->gl * v->send, rev[2 * i + 1] += out * v->gr * v->send;
        v->pos += v->step * v->detune;
        if (v->loop && v->pos >= v->loop_end) v->pos -= v->loop_end - v->loop_start;
        v->t += v->dt;
    }
    return true;
}

static float delay_comb(delay_t *c, float in, float feedback, float damp) {
    float y = c->buf[c->i];
    c->store = y * (1 - damp) + c->store * damp;
    c->buf[c->i] = in + c->store * feedback;
    if (++c->i >= c->len) c->i = 0;
    return y;
}

static float delay_allpass(delay_t *a, float in) {
    float y = a->buf[a->i];
    a->buf[a->i] = in + y * 0.5f;
    if (++a->i >= a->len) a->i = 0;
    return y - in;
}

void gs_synth_render(gs_synth *sy, float *lr, int frames) {
    if (frames > sy->rev_cap) {
        sy->rev_cap = frames;
        sy->rev_in = realloc(sy->rev_in, sizeof(float) * 2 * (size_t)frames);
    }
    float *rev = sy->rev_in;
    memset(rev, 0, sizeof(float) * 2 * (size_t)frames);
    for (int i = 0; i < MAX_VOICES; i++)
        if (sy->v[i].on && !render_voice(sy, &sy->v[i], lr, rev, frames)) sy->v[i].on = false;
    for (int i = 0; i < frames; i++) {
        float in = (rev[2 * i] + rev[2 * i + 1]) * 0.015f;
        for (int c = 0; c < 2; c++) {
            float y = 0;
            for (int k = 0; k < NCOMB; k++) y += delay_comb(&sy->comb[c][k], in, sy->feedback, sy->damp);
            for (int k = 0; k < NALL; k++) y = delay_allpass(&sy->all[c][k], y);
            lr[2 * i + c] += y * sy->wet * sy->mix;
        }
    }
}

gs_synth *gs_synth_new(const gs_bank *b, int rate) {
    static const int comb_len[NCOMB] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    static const int all_len[NALL] = { 556, 441, 341, 225 };
    gs_synth *sy = calloc(1, sizeof *sy);
    sy->bank = b;
    sy->rate = rate;
    // A plain hall of about two seconds, darkening as it fades.
    sy->feedback = 0.84f, sy->damp = 0.3f, sy->wet = 0.56f, sy->mix = 1;  // wet level calibrated against a convolution hall
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < NCOMB; k++) {
            sy->comb[c][k].len = (comb_len[k] + 23 * c) * rate / 44100;
            sy->comb[c][k].buf = calloc((size_t)sy->comb[c][k].len, sizeof(float));
        }
        for (int k = 0; k < NALL; k++) {
            sy->all[c][k].len = (all_len[k] + 23 * c) * rate / 44100;
            sy->all[c][k].buf = calloc((size_t)sy->all[c][k].len, sizeof(float));
        }
    }
    reset_channels(sy);
    return sy;
}

void gs_synth_free(gs_synth *sy) {
    if (!sy) return;
    for (int c = 0; c < 2; c++) {
        for (int k = 0; k < NCOMB; k++) free(sy->comb[c][k].buf);
        for (int k = 0; k < NALL; k++) free(sy->all[c][k].buf);
    }
    free(sy->rev_in);
    free(sy);
}
