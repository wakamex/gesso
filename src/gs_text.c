#include "gs_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "stb_truetype.h"

#include "kb_text_shape.h"  // Unicode segmentation and OpenType shaping (zlib licence; built in gs_kb.c)

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct gs_font {
    stbtt_fontinfo info;
    void *data;
    size_t len;  // of the mapping (or of the file read into memory, on the web)
    kbts_font kb;  // the shaper's copy of the tables it needs (a few hundred KB), made on first shaping
    bool kb_tried;
    // Colour glyph tables, as offsets into the data (0 when absent): layered outlines (COLR's version 0
    // records with CPAL, as in Windows' Segoe UI Emoji) or PNG strikes (CBLC and CBDT as in Noto Color Emoji, or
    // sbix as in Apple Color Emoji).
    uint32_t colr, cpal, cblc, cbdt, sbix;
    // A variable font drawn off its default: its place on each axis, normalized (2.14 fixed point),
    // the glyph variations table those apply to, and the weight asked for, which the shaper is told.
    int16_t *coords;
    int naxes;
    uint32_t gvar;
    float weight;
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A table's offset in the file, from the font's table directory; 0 when the font has none or the
// directory or table runs past the end of the file.
static uint32_t find_table(const unsigned char *d, size_t len, int fontstart, const char *tag) {
    if (fontstart < 0 || (size_t)fontstart + 12 > len) return 0;
    const uint8_t *dir = d + fontstart;
    for (int i = 0, n = u16(dir + 4); i < n && (size_t)fontstart + 28 + 16 * i <= len; i++) {
        const uint8_t *rec = dir + 12 + 16 * i;
        if (!memcmp(rec, tag, 4)) return (uint64_t)u32(rec + 8) + u32(rec + 12) <= len ? u32(rec + 8) : 0;
    }
    return 0;
}

// A font with only bitmap glyphs (CBDT or sbix, no outlines), which stb_truetype will not open: its
// character map and metrics are set up by hand, and outline lookups are switched off (stb reads no
// glyph locations with an index format past 1), so only the colour path draws its glyphs.
static bool init_bitmap_font(stbtt_fontinfo *info, unsigned char *d, size_t len, int fontstart) {
    uint32_t cmap = find_table(d, len, fontstart, "cmap"), maxp = find_table(d, len, fontstart, "maxp");
    memset(info, 0, sizeof *info);
    info->data = d, info->fontstart = fontstart;
    info->head = (int)find_table(d, len, fontstart, "head"), info->hhea = (int)find_table(d, len, fontstart, "hhea");
    info->hmtx = (int)find_table(d, len, fontstart, "hmtx");
    if (!cmap || !info->head || !info->hhea || !info->hmtx || !maxp) return false;
    info->numGlyphs = u16(d + maxp + 4);
    info->indexToLocFormat = 2;
    info->svg = -1;
    for (int i = 0, n = u16(d + cmap + 2); i < n; i++) {  // a Unicode character map, as stb would choose
        const uint8_t *rec = d + cmap + 4 + 8 * i;
        int platform = u16(rec), encoding = u16(rec + 2);
        if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0) info->index_map = (int)(cmap + u32(rec + 4));
    }
    return info->index_map != 0;
}

// The file mapped read-only rather than read in: a system font can be tens of MB (a CJK font's whole
// character set), and only the pages holding the glyphs drawn are ever read.
static void *font_map(const char *path, size_t *len) {
#if defined(_WIN32)
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024)) return NULL;
    HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    HANDLE map = GetFileSizeEx(file, &size) && size.QuadPart > 0 ? CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CloseHandle(file);
    if (!map) return NULL;
    void *data = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(map);  // the view keeps the mapping
    *len = (size_t)size.QuadPart;
    return data;
#elif defined(__EMSCRIPTEN__)
    return SDL_LoadFile(path, len);
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    void *data = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);  // the mapping keeps the file
    *len = (size_t)st.st_size;
    return data == MAP_FAILED ? NULL : data;
#endif
}

static void font_unmap(void *data, size_t len) {
#if defined(_WIN32)
    (void)len;
    UnmapViewOfFile(data);
#elif defined(__EMSCRIPTEN__)
    (void)len;
    SDL_free(data);
#else
    munmap(data, len);
#endif
}

gs_font *gs_font_load(const char *path) {
    size_t len;
    void *data = font_map(path, &len);
    if (!data) return NULL;
    gs_font *f = calloc(1, sizeof *f);
    f->data = data, f->len = len;
    int start = len >= 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
    bool bitmap_only = start >= 0 && !find_table(data, len, start, "glyf") && !find_table(data, len, start, "CFF ") &&
                       (find_table(data, len, start, "CBDT") || find_table(data, len, start, "sbix"));
    if (start < 0 || !(bitmap_only ? init_bitmap_font(&f->info, data, len, start) : stbtt_InitFont(&f->info, data, start))) {
        font_unmap(data, len);
        free(f);
        return NULL;
    }
    f->colr = find_table(data, len, start, "COLR"), f->cpal = find_table(data, len, start, "CPAL");
    // Version 0's layer records; a version 1 table (Windows 11's Segoe UI Emoji) carries them too.
    if (f->colr && (u16(f->info.data + f->colr) > 1 || !u16(f->info.data + f->colr + 2) || !f->cpal)) f->colr = 0;
    f->cblc = find_table(data, len, start, "CBLC"), f->cbdt = find_table(data, len, start, "CBDT"), f->sbix = find_table(data, len, start, "sbix");
    if (!f->cbdt) f->cblc = 0;
    return f;
}

void gs_font_free(gs_font *f) {
    if (!f) return;
    free(f->coords);
    if (kbts_FontIsValid(&f->kb)) kbts_FreeFont(&f->kb);
    font_unmap(f->data, f->len);
    free(f);
}

int gs_font_glyph(const gs_font *f, uint32_t cp) { return stbtt_FindGlyphIndex(&f->info, (int)cp); }
int gs_font_glyph_count(const gs_font *f) { return f->info.numGlyphs; }

static float em_scale(const gs_font *f, float px) { return stbtt_ScaleForMappingEmToPixels(&f->info, px); }

float gs_font_advance(const gs_font *f, int glyph, float px) {
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
    return adv * em_scale(f, px);
}

float gs_font_kern(const gs_font *f, int glyph, int next, float px) {
    return stbtt_GetGlyphKernAdvance(&f->info, glyph, next) * em_scale(f, px);
}

void gs_font_vmetrics(const gs_font *f, float px, float *ascent, float *descent, float *line_gap) {
    int a, d, g;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
    float s = em_scale(f, px);
    if (ascent) *ascent = a * s;
    if (descent) *descent = d * s;
    if (line_gap) *line_gap = g * s;
}

// ---- Variable fonts ----
//
// A variable font holds one set of outlines and, in its gvar table, how each glyph's points move
// along each axis (weight, for one). stb_truetype draws the default outlines only, so a font set to
// another weight has its glyphs built here: the points read from glyf, moved by gvar's deltas for the
// font's place on its axes, and handed to stb_truetype's rasterizer. The shaper applies the matching
// changes to advances and positioning itself. Fonts with TrueType outlines only (not CFF2).

static bool shapeable(gs_font *f);

bool gs_font_set_weight(gs_font *f, int weight) {
    const uint8_t *d = f->info.data;
    uint32_t fvar = find_table(d, f->len, f->info.fontstart, "fvar"), gvar = find_table(d, f->len, f->info.fontstart, "gvar");
    if (!fvar || !gvar || f->info.loca <= 0 || !shapeable(f)) return false;
    int axes = u16(d + fvar + 8), size = u16(d + fvar + 10), at = -1, kb_axes = 0, instances;
    kbts_GetVariationInfo(&f->kb, &kb_axes, &instances);
    if (!axes || axes != kb_axes || size < 20 || (size_t)fvar + u16(d + fvar + 4) + (size_t)axes * size > f->len) return false;
    float *user = malloc(sizeof *user * (size_t)axes);
    int16_t *coords = calloc((size_t)axes, sizeof *coords);
    for (int i = 0; i < axes; i++) {  // every axis at its default but the weight
        const uint8_t *axis = d + fvar + u16(d + fvar + 4) + (size_t)i * size;
        float min = (int32_t)u32(axis + 4) / 65536.0f, def = (int32_t)u32(axis + 8) / 65536.0f, max = (int32_t)u32(axis + 12) / 65536.0f;
        user[i] = def;
        if (!memcmp(axis, "wght", 4)) at = i, user[i] = f->weight = SDL_clamp((float)weight, min, max);
    }
    if (at >= 0) kbts_NormalizeVariationVector(&f->kb, user, coords);
    free(user);
    bool moved = false;
    for (int i = 0; i < axes; i++) moved = moved || coords[i];
    free(f->coords);
    f->coords = NULL, f->naxes = axes, f->gvar = gvar;
    if (moved) f->coords = coords;  // (at its default, the outlines are the file's own)
    else free(coords);
    return at >= 0;
}

typedef struct { float x, y; bool on; } vpoint;
typedef struct { stbtt_vertex *v; int n, cap; } outline;

// Packed point numbers: how many, with the numbers in *out, or -1 for every point of the glyph.
static int packed_points(const uint8_t **p, const uint8_t *end, uint16_t **out) {
    *out = NULL;
    if (*p >= end) return 0;
    int count = *(*p)++;
    if (!count) return -1;
    if (count & 0x80) count = *p < end ? (count & 0x7F) << 8 | *(*p)++ : 0;
    uint16_t *pts = malloc(sizeof *pts * (size_t)(count + 1));
    int n = 0, at = 0;
    while (pts && n < count && *p < end) {
        int control = *(*p)++, run = (control & 0x7F) + 1;
        for (; run && n < count && *p + (control & 0x80 ? 2 : 1) <= end; run--) {
            at += control & 0x80 ? u16(*p) : **p;
            *p += control & 0x80 ? 2 : 1;
            pts[n++] = (uint16_t)at;
        }
    }
    *out = pts;
    return pts ? n : 0;
}

static void packed_deltas(const uint8_t **p, const uint8_t *end, float *out, int count) {
    int n = 0;
    while (n < count && *p < end) {
        int control = *(*p)++, run = (control & 0x3F) + 1;
        for (; run && n < count; run--) {
            if (control & 0x80) out[n++] = 0;
            else if (control & 0x40) {
                if (*p + 2 > end) break;
                out[n++] = (int16_t)u16(*p), *p += 2;
            } else {
                if (*p >= end) break;
                out[n++] = (int8_t)*(*p)++;
            }
        }
    }
    for (; n < count; n++) out[n] = 0;
}

// A point between two that moved takes its share of their movement, by where it lies between them.
static float between(float p, float a, float b, float da, float db) {
    if (a == b) return da == db ? da : 0;
    if (a > b) { float t = a; a = b, b = t, t = da, da = db, db = t; }
    return p <= a ? da : p >= b ? db : da + (db - da) * (p - a) / (b - a);
}

// The movement of a glyph's n points (pts: where they are in the default outline; its contours end
// at `ends`) for the font's place on its axes, added to dx and dy. A composite glyph's points are its
// components' offsets, with no contours. Four phantom points follow either kind's, for its metrics.
static void gvar_deltas(const gs_font *f, int glyph, const vpoint *pts, int n, const uint16_t *ends, int contours, float *dx, float *dy) {
    const uint8_t *d = f->info.data, *gv = d + f->gvar, *limit = d + f->len;
    if (gv + 20 > limit) return;
    int axes = u16(gv + 4), shared = u16(gv + 6), flags = u16(gv + 14);
    if (axes != f->naxes || glyph >= u16(gv + 12) || gv + 20 + (flags & 1 ? 4 : 2) * (size_t)(glyph + 2) > limit) return;
    const uint8_t *offs = gv + 20;
    uint32_t from = flags & 1 ? u32(offs + 4 * glyph) : 2u * u16(offs + 2 * glyph), to = flags & 1 ? u32(offs + 4 * glyph + 4) : 2u * u16(offs + 2 * glyph + 2);
    const uint8_t *g = gv + u32(gv + 16) + from, *gend = gv + u32(gv + 16) + to, *tuples = gv + u32(gv + 8);
    if (to < from + 4 || gend > limit || tuples + 2 * (size_t)axes * shared > limit) return;
    int count = u16(g);
    const uint8_t *header = g + 4, *data = g + u16(g + 2);
    uint16_t *all = NULL, *own;
    int nall = -1;
    if (count & 0x8000) nall = packed_points(&data, gend, &all);  // point numbers its tuples share
    float *work = malloc(sizeof *work * 4 * (size_t)n);  // a tuple's deltas as packed, then as placed on the points
    bool *has = malloc((size_t)n);
    for (int t = 0; work && has && t < (count & 0x0FFF) && header + 4 <= gend; t++) {
        int size = u16(header), index = u16(header + 2);
        const uint8_t *h = header + 4, *peak, *lo = NULL, *hi = NULL, *p = data, *pend = data + size;
        if (index & 0x8000) peak = h, h += 2 * axes;
        else peak = (index & 0x0FFF) < shared ? tuples + 2 * (size_t)axes * (index & 0x0FFF) : NULL;
        if (index & 0x4000) lo = h, hi = h + 2 * axes, h += 4 * axes;
        header = h, data = pend;
        if (!peak || h > gend || pend > gend) break;
        float scalar = 1;  // how much of this tuple applies here: 1 at its peak, falling to 0 at the edges of its region
        for (int a = 0; a < axes && scalar; a++) {
            float c = f->coords[a], pk = (int16_t)u16(peak + 2 * a);
            if (pk == 0 || c == pk) continue;
            if (lo) {
                float s = (int16_t)u16(lo + 2 * a), e = (int16_t)u16(hi + 2 * a);
                if (c < s || c > e) scalar = 0;
                else if (c < pk) scalar *= pk != s ? (c - s) / (pk - s) : 1;
                else scalar *= pk != e ? (e - c) / (e - pk) : 1;
            } else if (c == 0 || (c < 0) != (pk < 0) || fabsf(c) > fabsf(pk)) scalar = 0;
            else scalar *= c / pk;
        }
        if (!scalar) continue;
        own = NULL;
        int npts = index & 0x2000 ? packed_points(&p, pend, &own) : nall, nd = npts < 0 ? n : npts;
        const uint16_t *use = index & 0x2000 ? own : all;
        float *tx = malloc(sizeof *tx * 2 * (size_t)(nd + 1)), *ty = tx ? tx + nd : NULL;
        if (tx) {
            packed_deltas(&p, pend, tx, nd), packed_deltas(&p, pend, ty, nd);
            if (npts < 0) {
                for (int i = 0; i < n; i++) dx[i] += scalar * tx[i], dy[i] += scalar * ty[i];
            } else {
                // Deltas for some points: each contour's other points move with their neighbours that have one.
                float *ex = work, *ey = work + n;
                memset(work, 0, sizeof *work * 2 * (size_t)n), memset(has, 0, (size_t)n);
                for (int i = 0; i < npts; i++)
                    if (use[i] < n) ex[use[i]] = tx[i], ey[use[i]] = ty[i], has[use[i]] = true;
                for (int c = 0, first = 0; c < contours; first = ends[c++] + 1) {
                    int last = ends[c], refs = 0;
                    if (last >= n - 4 || last < first) break;
                    for (int i = first; i <= last; i++) refs += has[i];
                    if (!refs || refs == last - first + 1) continue;
                    for (int i = first; i <= last; i++) {
                        if (has[i]) continue;
                        int a = i, b = i;
                        do a = a == first ? last : a - 1; while (!has[a]);
                        do b = b == last ? first : b + 1; while (!has[b]);
                        ex[i] = between(pts[i].x, pts[a].x, pts[b].x, ex[a], ex[b]), ey[i] = between(pts[i].y, pts[a].y, pts[b].y, ey[a], ey[b]);
                    }
                }
                for (int i = 0; i < n; i++) dx[i] += scalar * ex[i], dy[i] += scalar * ey[i];
            }
        }
        free(tx), free(own);
    }
    free(work), free(has), free(all);
}

static void vertex(outline *o, int type, float x, float y, float cx, float cy) {
    if (o->n == o->cap) o->v = realloc(o->v, sizeof *o->v * (size_t)(o->cap = o->cap ? o->cap * 2 : 64));
    stbtt_vertex *v = &o->v[o->n++];
    v->type = (unsigned char)type, v->x = (short)lroundf(x), v->y = (short)lroundf(y), v->cx = (short)lroundf(cx), v->cy = (short)lroundf(cy);
}

// A contour's points as moves, lines and quadratic curves: two off-curve points in a row have an
// on-curve point halfway between them.
static void contour(outline *o, const vpoint *p, int n) {
    if (n < 2) return;
    int first = -1;
    for (int i = 0; i < n && first < 0; i++)
        if (p[i].on) first = i;
    vpoint start = first >= 0 ? p[first] : (vpoint){ (p[n - 1].x + p[0].x) / 2, (p[n - 1].y + p[0].y) / 2, true }, ctrl = { 0 };
    bool pending = false;
    vertex(o, STBTT_vmove, start.x, start.y, 0, 0);
    for (int k = 1; k <= (first >= 0 ? n - 1 : n); k++) {  // the points after the start, round the contour; all of them when the start is implied
        vpoint q = first >= 0 ? p[(first + k) % n] : p[k - 1];
        if (!q.on) {
            if (pending) vertex(o, STBTT_vcurve, (ctrl.x + q.x) / 2, (ctrl.y + q.y) / 2, ctrl.x, ctrl.y);
            ctrl = q, pending = true;
        } else {
            if (pending) vertex(o, STBTT_vcurve, q.x, q.y, ctrl.x, ctrl.y);
            else vertex(o, STBTT_vline, q.x, q.y, 0, 0);
            pending = false;
        }
    }
    if (pending) vertex(o, STBTT_vcurve, start.x, start.y, ctrl.x, ctrl.y);
    else vertex(o, STBTT_vline, start.x, start.y, 0, 0);
}

// A glyph's outline at the font's place on its axes, placed by m (x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5).
static void varied_glyph(const gs_font *f, int glyph, const float m[6], outline *o, int depth) {
    const stbtt_fontinfo *info = &f->info;
    const uint8_t *d = info->data, *limit = d + f->len;
    if (glyph < 0 || glyph >= info->numGlyphs || depth > 6) return;
    uint32_t from = info->indexToLocFormat ? u32(d + info->loca + 4 * glyph) : 2u * u16(d + info->loca + 2 * glyph);
    uint32_t to = info->indexToLocFormat ? u32(d + info->loca + 4 * glyph + 4) : 2u * u16(d + info->loca + 2 * glyph + 2);
    const uint8_t *g = d + info->glyf + from, *end = d + info->glyf + to;
    if (to < from + 10 || end > limit) return;  // (no outline: a space)
    int contours = (int16_t)u16(g);
    const uint8_t *p = g + 10;
    if (contours >= 0) {
        if (p + 2 * contours + 2 > end) return;
        int n = contours ? u16(p + 2 * (contours - 1)) + 1 : 0;
        uint16_t *ends = malloc(sizeof *ends * (size_t)(contours + 1));
        vpoint *pts = calloc((size_t)n + 4, sizeof *pts);
        uint8_t *flags = malloc((size_t)n + 1);
        float *delta = calloc(2 * ((size_t)n + 4), sizeof *delta);
        for (int i = 0; i < contours; i++) ends[i] = u16(p + 2 * i);
        p += 2 * contours;
        p += 2 + u16(p);  // (the instructions, which are for hinting)
        bool ok = ends && pts && flags && delta;
        for (int i = 0, repeat = 0; ok && i < n; i++) {
            if (repeat) flags[i] = flags[i - 1], repeat--;
            else {
                if (p >= end) { ok = false; break; }
                flags[i] = *p++;
                if (flags[i] & 8) repeat = p < end ? *p++ : 0;
            }
        }
        for (int axis = 0; ok && axis < 2; axis++) {  // the x coordinates, then the y: each a step from the last
            int at = 0, is_short = axis ? 4 : 2, same = axis ? 32 : 16;
            for (int i = 0; i < n; i++) {
                if (flags[i] & is_short) {
                    if (p >= end) { ok = false; break; }
                    at += flags[i] & same ? *p : -(int)*p, p++;
                } else if (!(flags[i] & same)) {
                    if (p + 2 > end) { ok = false; break; }
                    at += (int16_t)u16(p), p += 2;
                }
                if (axis) pts[i].y = (float)at; else pts[i].x = (float)at, pts[i].on = flags[i] & 1;
            }
        }
        if (ok) {
            gvar_deltas(f, glyph, pts, n + 4, ends, contours, delta, delta + n + 4);
            for (int i = 0; i < n; i++) {
                float x = pts[i].x + delta[i], y = pts[i].y + delta[n + 4 + i];
                pts[i].x = m[0] * x + m[2] * y + m[4], pts[i].y = m[1] * x + m[3] * y + m[5];
            }
            for (int c = 0, first = 0; c < contours && ends[c] < n && ends[c] >= first; first = ends[c++] + 1) contour(o, pts + first, ends[c] - first + 1);
        }
        free(ends), free(pts), free(flags), free(delta);
        return;
    }
    // A composite: other glyphs, each scaled and placed. Its points for gvar are their offsets.
    enum { MAX = 64 };
    struct { int glyph; float t[6]; } parts[MAX];
    vpoint at[MAX + 4] = { 0 };
    float delta[2 * (MAX + 4)] = { 0 };
    int n = 0;
    for (bool more = true; more && n < MAX && p + 4 <= end;) {
        int flags = u16(p);
        parts[n].glyph = u16(p + 2), p += 4;
        float *t = parts[n].t;
        t[0] = t[3] = 1, t[1] = t[2] = t[4] = t[5] = 0;
        if (p + (flags & 1 ? 4 : 2) > end) break;
        if (flags & 2) t[4] = flags & 1 ? (int16_t)u16(p) : (int8_t)p[0], t[5] = flags & 1 ? (int16_t)u16(p + 2) : (int8_t)p[1];  // (offsets; placing by matched points is not handled)
        p += flags & 1 ? 4 : 2;
        int scales = flags & 8 ? 1 : flags & 0x40 ? 2 : flags & 0x80 ? 4 : 0;
        if (p + 2 * scales > end) break;
        if (scales == 1) t[0] = t[3] = (int16_t)u16(p) / 16384.0f;
        if (scales == 2) t[0] = (int16_t)u16(p) / 16384.0f, t[3] = (int16_t)u16(p + 2) / 16384.0f;
        if (scales == 4) t[0] = (int16_t)u16(p) / 16384.0f, t[1] = (int16_t)u16(p + 2) / 16384.0f, t[2] = (int16_t)u16(p + 4) / 16384.0f, t[3] = (int16_t)u16(p + 6) / 16384.0f;
        p += 2 * scales;
        at[n].x = t[4], at[n].y = t[5];
        more = flags & 0x20, n++;
    }
    gvar_deltas(f, glyph, at, n + 4, NULL, 0, delta, delta + n + 4);
    for (int i = 0; i < n; i++) {
        float *t = parts[i].t, ox = t[4] + delta[i], oy = t[5] + delta[n + 4 + i];
        float child[6] = { m[0] * t[0] + m[2] * t[1], m[1] * t[0] + m[3] * t[1], m[0] * t[2] + m[2] * t[3], m[1] * t[2] + m[3] * t[3],
                           m[0] * ox + m[2] * oy + m[4], m[1] * ox + m[3] * oy + m[5] };
        varied_glyph(f, parts[i].glyph, child, o, depth + 1);
    }
}

// A glyph's coverage at `scale` (pixels per font unit), with a clear pixel on each side, and where
// its top left sits from the pen position, drawn `shift` of a pixel to the right of it. A font off
// its default axes has its outline built above.
static unsigned char *glyph_cover(const gs_font *f, int glyph, float scale, float shift, int *w, int *h, int *ox, int *oy) {
    outline o = { 0 };
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (f->coords) {
        static const float identity[6] = { 1, 0, 0, 1, 0, 0 };
        varied_glyph(f, glyph, identity, &o, 0);
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int i = 0; i < o.n; i++) {
            minx = fminf(minx, o.v[i].x), maxx = fmaxf(maxx, o.v[i].x), miny = fminf(miny, o.v[i].y), maxy = fmaxf(maxy, o.v[i].y);
            if (o.v[i].type == STBTT_vcurve) minx = fminf(minx, o.v[i].cx), maxx = fmaxf(maxx, o.v[i].cx), miny = fminf(miny, o.v[i].cy), maxy = fmaxf(maxy, o.v[i].cy);
        }
        if (o.n) x0 = (int)floorf(minx * scale + shift), y0 = (int)floorf(-maxy * scale), x1 = (int)ceilf(maxx * scale + shift), y1 = (int)ceilf(-miny * scale);
    } else stbtt_GetGlyphBitmapBoxSubpixel(&f->info, glyph, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
    *w = x1 - x0 + 2, *h = y1 - y0 + 2, *ox = x0 - 1, *oy = y0 - 1;
    unsigned char *cov = calloc((size_t)*w * (size_t)*h, 1);
    if (cov && f->coords && o.n) {
        stbtt__bitmap bitmap = { *w - 2, *h - 2, *w, cov + *w + 1 };
        stbtt_Rasterize(&bitmap, 0.35f, o.v, o.n, scale, scale, shift, 0, x0, y0, 1, NULL);
    } else if (cov && !f->coords) stbtt_MakeGlyphBitmapSubpixel(&f->info, cov + *w + 1, *w - 2, *h - 2, *w, scale, scale, shift, 0, glyph);
    free(o.v);
    return cov;
}

// ---- Glyph atlas ----

// One cached glyph: where it sits in the atlas and its offset from the pen position.
typedef struct {
    const gs_font *font;
    int glyph, size4;  // size in quarter pixels
    uint8_t tone, phase;  // which of the text's lightnesses its coverage was corrected for, and how many quarter pixels right of the pen it was drawn
    int x, y, w, h, ox, oy;
    bool pending;      // rasterized, waiting for gs_glyphs_begin_frame to upload it
    bool colour;       // its own colours, drawn untinted
} slot;

typedef struct { SDL_Rect at; uint32_t *px; } upload;

#define SLOTS 4096  // open addressing; the atlas is cleared well before this fills
#define TONES 16    // lightnesses of text told apart
#define PHASES 4    // positions between two pixels a glyph is drawn at

struct gs_glyphs {
    SDL_Renderer *r;
    SDL_Texture *tex;
    int size, shelf_x, shelf_y, shelf_h, used;
    slot slots[SLOTS];
    bool deferred;     // gs_glyphs_begin_frame is called: uploads wait for it
    bool waiting;      // a glyph was left out of this frame for want of its upload
    upload *uploads;
    int nuploads, uploads_cap;
    float gamma, behind;
    float untinted;    // the colour glyphs are drawn in when they are to be left as they are
    uint8_t correct[TONES][256];  // coverage to alpha, for each lightness of text
    bool made[TONES];
};

gs_glyphs *gs_glyphs_new(SDL_Renderer *r, int atlas_size) {
    gs_glyphs *g = calloc(1, sizeof *g);
    g->r = r;
    g->size = atlas_size;
    g->gamma = 2.2f, g->behind = -1;
    // SDL's software renderer draws an untinted texture by another path than a tinted one, and prepares
    // the whole atlas again each time a frame goes from one to the other (tens of milliseconds a frame
    // with white text among coloured): there, nothing is drawn quite untinted.
    const char *name = SDL_GetRendererName(r);
    g->untinted = name && !strcmp(name, SDL_SOFTWARE_RENDERER) ? 254.f / 255 : 1;
    g->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, atlas_size, atlas_size);
    SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g->tex, SDL_SCALEMODE_LINEAR);
    return g;
}

static void drop_uploads(gs_glyphs *g) {
    for (int i = 0; i < g->nuploads; i++) free(g->uploads[i].px);
    g->nuploads = 0;
}

void gs_glyphs_free(gs_glyphs *g) {
    if (!g) return;
    drop_uploads(g);
    free(g->uploads);
    SDL_DestroyTexture(g->tex);
    free(g);
}

void gs_glyphs_begin_frame(gs_glyphs *g) {
    g->deferred = true, g->waiting = false;
    for (int i = 0; i < g->nuploads; i++) SDL_UpdateTexture(g->tex, &g->uploads[i].at, g->uploads[i].px, g->uploads[i].at.w * 4);
    if (g->nuploads)
        for (int i = 0; i < SLOTS; i++) g->slots[i].pending = false;
    drop_uploads(g);
}

bool gs_glyphs_waiting(const gs_glyphs *g) { return g->waiting; }

static void clear(gs_glyphs *g) {
    memset(g->slots, 0, sizeof g->slots);
    g->shelf_x = g->shelf_y = g->shelf_h = g->used = 0;
    drop_uploads(g);  // (their places in the atlas are given out again)
}

// ---- Colour glyphs ----

// A rasterized glyph in straight-alpha RGBA (SDL_PIXELFORMAT_RGBA32), with its top left's offset from
// the pen position.
typedef struct { int w, h, ox, oy; uint32_t *px; } image;

static uint32_t pack(float r, float g, float b, float a) {  // from premultiplied components in [0, 1]
    if (a <= 0) return 0;
    uint32_t R = (uint32_t)lroundf(fminf(r / a, 1) * 255), G = (uint32_t)lroundf(fminf(g / a, 1) * 255), B = (uint32_t)lroundf(fminf(b / a, 1) * 255);
    return SDL_Swap32LE(R | G << 8 | B << 16 | (uint32_t)lroundf(fminf(a, 1) * 255) << 24);
}

// COLR version 0: the glyph is a stack of other glyphs, each filled with one colour from CPAL's first palette.
static bool colr_glyph(const gs_font *f, int glyph, float scale, image *out) {
    const uint8_t *d = f->info.data, *c = d + f->colr, *p = d + f->cpal;
    int lo = 0, hi = u16(c + 2) - 1;
    const uint8_t *base = NULL;
    while (lo <= hi) {  // the base glyph records are sorted by glyph
        int mid = (lo + hi) / 2;
        const uint8_t *r = c + u32(c + 4) + 6 * mid;
        if (u16(r) == glyph) { base = r; break; }
        if (u16(r) < glyph) lo = mid + 1; else hi = mid - 1;
    }
    if (!base) return false;
    const uint8_t *layers = c + u32(c + 8) + 4 * u16(base + 2);
    int count = u16(base + 4), X0 = INT32_MAX, Y0 = INT32_MAX, X1 = INT32_MIN, Y1 = INT32_MIN;
    for (int i = 0; i < count; i++) {
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&f->info, u16(layers + 4 * i), scale, scale, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0) X0 = SDL_min(X0, x0), Y0 = SDL_min(Y0, y0), X1 = SDL_max(X1, x1), Y1 = SDL_max(Y1, y1);
    }
    if (X1 <= X0) return false;
    int w = X1 - X0 + 2, h = Y1 - Y0 + 2;  // one clear pixel each side for linear filtering
    float *acc = calloc((size_t)w * h * 4, sizeof *acc);
    unsigned char *cov = malloc((size_t)w * h);
    const uint8_t *palette = p + u32(p + 8) + 4 * u16(p + 12);  // the first palette's colour records, BGRA
    int entries = u16(p + 2);
    for (int i = 0; acc && cov && i < count; i++) {
        int lg = u16(layers + 4 * i), index = u16(layers + 4 * i + 2), x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&f->info, lg, scale, scale, &x0, &y0, &x1, &y1);
        if (x1 <= x0 || y1 <= y0) continue;
        memset(cov, 0, (size_t)w * h);
        stbtt_MakeGlyphBitmap(&f->info, cov + (y0 - Y0 + 1) * w + (x0 - X0 + 1), x1 - x0, y1 - y0, w, scale, scale, lg);
        // 0xFFFF is the text colour; drawn white here, the glyph's own colours being the point.
        float b = 1, g = 1, r = 1, a = 1;
        if (index != 0xFFFF && index < entries) b = palette[4 * index] / 255.0f, g = palette[4 * index + 1] / 255.0f, r = palette[4 * index + 2] / 255.0f, a = palette[4 * index + 3] / 255.0f;
        for (int k = 0; k < w * h; k++) {
            float s = cov[k] / 255.0f * a;
            if (!s) continue;
            float *px = acc + 4 * k;  // source over, premultiplied
            px[0] = r * s + px[0] * (1 - s), px[1] = g * s + px[1] * (1 - s), px[2] = b * s + px[2] * (1 - s), px[3] = s + px[3] * (1 - s);
        }
    }
    out->px = acc && cov ? malloc((size_t)w * h * 4) : NULL;
    for (int k = 0; out->px && k < w * h; k++) out->px[k] = pack(acc[4 * k], acc[4 * k + 1], acc[4 * k + 2], acc[4 * k + 3]);
    free(acc), free(cov);
    out->w = w, out->h = h, out->ox = X0 - 1, out->oy = Y0 - 1;
    return out->px != NULL;
}

// Scales a decoded PNG (straight RGBA) by k with a box filter, into out with a clear border.
static bool scale_bitmap(const uint8_t *src, int sw, int sh, float k, image *out) {
    int w = (int)ceilf(sw * k) + 2, h = (int)ceilf(sh * k) + 2;
    out->px = calloc((size_t)w * h, 4);
    if (!out->px) return false;
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++) {
            float fx0 = (x - 1) / k, fx1 = x / k, fy0 = (y - 1) / k, fy1 = y / k, r = 0, g = 0, b = 0, a = 0, area = 0;
            for (int sy = (int)fy0; sy < sh && sy < fy1; sy++)
                for (int sx = (int)fx0; sx < sw && sx < fx1; sx++) {
                    float cover = (fminf(fx1, sx + 1.0f) - fmaxf(fx0, (float)sx)) * (fminf(fy1, sy + 1.0f) - fmaxf(fy0, (float)sy));
                    const uint8_t *q = src + 4 * ((size_t)sy * sw + sx);
                    float qa = q[3] / 255.0f * cover;
                    r += q[0] / 255.0f * qa, g += q[1] / 255.0f * qa, b += q[2] / 255.0f * qa, a += qa, area += cover;
                }
            if (area > 0) out->px[y * w + x] = pack(r / area, g / area, b / area, a / area);
        }
    out->w = w, out->h = h;
    return true;
}

static int units_per_em(const gs_font *f) { return u16(f->info.data + f->info.head + 18); }

static bool png_glyph(const uint8_t *png, size_t len, float k, int left, int top, image *out) {
    int sw, sh, n;
    uint8_t *src = len ? stbi_load_from_memory(png, (int)len, &sw, &sh, &n, 4) : NULL;
    bool ok = src && scale_bitmap(src, sw, sh, k, out);
    stbi_image_free(src);
    if (ok) out->ox = (int)floorf(left * k) - 1, out->oy = (int)floorf(-top * k) - 1;
    return ok;
}

// CBLC and CBDT: the strike whose size is nearest above the size drawn (or the largest), scaled down.
static bool cbdt_glyph(const gs_font *f, int glyph, float px, image *out) {
    const uint8_t *d = f->info.data, *c = d + f->cblc, *best = NULL;
    for (uint32_t i = 0, n = u32(c + 4); i < n; i++) {
        const uint8_t *size = c + 8 + 48 * i;
        if (glyph < u16(size + 40) || glyph > u16(size + 42) || size[46] != 32) continue;  // (32-bit colour strikes)
        if (!best || (best[45] < px ? size[45] > best[45] : size[45] >= px && size[45] < best[45])) best = size;
    }
    if (!best) return false;
    const uint8_t *array = c + u32(best);
    for (uint32_t i = 0, n = u32(best + 8); i < n; i++) {
        const uint8_t *e = array + 8 * i;
        int first = u16(e), last = u16(e + 2);
        if (glyph < first || glyph > last) continue;
        const uint8_t *sub = array + u32(e + 4);
        int index_format = u16(sub), image_format = u16(sub + 2), at = glyph - first;
        uint32_t base = f->cbdt + u32(sub + 4), off, end;
        if (index_format == 1) off = u32(sub + 8 + 4 * at), end = u32(sub + 12 + 4 * at);
        else if (index_format == 3) off = u16(sub + 8 + 2 * at), end = u16(sub + 10 + 2 * at);
        else return false;
        if (end <= off || base + end > f->len) return false;
        const uint8_t *img = d + base + off;
        float k = px / best[45];
        if (image_format == 17) return png_glyph(img + 9, u32(img + 5), k, (int8_t)img[2], (int8_t)img[3], out);
        if (image_format == 18) return png_glyph(img + 12, u32(img + 8), k, (int8_t)img[2], (int8_t)img[3], out);
        return false;
    }
    return false;
}

// sbix: PNG strikes, the bitmap's bottom left at its origin offset.
static bool sbix_glyph(const gs_font *f, int glyph, float px, image *out) {
    const uint8_t *d = f->info.data, *s = d + f->sbix, *best = NULL;
    for (uint32_t i = 0, n = u32(s + 4); i < n; i++) {
        const uint8_t *strike = s + u32(s + 8 + 4 * i);
        int ppem = u16(strike), have = best ? u16(best) : 0;
        if (!best || (have < px ? ppem > have : ppem >= px && ppem < have)) best = strike;
    }
    if (!best || glyph + 1 >= f->info.numGlyphs + 1) return false;
    uint32_t off = u32(best + 4 + 4 * glyph), end = u32(best + 8 + 4 * glyph);
    if (end <= off + 8 || (size_t)(best - d) + end > f->len) return false;
    const uint8_t *g = best + off;
    if (memcmp(g + 4, "png ", 4)) return false;
    int sw, sh, n;
    if (!stbi_info_from_memory(g + 8, (int)(end - off - 8), &sw, &sh, &n)) return false;
    float k = px / u16(best);
    return png_glyph(g + 8, end - off - 8, k, (int16_t)u16(g), (int16_t)u16(g + 2) + sh, out);
}

static bool colour_glyph(const gs_font *f, int glyph, float scale, image *out) {
    float px = scale * units_per_em(f);
    return (f->colr && colr_glyph(f, glyph, scale, out)) || (f->cblc && cbdt_glyph(f, glyph, px, out)) || (f->sbix && sbix_glyph(f, glyph, px, out));
}

// What a glyph's coverage becomes as the alpha it is blended with, for text of one lightness.
//
// The renderer blends in the display's encoding, not in light: a pixel half covered by white text
// on black comes out at half the encoded value, which is a fifth of the light, so light text looks
// thinner than its outline. The alpha that gives the right light depends on what is behind the
// text: the lightness the app gave, or, not knowing, as far from the text as the text is from mid
// grey. Text darker than what is behind it is left as it is: blending in the encoding makes it a
// little heavy, which is what DirectWrite's contrast does to it on purpose, and what Chromium on
// Windows shows.
static const uint8_t *correction(gs_glyphs *g, int tone) {
    uint8_t *t = g->correct[tone];
    if (g->made[tone]) return t;
    g->made[tone] = true;
    const float text = (float)tone / (TONES - 1), behind = g->behind >= 0 ? g->behind : 1 - text;
    const float text_light = powf(text, g->gamma), behind_light = powf(behind, g->gamma);
    for (int i = 0; i < 256; i++) {
        const float a = (float)i / 255;
        if (text < behind + 0.1f || g->gamma == 1) { t[i] = (uint8_t)i; continue; }
        const float out = powf(text_light * a + behind_light * (1 - a), 1 / g->gamma);
        t[i] = (uint8_t)lroundf(255 * SDL_clamp((out - behind) / (text - behind), 0, 1));
    }
    return t;
}

static float lightness(const gs_glyphs *g, SDL_FColor c) {
    const float light = 0.2126f * powf(c.r, g->gamma) + 0.7152f * powf(c.g, g->gamma) + 0.0722f * powf(c.b, g->gamma);
    return powf(SDL_clamp(light, 0, 1), 1 / g->gamma);
}

void gs_glyphs_correction(gs_glyphs *g, float gamma) {
    gamma = gamma > 0 ? gamma : 1;
    if (gamma == g->gamma) return;
    g->gamma = gamma;
    memset(g->made, 0, sizeof g->made);
    clear(g);
}

void gs_glyphs_behind(gs_glyphs *g, const SDL_FColor *background) {
    const float behind = background ? roundf(lightness(g, *background) * 32) / 32 : -1;  // (in steps, so that a colour easing to another empties the atlas a few times, not every frame)
    if (behind == g->behind) return;
    g->behind = behind;
    memset(g->made, 0, sizeof g->made);
    clear(g);
}

// Finds a glyph in the cache, rasterizing it into the atlas on first use. NULL if it cannot fit.
static slot *lookup(gs_glyphs *g, const gs_font *f, int glyph, int size4, int tone, int phase) {
    uint32_t h = ((uint32_t)(uintptr_t)f * 31u + (uint32_t)glyph) * 2654435761u ^ (uint32_t)size4 * 40503u ^ (uint32_t)(tone * PHASES + phase) * 0x9E3779B1u;
    for (int tries = 0; tries < 2; tries++) {
        for (uint32_t i = h;; i++) {
            slot *s = &g->slots[i % SLOTS];
            if (s->font == f && s->glyph == glyph && s->size4 == size4 && s->tone == tone && s->phase == phase) return s;
            if (s->font) continue;
            if (g->used > SLOTS * 3 / 4) break;  // too full: clear and start over
            float scale = em_scale(f, size4 / 4.0f);
            image img = { 0 };
            bool colour = colour_glyph(f, glyph, scale, &img);
            unsigned char *cov = colour ? NULL : glyph_cover(f, glyph, scale, (float)phase / PHASES, &img.w, &img.h, &img.ox, &img.oy);  // (one clear pixel each side for linear filtering)
            int w = img.w, hh = img.h;
            if (w > g->size || hh > g->size) return free(img.px), free(cov), NULL;
            if (g->shelf_x + w > g->size) g->shelf_x = 0, g->shelf_y += g->shelf_h, g->shelf_h = 0;
            if (g->shelf_y + hh > g->size) { free(img.px), free(cov); break; }
            uint32_t *px = img.px;
            if (!colour) {
                px = malloc((size_t)w * hh * 4);
                const uint8_t *alpha = correction(g, tone);
                for (int k = 0; cov && k < w * hh; k++) px[k] = SDL_Swap32LE(0x00FFFFFFu | (uint32_t)alpha[cov[k]] << 24);
                free(cov);
            }
            SDL_Rect dst = { g->shelf_x, g->shelf_y, w, hh };
            if (g->deferred) {
                if (g->nuploads == g->uploads_cap)
                    g->uploads = realloc(g->uploads, sizeof *g->uploads * (size_t)(g->uploads_cap = g->uploads_cap ? g->uploads_cap * 2 : 64));
                g->uploads[g->nuploads++] = (upload){ dst, px };
            } else {
                SDL_UpdateTexture(g->tex, &dst, px, w * 4);
                free(px);
            }
            *s = (slot){ f, glyph, size4, (uint8_t)tone, (uint8_t)phase, g->shelf_x, g->shelf_y, w, hh, img.ox, img.oy, g->deferred, colour };
            g->shelf_x += w;
            if (hh > g->shelf_h) g->shelf_h = hh;
            g->used++;
            return s;
        }
        clear(g);
    }
    return NULL;
}

void gs_glyphs_draw(gs_glyphs *g, const gs_font *f, int glyph, float px, float x, float y, float angle,
                    SDL_FColor colour, float reveal) {
    if (reveal <= 0) return;
    // Upright text is drawn on whole pixels, from the glyph rasterized at the nearest quarter pixel to
    // where it falls between them: sharper than one image of it resampled to every position. A font
    // with colour glyphs is drawn as before, and text at an angle is resampled as it must be.
    int tone = 0, phase = 0;
    if (!f->colr && !f->cblc && !f->sbix) {
        tone = (int)lroundf(lightness(g, colour) * (TONES - 1));
        if (angle == 0) {
            const float whole = floorf(x * PHASES + 0.5f) / PHASES;
            phase = (int)lroundf((whole - floorf(whole)) * PHASES) % PHASES;
            x = floorf(whole), y = roundf(y);
        }
    }
    slot *s = lookup(g, f, glyph, (int)lroundf(px * 4), tone, phase);
    if (!s || s->w <= 2) return;
    if (s->pending) { g->waiting = true; return; }
    float wr = reveal >= 1 ? (float)s->w : s->w * reveal;
    SDL_FRect src = { (float)s->x, (float)s->y, wr, (float)s->h };
    SDL_FRect dst = { x + s->ox, y + s->oy, wr, (float)s->h };
    SDL_FPoint centre = { (float)-s->ox, (float)-s->oy };
    if (s->colour || (colour.r >= g->untinted && colour.g >= g->untinted && colour.b >= g->untinted)) SDL_SetTextureColorModFloat(g->tex, g->untinted, g->untinted, g->untinted);  // (its own colours, or white)
    else SDL_SetTextureColorModFloat(g->tex, colour.r, colour.g, colour.b);
    SDL_SetTextureAlphaModFloat(g->tex, colour.a);
    SDL_RenderTextureRotated(g->r, g->tex, &src, &dst, angle, &centre, SDL_FLIP_NONE);
}

uint32_t gs_utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    if (!*p) return 0;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= 0x3F >> extra;
    for (; extra && (*p & 0xC0) == 0x80; extra--) c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return extra ? 0xFFFD : c;
}

float gs_text_draw(gs_glyphs *g, const gs_font *f, float px, float x, float y, const char *utf8, SDL_FColor colour) {
    float pen = x;
    int prev = 0;
    for (uint32_t cp; (cp = gs_utf8_next(&utf8));) {
        int gl = gs_font_glyph(f, cp);
        if (prev) pen += gs_font_kern(f, prev, gl, px);
        gs_glyphs_draw(g, f, gl, px, pen, y, 0, colour, 1);
        pen += gs_font_advance(f, gl, px);
        prev = gl;
    }
    return pen - x;
}

float gs_text_width(const gs_font *f, float px, const char *utf8) {
    float w = 0;
    int prev = 0;
    for (uint32_t cp; (cp = gs_utf8_next(&utf8));) {
        int gl = gs_font_glyph(f, cp);
        if (prev) w += gs_font_kern(f, prev, gl, px);
        w += gs_font_advance(f, gl, px);
        prev = gl;
    }
    return w;
}

// ---- Font chains and shaping ----

#define MAX_FONTS 24
#define LINE_CACHE 512  // shaped lines kept, so text drawn every frame is shaped once

typedef struct {
    char *text;
    int size4;  // px in quarter pixels
    int dir;
    uint32_t around;  // for a line of a longer paragraph: a hash of the paragraph and of the line's place in it; 0 for a text shaped alone
    gs_line *line;
} cached_line;

struct gs_fontset {
    char *path[MAX_FONTS];
    gs_font *font[MAX_FONTS];
    kbts_shape_context *ctx[MAX_FONTS];  // a shaping context holding just this font, made on first use
    bool tried[MAX_FONTS], emoji[MAX_FONTS];
    int n, weight;  // weight: what its variable fonts are set to as they load; 0 leaves them at their default
    cached_line cache[LINE_CACHE];
};

gs_fontset *gs_fontset_new(void) { return calloc(1, sizeof(gs_fontset)); }

void gs_fontset_add(gs_fontset *fs, const char *path) {
    SDL_PathInfo info;
    if (fs->n == MAX_FONTS || !SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return;
    // An emoji font (by its name, before it is loaded) takes the graphemes shown as emoji, even where an
    // earlier font has a plain glyph for them.
    fs->emoji[fs->n] = SDL_strcasestr(path, "emoji") != NULL || SDL_strcasestr(path, "seguiemj") != NULL;
    fs->path[fs->n++] = SDL_strdup(path);
}

static void forget_lines(gs_fontset *fs);

void gs_fontset_prepend(gs_fontset *fs, const char *path) {
    int n = fs->n;
    gs_fontset_add(fs, path);
    if (fs->n == n) return;
    char *p = fs->path[n];
    bool emoji = fs->emoji[n];
    memmove(fs->path + 1, fs->path, sizeof *fs->path * (size_t)n);
    memmove(fs->font + 1, fs->font, sizeof *fs->font * (size_t)n);
    memmove(fs->ctx + 1, fs->ctx, sizeof *fs->ctx * (size_t)n);
    memmove(fs->tried + 1, fs->tried, sizeof *fs->tried * (size_t)n);
    memmove(fs->emoji + 1, fs->emoji, sizeof *fs->emoji * (size_t)n);
    fs->path[0] = p, fs->font[0] = NULL, fs->ctx[0] = NULL, fs->tried[0] = false, fs->emoji[0] = emoji;
    forget_lines(fs);  // (their glyphs came from the old order)
}

void gs_fontset_weight(gs_fontset *fs, int weight) { fs->weight = weight; }

static void add_in(gs_fontset *fs, const char *dir, const char *name) {
    char path[512];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    gs_fontset_add(fs, path);
}

// The system's fonts: its interface face (or a monospace one) at a weight first, then its fonts for
// other scripts and emoji. A face is a file of that weight where the system has one, and a variable
// font set to it otherwise.
static gs_fontset *system_fonts(int weight, bool mono) {
    gs_fontset *fs = gs_fontset_new();
    if (!fs) return NULL;
    if (weight != 400) fs->weight = weight;
    bool bold = weight >= 650, semi = weight >= 550 && !bold, medium = weight >= 450 && weight < 550;
#if defined(_WIN32)
    (void)medium;
    const char *root = SDL_getenv("SystemRoot");
    if (!root) root = SDL_getenv("SYSTEMROOT");
    char dir[400];
    SDL_snprintf(dir, sizeof dir, "%s\\Fonts\\", root ? root : "C:\\Windows");
    // Segoe UI has no medium: 500 is drawn regular. Consolas has regular and bold.
    if (mono) add_in(fs, dir, bold || semi ? "consolab.ttf" : "consola.ttf"), add_in(fs, dir, "CascadiaMono.ttf"), add_in(fs, dir, "cour.ttf");
    add_in(fs, dir, bold ? "segoeuib.ttf" : semi ? "seguisb.ttf" : "segoeui.ttf");
    static const char *const names[] = { "segoeui.ttf", "seguiemj.ttf", "seguisym.ttf", "YuGothM.ttc", "msyh.ttc", "malgun.ttf", "Nirmala.ttc", "Nirmala.ttf", "tahoma.ttf", "arial.ttf" };
    for (size_t i = (bold || semi ? 0 : 1); i < sizeof names / sizeof *names; i++) add_in(fs, dir, names[i]);
#elif defined(__APPLE__)
    (void)bold, (void)semi, (void)medium;  // (San Francisco is a variable font: any weight)
    if (mono) add_in(fs, "/System/Library/Fonts/", "SFNSMono.ttf"), add_in(fs, "/System/Library/Fonts/", "Menlo.ttc");
    static const char *const paths[] = { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Apple Color Emoji.ttc",
                                         "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                                         "/System/Library/Fonts/Supplemental/GeezaPro.ttc", "/System/Library/Fonts/Kohinoor.ttc",
                                         "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
#else
    // Noto's variable fonts take any weight (Fedora); elsewhere a file of the weight, where it is installed.
    const char *style = bold ? "Bold" : semi ? "SemiBold" : medium ? "Medium" : "Regular";
    char name[64];
    if (mono) {
        gs_fontset_add(fs, "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf");
        SDL_snprintf(name, sizeof name, "NotoSansMono-%s.ttf", style);
        add_in(fs, "/usr/share/fonts/truetype/noto/", name), add_in(fs, "/usr/share/fonts/noto/", name);
        add_in(fs, "/usr/share/fonts/adwaita-mono-fonts/", bold || semi ? "AdwaitaMono-Bold.ttf" : "AdwaitaMono-Regular.ttf");
        add_in(fs, "/usr/share/fonts/liberation-mono-fonts/", bold || semi ? "LiberationMono-Bold.ttf" : "LiberationMono-Regular.ttf");
        add_in(fs, "/usr/share/fonts/truetype/liberation/", bold || semi ? "LiberationMono-Bold.ttf" : "LiberationMono-Regular.ttf");
        add_in(fs, "/usr/share/fonts/dejavu-sans-mono-fonts/", bold || semi ? "DejaVuSansMono-Bold.ttf" : "DejaVuSansMono.ttf");
        add_in(fs, "/usr/share/fonts/truetype/dejavu/", bold || semi ? "DejaVuSansMono-Bold.ttf" : "DejaVuSansMono.ttf");
    }
    gs_fontset_add(fs, "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf");
    if (weight != 400) {
        SDL_snprintf(name, sizeof name, "NotoSans-%s.ttf", style);
        add_in(fs, "/usr/share/fonts/truetype/noto/", name), add_in(fs, "/usr/share/fonts/noto/", name);
        if (bold || semi) add_in(fs, "/usr/share/fonts/dejavu-sans-fonts/", "DejaVuSans-Bold.ttf"), add_in(fs, "/usr/share/fonts/truetype/dejavu/", "DejaVuSans-Bold.ttf");
    }
    static const char *const paths[] = {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-vf/NotoSansArabic[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSansDevanagari[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        // Where the Noto fonts for a script are missing, Droid's (Fedora's google-droid-sans-fonts), and a Korean font.
        "/usr/share/fonts/google-droid-sans-fonts/DroidKufi-Regular.ttf", "/usr/share/fonts/google-droid-sans-fonts/DroidSansDevanagari-Regular.ttf",
        "/usr/share/fonts/google-droid-sans-fonts/DroidSansHebrew-Regular.ttf", "/usr/share/fonts/google-droid-sans-fonts/DroidSansThai.ttf",
        "/usr/share/fonts/nanum-gothic/NanumGothic.ttf", "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
        // Monochrome emoji, where the colour font is one gesso cannot draw (COLRv1, as on Fedora 43 and later).
        "/usr/share/fonts/google-noto-emoji-fonts/NotoEmoji-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/gdouros-symbola/Symbola.ttf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
    const char *home = SDL_getenv("HOME");  // (a font the user installed, for Korean where the system has none)
    if (home) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s/.local/share/fonts/D2Coding.ttf", home);
        gs_fontset_add(fs, path);
        SDL_snprintf(path, sizeof path, "%s/.local/share/fonts/D2CodingLigature.ttf", home);
        gs_fontset_add(fs, path);
    }
#endif
    return fs;
}

gs_fontset *gs_fontset_system(void) { return system_fonts(400, false); }
gs_fontset *gs_fontset_system_weight(int weight) { return system_fonts(weight, false); }
gs_fontset *gs_fontset_system_mono(int weight) { return system_fonts(weight, true); }

static void forget_lines(gs_fontset *fs) {
    for (int i = 0; i < LINE_CACHE; i++) {
        free(fs->cache[i].text);
        gs_line_free(fs->cache[i].line);
        fs->cache[i] = (cached_line){ 0 };
    }
}

void gs_fontset_free(gs_fontset *fs) {
    if (!fs) return;
    forget_lines(fs);
    for (int i = 0; i < fs->n; i++) {
        if (fs->ctx[i]) kbts_DestroyShapeContext(fs->ctx[i]);
        SDL_free(fs->path[i]), gs_font_free(fs->font[i]);
    }
    free(fs);
}

static gs_font *font_at(gs_fontset *fs, int i) {
    if (!fs->tried[i]) {
        fs->tried[i] = true, fs->font[i] = gs_font_load(fs->path[i]);
        if (fs->font[i] && fs->weight) gs_font_set_weight(fs->font[i], fs->weight);
    }
    return fs->font[i];
}

static bool shapeable(gs_font *f) {
    if (!f->kb_tried) {
        f->kb_tried = true;
        f->kb = kbts_FontFromMemory(f->data, (int)f->len, 0, NULL, NULL);
        f->kb.UserData = f;
    }
    return kbts_FontIsValid(&f->kb);
}

// Code points that do not need a glyph of their own for a font to show a grapheme: joiners and
// variation selectors.
static bool ignorable(uint32_t cp) { return cp == 0x200C || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF); }

// A grapheme shown as an emoji: with the emoji variation selector, a keycap, a regional-indicator
// flag, a skin tone, or pictographs that default to emoji presentation.
static bool emoji_grapheme(const char *p, const char *end) {
    while (p < end) {
        uint32_t cp = gs_utf8_next(&p);
        if (cp == 0xFE0F || cp == 0x20E3 || (cp >= 0x1F1E6 && cp <= 0x1F1FF) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F000 && cp <= 0x1F0FF)) return true;
    }
    return false;
}

static bool covers(gs_font *f, const char *p, const char *end) {
    while (p < end) {
        uint32_t cp = gs_utf8_next(&p);
        if (!ignorable(cp) && !gs_font_glyph(f, cp)) return false;
    }
    return true;
}

bool gs_fontset_vmetrics(gs_fontset *fs, float px, float *ascent, float *descent, float *line_gap) {
    for (int i = 0; i < fs->n; i++)
        if (!fs->emoji[i] && font_at(fs, i)) return gs_font_vmetrics(fs->font[i], px, ascent, descent, line_gap), true;
    return false;
}

// The font for one grapheme: an emoji font for emoji, otherwise the first text font that has all its
// characters, then the first font of any kind that has them (an emoji font often has digits and a
// space of its own, which are not to replace the text font's wherever it stands in the chain), then
// the first with its first character, then the first that loads (its missing glyph).
static int choose(gs_fontset *fs, const char *p, const char *end) {
    if (emoji_grapheme(p, end))
        for (int i = 0; i < fs->n; i++)
            if (fs->emoji[i] && font_at(fs, i) && shapeable(fs->font[i]) && covers(fs->font[i], p, end)) return i;
    for (int i = 0; i < fs->n; i++)
        if (!fs->emoji[i] && font_at(fs, i) && shapeable(fs->font[i]) && covers(fs->font[i], p, end)) return i;
    for (int i = 0; i < fs->n; i++)
        if (fs->emoji[i] && font_at(fs, i) && shapeable(fs->font[i]) && covers(fs->font[i], p, end)) return i;
    const char *q = p;
    uint32_t first = gs_utf8_next(&q);
    for (int i = 0; i < fs->n; i++)
        if (fs->font[i] && fs->font[i]->kb_tried && kbts_FontIsValid(&fs->font[i]->kb) && gs_font_glyph(fs->font[i], first)) return i;
    for (int i = 0; i < fs->n; i++)
        if (font_at(fs, i) && shapeable(fs->font[i])) return i;
    return -1;
}

static kbts_shape_context *context(gs_fontset *fs, int i) {
    if (!fs->ctx[i] && (fs->ctx[i] = kbts_CreateShapeContext(NULL, NULL))) {
        kbts_variation weight = { KBTS_FOURCC('w', 'g', 'h', 't'), fs->font[i]->weight };
        if (fs->font[i]->coords) kbts_ShapePushFont2(fs->ctx[i], &fs->font[i]->kb, &weight, 1, NULL);  // (advances and positioning for that weight)
        else kbts_ShapePushFont(fs->ctx[i], &fs->font[i]->kb);
    }
    return fs->ctx[i];
}

// ---- Bidirectional text ----
//
// Text that mixes directions is ordered by the Unicode Bidirectional Algorithm (UAX 9): each
// character gets an embedding level, even for left-to-right and odd for right-to-left, from its own
// class and from the text around it, and the line is then put in visual order by reversing each
// stretch of a level and above, from the highest level down. So a number after Latin letters stays
// with them inside a right-to-left paragraph, a number after Arabic sits to its left, a bracket pair
// takes one direction for both its halves, and punctuation at the end of a line takes the
// paragraph's. Explicit embedding and isolate controls are passed over as invisible characters:
// the text of a chat or a label does not carry them.

int gs_kb_bidi_class(unsigned cp);  // (gs_kb.c: kb_text_shape's tables)
unsigned gs_kb_mirror(unsigned cp);

// A character's class, in kb_text_shape's numbering: NI covers spaces, punctuation and symbols.
enum { B_NI, B_BN, B_L, B_R, B_NSM, B_AL, B_AN, B_EN, B_ES, B_ET, B_CS };

// The direction of a paragraph: that of its first strongly directional character (rule P2).
static kbts_direction paragraph_direction(const char *utf8) {
    for (const char *p = utf8; *p;) {
        int c = gs_kb_bidi_class(gs_utf8_next(&p));
        if (c == B_L) return KBTS_DIRECTION_LTR;
        if (c == B_R || c == B_AL) return KBTS_DIRECTION_RTL;
    }
    return KBTS_DIRECTION_LTR;
}

// The closing bracket that pairs with an opening one, or 0 for any other character.
static uint32_t closing_bracket(uint32_t cp) {
    static const uint32_t opening[] = { '(', '[', '{', 0x2045, 0x2329, 0x27E6, 0x27E8, 0x27EA, 0x3008, 0x300A, 0x300C, 0x300E, 0x3010, 0x3014, 0x3016, 0xFF08, 0xFF3B, 0xFF5B };
    for (size_t i = 0; i < sizeof opening / sizeof *opening; i++)
        if (opening[i] == cp) return gs_kb_mirror(cp);
    return 0;
}

// The embedding level of each of n code points in a paragraph whose own level is `base` (0 left to
// right, 1 right to left).
static void bidi_levels(const uint32_t *cp, int n, int base, uint8_t *level) {
    uint8_t *t = malloc((size_t)n + 1);
    if (!t) { memset(level, base, (size_t)n); return; }
    int e = base ? B_R : B_L;  // the paragraph's direction, which the text's start and end also count as
    // W1 to W3: marks take the class of what they sit on; a European number after Arabic letters is an
    // Arabic number; Arabic letters are then plain right-to-left.
    for (int i = 0, prev = e, strong = e; i < n; i++) {
        int c = gs_kb_bidi_class(cp[i]);
        if (c == B_NSM || c == B_BN) c = prev;
        if (c == B_L || c == B_R || c == B_AL) strong = c;
        if (c == B_EN && strong == B_AL) c = B_AN;
        t[i] = (uint8_t)c, prev = c;
    }
    for (int i = 0; i < n; i++)
        if (t[i] == B_AL) t[i] = B_R;
    // W4: one separator between two numbers of a kind is part of the number.
    for (int i = 1; i + 1 < n; i++) {
        if (t[i] == B_ES && t[i - 1] == B_EN && t[i + 1] == B_EN) t[i] = B_EN;
        if (t[i] == B_CS && t[i - 1] == t[i + 1] && (t[i - 1] == B_EN || t[i - 1] == B_AN)) t[i] = t[i - 1];
    }
    // W5: signs written against a European number (a percent sign, a currency sign) are part of it.
    for (int i = 0; i < n; i++) {
        if (t[i] != B_ET) continue;
        int j = i;
        while (j < n && t[j] == B_ET) j++;
        if ((i > 0 && t[i - 1] == B_EN) || (j < n && t[j] == B_EN))
            for (int k = i; k < j; k++) t[k] = B_EN;
        i = j - 1;
    }
    // W6 and W7: separators and signs left over are neutral; a European number after left-to-right
    // letters is left-to-right text.
    for (int i = 0, strong = e; i < n; i++) {
        if (t[i] == B_ES || t[i] == B_ET || t[i] == B_CS) t[i] = B_NI;
        if (t[i] == B_L || t[i] == B_R) strong = t[i];
        if (t[i] == B_EN && strong == B_L) t[i] = B_L;
    }
    #define STRONG(c) ((c) == B_L ? B_L : (c) == B_R || (c) == B_EN || (c) == B_AN ? B_R : 0)  // (numbers count as right-to-left for what is around them)
    // N0: a pair of brackets takes one direction: the paragraph's when it holds text of that
    // direction; otherwise that of the text it holds, if the text before it runs the same way, and
    // the paragraph's if not.
    struct { int at; uint32_t close; } open[64];
    for (int i = 0, depth = 0; i < n; i++) {
        if (t[i] != B_NI) continue;
        uint32_t close = closing_bracket(cp[i]);
        if (close && depth < 64) { open[depth].at = i, open[depth++].close = close; continue; }
        int match = -1;
        for (int k = depth - 1; k >= 0 && match < 0; k--)
            if (open[k].close == cp[i]) match = k;
        if (match < 0) continue;
        int from = open[match].at, inside_e = 0, inside_o = 0, before = e;
        depth = match;
        for (int k = from + 1; k < i; k++) {
            int s = STRONG(t[k]);
            if (s == e) inside_e = 1; else if (s) inside_o = 1;
        }
        for (int k = from - 1; k >= 0; k--)
            if (STRONG(t[k])) { before = STRONG(t[k]); break; }
        if (inside_e) t[from] = t[i] = (uint8_t)e;
        else if (inside_o) t[from] = t[i] = (uint8_t)(before != e ? before : e);
    }
    // N1 and N2: neutrals between text of one direction take it; any others take the paragraph's.
    for (int i = 0; i < n; i++) {
        if (t[i] != B_NI) continue;
        int j = i, before = e, after = e;
        while (j < n && t[j] == B_NI) j++;
        if (i > 0) before = STRONG(t[i - 1]);
        if (j < n) after = STRONG(t[j]);
        for (int k = i; k < j; k++) t[k] = (uint8_t)(before == after ? before : e);
        i = j - 1;
    }
    #undef STRONG
    // I1 and I2: text against the paragraph's direction goes one level up, and numbers in
    // right-to-left text one further, so their digits still read left to right.
    for (int i = 0; i < n; i++) level[i] = (uint8_t)(base ? (t[i] == B_R ? 1 : 2) : (t[i] == B_L ? 0 : t[i] == B_R ? 1 : 2));
    // L1: spaces at the end of the line belong to the paragraph, whatever came before them.
    for (int i = n - 1; i >= 0 && (cp[i] == ' ' || cp[i] == '\t'); i--) level[i] = (uint8_t)base;
    free(t);
}

// A stretch of the line shaped as one: its glyphs (in visual order) and its level.
typedef struct { int first, count, level; } piece;

// Shapes text[start, start + len), which uses one font and has one level, appending its glyphs to the
// line and a piece for each run the shaper makes of it, in the order the pieces are read.
static void shape_stretch(gs_fontset *fs, int font, int level, const char *text, int start, int len, gs_line *l, int *cap, piece **pieces, int *npieces, int *pieces_cap) {
    kbts_shape_context *ctx = context(fs, font);
    if (!ctx) return;
    gs_font *f = fs->font[font];
    float scale = em_scale(f, l->px);
    bool rtl = level & 1;
    kbts_ShapeBegin(ctx, rtl ? KBTS_DIRECTION_RTL : KBTS_DIRECTION_LTR, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8WithUserId(ctx, text + start, len, start, KBTS_USER_ID_GENERATION_MODE_SOURCE_INDEX);
    kbts_ShapeEnd(ctx);
    kbts_run run;
    while (kbts_ShapeRun(ctx, &run)) {
        int first = l->count;
        kbts_glyph *g;
        while (kbts_GlyphIteratorNext(&run.Glyphs, &g)) {
            if (l->count == *cap) l->glyphs = realloc(l->glyphs, sizeof *l->glyphs * (size_t)(*cap = *cap ? *cap * 2 : 32));
            kbts_shape_codepoint cp;
            int cluster = kbts_ShapeGetShapeCodepoint(ctx, g->UserIdOrCodepointIndex, &cp) ? cp.UserId : start;
            l->glyphs[l->count++] = (gs_glyph_pos){ f, g->Id, g->OffsetX * scale, -g->OffsetY * scale, g->AdvanceX * scale, cluster, rtl };
        }
        // The shaper orders a run's glyphs for the direction it finds in the run alone; where that is not
        // the level's (a stretch of neutrals, read here by the text around it), they are turned round.
        if ((run.Direction == KBTS_DIRECTION_RTL) != rtl)
            for (int a = first, b = l->count - 1; a < b; a++, b--) { gs_glyph_pos t = l->glyphs[a]; l->glyphs[a] = l->glyphs[b], l->glyphs[b] = t; }
        if (*npieces == *pieces_cap) *pieces = realloc(*pieces, sizeof **pieces * (size_t)(*pieces_cap = *pieces_cap ? *pieces_cap * 2 : 8));
        (*pieces)[(*npieces)++] = (piece){ first, l->count - first, level };
    }
}

// The grapheme boundaries of a text: byte offsets, the last being its length.
static int graphemes(const char *utf8, int len, int **out) {
    int n = 0, cap = 16;
    *out = malloc(sizeof **out * (size_t)cap);
    kbts_break_state state;
    kbts_BreakBegin(&state, KBTS_DIRECTION_DONT_KNOW, KBTS_JAPANESE_LINE_BREAK_STYLE_NORMAL, 0);
    for (const char *p = utf8; p < utf8 + len;) {
        const char *start = p;
        uint32_t cp = gs_utf8_next(&p);
        kbts_BreakAddCodepoint(&state, (int)cp, (int)(p - start), p >= utf8 + len);
        kbts_break b;
        while (kbts_Break(&state, &b)) {
            if (!(b.Flags & KBTS_BREAK_FLAG_GRAPHEME) || b.Position <= 0 || (n && (*out)[n - 1] >= b.Position)) continue;
            if (n == cap) *out = realloc(*out, sizeof **out * (size_t)(cap *= 2));
            (*out)[n++] = b.Position;
        }
    }
    if (!n || (*out)[n - 1] != len) {
        if (n == cap) *out = realloc(*out, sizeof **out * (size_t)(cap + 1));
        (*out)[n++] = len;
    }
    return n;
}

// The level of each code point of text[from, to), by byte offset from `from`, in a paragraph of
// direction `rtl`. free() the result.
static uint8_t *levels_of(const char *text, int from, int to, bool rtl) {
    int len = to - from, ncp = 0;
    uint32_t *cps = malloc(sizeof *cps * (size_t)(len + 1));
    int *at = malloc(sizeof *at * (size_t)(len + 1));
    uint8_t *levels = malloc((size_t)len + 1), *by_byte = calloc((size_t)len + 1, 1);
    for (const char *p = text + from; p < text + to && *p;) at[ncp] = (int)(p - text) - from, cps[ncp++] = gs_utf8_next(&p);
    bidi_levels(cps, ncp, rtl, levels);
    for (int i = 0; i < ncp; i++) by_byte[at[i]] = levels[i];
    free(cps), free(at), free(levels);
    return by_byte;
}

// Shapes a line (a whole text of its own here) whose code points have the levels given by byte offset.
static gs_line *shape_levels(gs_fontset *fs, float px, const char *utf8, bool rtl, const uint8_t *by_byte) {
    int len = (int)strlen(utf8);
    gs_line *l = calloc(1, sizeof *l);
    if (!l || !len) return l;
    l->px = px, l->rtl = rtl;
    int *bounds, cap = 0, npieces = 0, pieces_cap = 0;
    int nb = graphemes(utf8, len, &bounds);
    piece *pieces = NULL;
    // Consecutive graphemes of one font and one level are shaped together.
    for (int i = 0, start = 0, font = -1, level = -1, stretch = 0; i <= nb; i++) {
        int f = i < nb ? choose(fs, utf8 + start, utf8 + bounds[i]) : -2, lv = i < nb ? by_byte[start] : -2;
        if (f != font || lv != level) {
            if (font >= 0) shape_stretch(fs, font, level, utf8, stretch, start - stretch, l, &cap, &pieces, &npieces, &pieces_cap);
            font = f, level = lv, stretch = start;
        }
        if (i < nb) start = bounds[i];
    }
    free(bounds);
    // Pieces come in the order they are read; on the line they go in visual order (rule L2): from the
    // highest level down to the lowest odd one, each stretch of pieces at that level or above is
    // reversed. A piece's own glyphs are in visual order already. A right-to-left piece cut in several
    // by the shaper has its parts in reading order, which the reversal puts right as well.
    int *order = malloc(sizeof *order * (size_t)(npieces ? npieces : 1)), highest = 0, lowest_odd = 99;
    for (int i = 0; i < npieces; i++) {
        order[i] = i;
        if (pieces[i].level > highest) highest = pieces[i].level;
        if ((pieces[i].level & 1) && pieces[i].level < lowest_odd) lowest_odd = pieces[i].level;
    }
    for (int lv = highest; lv >= lowest_odd; lv--)
        for (int i = 0; i < npieces;) {
            int j = i;
            while (j < npieces && pieces[order[j]].level >= lv) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) { int t = order[a]; order[a] = order[b], order[b] = t; }
            i = j > i ? j : i + 1;
        }
    gs_glyph_pos *visual = malloc(sizeof *visual * (size_t)(l->count ? l->count : 1));
    float pen = 0;
    int k = 0;
    for (int i = 0; i < npieces; i++)
        for (int j = 0; j < pieces[order[i]].count; j++) {
            gs_glyph_pos gp = l->glyphs[pieces[order[i]].first + j];
            gp.x += pen;
            pen += gp.advance;
            visual[k++] = gp;
        }
    free(l->glyphs), free(order), free(pieces);
    l->glyphs = visual;
    l->width = pen;
    return l;
}

gs_line *gs_fontset_shape_dir(gs_fontset *fs, float px, const char *utf8, int dir) {
    bool rtl = dir == GS_DIR_RTL || (dir == GS_DIR_AUTO && paragraph_direction(utf8) == KBTS_DIRECTION_RTL);
    uint8_t *levels = levels_of(utf8, 0, (int)strlen(utf8), rtl);
    gs_line *l = shape_levels(fs, px, utf8, rtl, levels);
    free(levels);
    return l;
}

gs_line *gs_fontset_shape_part(gs_fontset *fs, float px, const char *utf8, int start, int end, int dir) {
    // Levels come from the whole paragraph the line is part of: what a comma at the end of this line
    // is depends on the word that begins the next.
    int len = (int)strlen(utf8), from = start = SDL_clamp(start, 0, len), to = end = SDL_clamp(end, start, len);
    while (from > 0 && utf8[from - 1] != '\n') from--;
    while (to < len && utf8[to] != '\n') to++;
    char *line = malloc((size_t)(end - start) + 1), *paragraph = malloc((size_t)(to - from) + 1);
    memcpy(line, utf8 + start, (size_t)(end - start)), line[end - start] = 0;
    memcpy(paragraph, utf8 + from, (size_t)(to - from)), paragraph[to - from] = 0;
    bool rtl = dir == GS_DIR_RTL || (dir == GS_DIR_AUTO && paragraph_direction(paragraph) == KBTS_DIRECTION_RTL);
    uint8_t *levels = levels_of(utf8, from, to, rtl), *mine = levels + (start - from);
    for (int i = end - start - 1; i >= 0 && (line[i] == ' ' || line[i] == '\t'); i--) mine[i] = rtl;  // (rule L1 again: spaces at the end of this line)
    gs_line *l = shape_levels(fs, px, line, rtl, mine);
    free(levels), free(line), free(paragraph);
    return l;
}

gs_line *gs_fontset_shape(gs_fontset *fs, float px, const char *utf8) { return gs_fontset_shape_dir(fs, px, utf8, GS_DIR_AUTO); }

void gs_line_free(gs_line *l) {
    if (l) free(l->glyphs), free(l);
}

const gs_line *gs_fontset_line_dir(gs_fontset *fs, float px, const char *utf8, int dir) {
    int size4 = (int)lroundf(px * 4);
    uint32_t h = 2166136261u ^ (uint32_t)size4 ^ (uint32_t)dir * 40503u;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) h = (h ^ *p) * 16777619u;
    cached_line *c = &fs->cache[h % LINE_CACHE];
    if (c->line && c->size4 == size4 && c->dir == dir && !c->around && !strcmp(c->text, utf8)) return c->line;
    free(c->text), gs_line_free(c->line);
    c->text = SDL_strdup(utf8), c->size4 = size4, c->dir = dir, c->around = 0, c->line = gs_fontset_shape_dir(fs, px, utf8, dir);
    return c->line;
}

const gs_line *gs_fontset_line_part(gs_fontset *fs, float px, const char *utf8, int start, int end, int dir) {
    // Kept by the line's own text and by what stands around it in its paragraph, which its levels depend on.
    int size4 = (int)lroundf(px * 4), len = (int)strlen(utf8), from = start = SDL_clamp(start, 0, len), to = end = SDL_clamp(end, start, len);
    while (from > 0 && utf8[from - 1] != '\n') from--;
    while (to < len && utf8[to] != '\n') to++;
    uint32_t h = 2166136261u ^ (uint32_t)size4 ^ (uint32_t)dir * 40503u, around = 2166136261u ^ (uint32_t)(start - from) * 31u;
    for (int i = start; i < end; i++) h = (h ^ (unsigned char)utf8[i]) * 16777619u;
    for (int i = from; i < to; i++) around = (around ^ (unsigned char)utf8[i]) * 16777619u;
    around += !around;
    cached_line *c = &fs->cache[(h ^ around) % LINE_CACHE];
    if (c->line && c->size4 == size4 && c->dir == dir && c->around == around && (int)strlen(c->text) == end - start && !memcmp(c->text, utf8 + start, (size_t)(end - start))) return c->line;
    free(c->text), gs_line_free(c->line);
    c->text = malloc((size_t)(end - start) + 1);
    memcpy(c->text, utf8 + start, (size_t)(end - start)), c->text[end - start] = 0;
    c->size4 = size4, c->dir = dir, c->around = around, c->line = gs_fontset_shape_part(fs, px, utf8, start, end, dir);
    return c->line;
}

const gs_line *gs_fontset_line(gs_fontset *fs, float px, const char *utf8) { return gs_fontset_line_dir(fs, px, utf8, GS_DIR_AUTO); }

float gs_line_draw(gs_glyphs *g, const gs_line *l, float x, float y, SDL_FColor colour) {
    if (!l) return 0;
    for (int i = 0; i < l->count; i++) gs_glyphs_draw(g, l->glyphs[i].font, l->glyphs[i].glyph, l->px, x + l->glyphs[i].x, y + l->glyphs[i].y, 0, colour, 1);
    return l->width;
}

float gs_line_caret(const gs_line *l, int offset) {
    // The glyph of the cluster that starts at or after the offset; a right-to-left glyph's caret is on its right.
    float best = l->rtl ? 0 : l->width;
    int best_cluster = INT32_MAX;
    for (int i = 0; i < l->count; i++) {
        const gs_glyph_pos *g = &l->glyphs[i];
        if (g->cluster >= offset && g->cluster < best_cluster) best_cluster = g->cluster, best = g->rtl ? g->x + g->advance : g->x;
    }
    return best;
}

int gs_line_hit(const gs_line *l, float x, const char *utf8) {
    int best = 0;
    float best_d = INFINITY;
    int len = (int)strlen(utf8);
    for (int i = 0; i <= l->count; i++) {
        int offset = i < l->count ? l->glyphs[i].cluster : len;
        float d = fabsf(gs_line_caret(l, offset) - x);
        if (d < best_d) best_d = d, best = offset;
    }
    // A cluster's end, past the last glyph of a left-to-right line.
    if (fabsf(gs_line_caret(l, len) - x) < best_d) best = len;
    return best;
}

// ---- Line wrapping ----

enum { BREAK_GRAPHEME = 1, BREAK_SOFT = 2, BREAK_HARD = 4 };

// The break opportunities of a text by byte offset: grapheme boundaries, where a line may wrap
// (Unicode's line breaking: after spaces and hyphens, between CJK characters), and where it must end.
static uint8_t *break_flags(const char *utf8, int len) {
    uint8_t *flags = calloc((size_t)len + 1, 1);
    kbts_break_state state;
    kbts_BreakBegin(&state, KBTS_DIRECTION_DONT_KNOW, KBTS_JAPANESE_LINE_BREAK_STYLE_NORMAL, 0);
    for (const char *p = utf8; flags && p < utf8 + len;) {
        const char *start = p;
        uint32_t cp = gs_utf8_next(&p);
        kbts_BreakAddCodepoint(&state, (int)cp, (int)(p - start), p >= utf8 + len);
        kbts_break b;
        while (kbts_Break(&state, &b)) {
            if (b.Position <= 0 || b.Position > len) continue;
            if (b.Flags & KBTS_BREAK_FLAG_GRAPHEME) flags[b.Position] |= BREAK_GRAPHEME;
            if (b.Flags & KBTS_BREAK_FLAG_LINE_SOFT) flags[b.Position] |= BREAK_SOFT;
            if (b.Flags & KBTS_BREAK_FLAG_LINE_HARD) flags[b.Position] |= BREAK_HARD;
        }
    }
    if (flags) flags[len] |= BREAK_HARD | BREAK_GRAPHEME;
    return flags;
}

// The end of the text from s to p without the spaces and line ends that hang off its last line.
static int visible_end(const char *utf8, int s, int p) {
    while (p > s && (utf8[p - 1] == ' ' || utf8[p - 1] == '\t' || utf8[p - 1] == '\n' || utf8[p - 1] == '\r')) p--;
    return p;
}

static void wrap_add(gs_wrap *w, int *cap, int start, int end, float width, bool rtl) {
    if (w->count == *cap) w->lines = realloc(w->lines, sizeof *w->lines * (size_t)(*cap = *cap ? *cap * 2 : 8));
    w->lines[w->count++] = (gs_wrap_line){ start, end, width, rtl };
}

gs_wrap *gs_fontset_wrap(gs_fontset *fs, float px, const char *utf8, float width) {
    int len = (int)strlen(utf8), cap = 0;
    gs_wrap *w = calloc(1, sizeof *w);
    uint8_t *flags = break_flags(utf8, len);
    float *cum = NULL;
    if (!w || !flags) return free(flags), free(w), NULL;
    for (int ps = 0;;) {  // a paragraph: up to the next hard break
        int pe = ps;
        if (ps < len)
            for (pe = ps + 1; pe < len && !(flags[pe] & BREAK_HARD);) pe++;
        // Where each byte offset of the paragraph falls along it: the advances of the glyphs before it.
        int plen = pe - ps, ve = visible_end(utf8, ps, pe) - ps;
        cum = realloc(cum, sizeof *cum * (size_t)(plen + 2));
        memset(cum, 0, sizeof *cum * (size_t)(plen + 2));
        char *text = malloc((size_t)ve + 1);
        memcpy(text, utf8 + ps, (size_t)ve), text[ve] = 0;
        bool rtl = paragraph_direction(text) == KBTS_DIRECTION_RTL;
        gs_line *shaped = ve ? gs_fontset_shape_dir(fs, px, text, rtl ? GS_DIR_RTL : GS_DIR_LTR) : NULL;
        for (int i = 0; shaped && i < shaped->count; i++)
            if (shaped->glyphs[i].cluster >= 0 && shaped->glyphs[i].cluster < plen) cum[shaped->glyphs[i].cluster + 1] += shaped->glyphs[i].advance;
        for (int i = 1; i <= plen; i++) cum[i] += cum[i - 1];
        gs_line_free(shaped), free(text);
        #define SPAN(a, b) (cum[(b) - ps] - cum[(a) - ps])
        int s = ps;
        do {
            int end = s, next = s, best = -1, fit = s, fit_end = s;
            for (int p = s + 1; p <= pe; p++) {
                if (!(flags[p] & BREAK_GRAPHEME) && p != pe) continue;
                int v = visible_end(utf8, s, p);
                if (SPAN(s, v) > width) {  // wrap at the last place a line may break, else inside the word
                    if (best > s) end = visible_end(utf8, s, best), next = best;
                    else if (fit > s) end = fit_end, next = fit;
                    else end = v, next = p;  // (one character wider than the line)
                    break;
                }
                fit = p, fit_end = v;
                if (flags[p] & (BREAK_SOFT | BREAK_HARD)) best = p;
                if (p == pe) { end = v, next = pe; break; }
            }
            if (next <= s && s < pe) end = visible_end(utf8, s, pe), next = pe;  // (always a step forward)
            wrap_add(w, &cap, s, end, SPAN(s, end), rtl);
            s = next;
        } while (s < pe);
        #undef SPAN
        if (pe >= len) {
            if (pe > ps && (utf8[len - 1] == '\n' || utf8[len - 1] == '\r')) wrap_add(w, &cap, len, len, 0, rtl);  // after a final newline
            break;
        }
        ps = pe;
    }
    free(cum), free(flags);
    return w;
}

gs_wrap *gs_fontset_wrap_lines(gs_fontset *fs, float px, const char *utf8, float width, int max_lines) {
    gs_wrap *w = gs_fontset_wrap(fs, px, utf8, width);
    if (!w || max_lines < 1 || w->count <= max_lines) return w;
    w->count = max_lines, w->cut = true;
    // The last line kept gives up characters from its end until an ellipsis fits after it.
    gs_wrap_line *last = &w->lines[max_lines - 1];
    float ellipsis = gs_fontset_width(fs, px, "\xE2\x80\xA6");
    char *text = malloc((size_t)(last->end - last->start) + 1);
    for (int end = last->end; text;) {
        while (end > last->start && (utf8[end - 1] == ' ' || utf8[end - 1] == '\t')) end--;
        memcpy(text, utf8 + last->start, (size_t)(end - last->start)), text[end - last->start] = 0;
        const gs_line *l = gs_fontset_line_dir(fs, px, text, last->rtl ? GS_DIR_RTL : GS_DIR_LTR);
        float fits = l ? l->width : 0;
        if (fits + ellipsis <= width || end == last->start) { last->end = end, last->width = fits + ellipsis; break; }
        end = last->start + gs_utf8_grapheme_prev(text, end - last->start);
    }
    free(text);
    return w;
}

void gs_wrap_free(gs_wrap *w) {
    if (w) free(w->lines), free(w);
}

float gs_fontset_draw(gs_glyphs *g, gs_fontset *fs, float px, float x, float y, const char *utf8, SDL_FColor colour) {
    return gs_line_draw(g, gs_fontset_line(fs, px, utf8), x, y, colour);
}

float gs_fontset_width(gs_fontset *fs, float px, const char *utf8) {
    const gs_line *l = gs_fontset_line(fs, px, utf8);
    return l ? l->width : 0;
}

// ---- Graphemes ----

// The byte offset of the grapheme boundary after (or before) `offset`: what a text cursor steps
// over, so an emoji sequence or a letter with its marks moves as one.
static int grapheme_step(const char *utf8, int offset, bool forward) {
    int len = (int)strlen(utf8), prev = 0;
    kbts_break_state state;
    kbts_BreakBegin(&state, KBTS_DIRECTION_DONT_KNOW, KBTS_JAPANESE_LINE_BREAK_STYLE_NORMAL, 0);
    for (const char *p = utf8; *p;) {
        const char *start = p;
        uint32_t cp = gs_utf8_next(&p);
        kbts_BreakAddCodepoint(&state, (int)cp, (int)(p - start), !*p);
        kbts_break b;
        while (kbts_Break(&state, &b)) {
            if (!(b.Flags & KBTS_BREAK_FLAG_GRAPHEME) || b.Position <= 0) continue;
            if (forward && b.Position > offset) return b.Position;
            if (!forward && b.Position >= offset) return prev;
            prev = b.Position;
        }
    }
    return forward ? len : prev < offset ? prev : 0;
}

int gs_utf8_grapheme_next(const char *utf8, int offset) { return grapheme_step(utf8, offset, true); }
int gs_utf8_grapheme_prev(const char *utf8, int offset) { return offset <= 0 ? 0 : grapheme_step(utf8, offset, false); }
