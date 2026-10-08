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

// ---- Glyph atlas ----

// One cached glyph: where it sits in the atlas and its offset from the pen position.
typedef struct {
    const gs_font *font;
    int glyph, size4;  // size in quarter pixels
    int x, y, w, h, ox, oy;
    bool pending;      // rasterized, waiting for gs_glyphs_begin_frame to upload it
    bool colour;       // its own colours, drawn untinted
} slot;

typedef struct { SDL_Rect at; uint32_t *px; } upload;

#define SLOTS 4096  // open addressing; the atlas is cleared well before this fills

struct gs_glyphs {
    SDL_Renderer *r;
    SDL_Texture *tex;
    int size, shelf_x, shelf_y, shelf_h, used;
    slot slots[SLOTS];
    bool deferred;     // gs_glyphs_begin_frame is called: uploads wait for it
    upload *uploads;
    int nuploads, uploads_cap;
};

gs_glyphs *gs_glyphs_new(SDL_Renderer *r, int atlas_size) {
    gs_glyphs *g = calloc(1, sizeof *g);
    g->r = r;
    g->size = atlas_size;
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
    g->deferred = true;
    for (int i = 0; i < g->nuploads; i++) SDL_UpdateTexture(g->tex, &g->uploads[i].at, g->uploads[i].px, g->uploads[i].at.w * 4);
    if (g->nuploads)
        for (int i = 0; i < SLOTS; i++) g->slots[i].pending = false;
    drop_uploads(g);
}

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

// Finds a glyph in the cache, rasterizing it into the atlas on first use. NULL if it cannot fit.
static slot *lookup(gs_glyphs *g, const gs_font *f, int glyph, int size4) {
    uint32_t h = ((uint32_t)(uintptr_t)f * 31u + (uint32_t)glyph) * 2654435761u ^ (uint32_t)size4 * 40503u;
    for (int tries = 0; tries < 2; tries++) {
        for (uint32_t i = h;; i++) {
            slot *s = &g->slots[i % SLOTS];
            if (s->font == f && s->glyph == glyph && s->size4 == size4) return s;
            if (s->font) continue;
            if (g->used > SLOTS * 3 / 4) break;  // too full: clear and start over
            float scale = em_scale(f, size4 / 4.0f);
            image img = { 0 };
            bool colour = colour_glyph(f, glyph, scale, &img);
            if (!colour) {
                int x0, y0, x1, y1;
                stbtt_GetGlyphBitmapBox(&f->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
                img.w = x1 - x0 + 2, img.h = y1 - y0 + 2, img.ox = x0 - 1, img.oy = y0 - 1;  // one clear pixel each side for linear filtering
            }
            int w = img.w, hh = img.h;
            if (w > g->size || hh > g->size) return free(img.px), NULL;
            if (g->shelf_x + w > g->size) g->shelf_x = 0, g->shelf_y += g->shelf_h, g->shelf_h = 0;
            if (g->shelf_y + hh > g->size) { free(img.px); break; }
            uint32_t *px = img.px;
            if (!colour) {
                unsigned char *cov = calloc((size_t)w * hh, 1);
                stbtt_MakeGlyphBitmap(&f->info, cov + w + 1, w - 2, hh - 2, w, scale, scale, glyph);
                px = malloc((size_t)w * hh * 4);
                for (int k = 0; k < w * hh; k++) px[k] = SDL_Swap32LE(0x00FFFFFFu | (uint32_t)cov[k] << 24);
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
            *s = (slot){ f, glyph, size4, g->shelf_x, g->shelf_y, w, hh, img.ox, img.oy, g->deferred, colour };
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
    slot *s = lookup(g, f, glyph, (int)lroundf(px * 4));
    if (!s || s->w <= 2 || s->pending) return;
    float wr = reveal >= 1 ? (float)s->w : s->w * reveal;
    SDL_FRect src = { (float)s->x, (float)s->y, wr, (float)s->h };
    SDL_FRect dst = { x + s->ox, y + s->oy, wr, (float)s->h };
    SDL_FPoint centre = { (float)-s->ox, (float)-s->oy };
    if (s->colour) SDL_SetTextureColorModFloat(g->tex, 1, 1, 1);  // (its own colours)
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

#define MAX_FONTS 16
#define LINE_CACHE 512  // shaped lines kept, so text drawn every frame is shaped once

typedef struct {
    char *text;
    int size4;  // px in quarter pixels
    gs_line *line;
} cached_line;

struct gs_fontset {
    char *path[MAX_FONTS];
    gs_font *font[MAX_FONTS];
    kbts_shape_context *ctx[MAX_FONTS];  // a shaping context holding just this font, made on first use
    bool tried[MAX_FONTS], emoji[MAX_FONTS];
    int n;
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

gs_fontset *gs_fontset_system(void) {
    gs_fontset *fs = gs_fontset_new();
#if defined(_WIN32)
    const char *root = SDL_getenv("SystemRoot");
    if (!root) root = SDL_getenv("SYSTEMROOT");
    static const char *const names[] = { "segoeui.ttf", "seguiemj.ttf", "seguisym.ttf", "YuGothM.ttc", "msyh.ttc", "malgun.ttf", "Nirmala.ttc", "Nirmala.ttf", "tahoma.ttf", "arial.ttf" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s\\Fonts\\%s", root ? root : "C:\\Windows", names[i]);
        gs_fontset_add(fs, path);
    }
#elif defined(__APPLE__)
    static const char *const paths[] = { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Apple Color Emoji.ttc",
                                         "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc",
                                         "/System/Library/Fonts/Supplemental/GeezaPro.ttc", "/System/Library/Fonts/Kohinoor.ttc",
                                         "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
#else
    static const char *const paths[] = {
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-vf/NotoSansArabic[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSansDevanagari[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        // Monochrome emoji, where the colour font is one gesso cannot draw (COLRv1, as on Fedora 43 and later).
        "/usr/share/fonts/google-noto-emoji-fonts/NotoEmoji-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/gdouros-symbola/Symbola.ttf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
#endif
    return fs;
}

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
    if (!fs->tried[i]) fs->tried[i] = true, fs->font[i] = gs_font_load(fs->path[i]);
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

// The font for one grapheme: an emoji font for emoji, otherwise the first font that has all its
// characters, then the first with its first character, then the first that loads (its missing glyph).
static int choose(gs_fontset *fs, const char *p, const char *end) {
    if (emoji_grapheme(p, end))
        for (int i = 0; i < fs->n; i++)
            if (fs->emoji[i] && font_at(fs, i) && shapeable(fs->font[i]) && covers(fs->font[i], p, end)) return i;
    for (int i = 0; i < fs->n; i++)
        if (font_at(fs, i) && shapeable(fs->font[i]) && covers(fs->font[i], p, end)) return i;
    const char *q = p;
    uint32_t first = gs_utf8_next(&q);
    for (int i = 0; i < fs->n; i++)
        if (fs->font[i] && fs->font[i]->kb_tried && kbts_FontIsValid(&fs->font[i]->kb) && gs_font_glyph(fs->font[i], first)) return i;
    for (int i = 0; i < fs->n; i++)
        if (font_at(fs, i) && shapeable(fs->font[i])) return i;
    return -1;
}

static kbts_shape_context *context(gs_fontset *fs, int i) {
    if (!fs->ctx[i] && (fs->ctx[i] = kbts_CreateShapeContext(NULL, NULL))) kbts_ShapePushFont(fs->ctx[i], &fs->font[i]->kb);
    return fs->ctx[i];
}

// A character's strong direction: 1 for the right-to-left scripts' blocks, 0 for letters elsewhere, -1
// for the rest (spaces, punctuation, digits, symbols and emoji, which take their direction from context).
static int strong(uint32_t cp) {
    if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||
        (cp >= 0x10800 && cp <= 0x10FFF) || (cp >= 0x1E800 && cp <= 0x1EFFF))
        return 1;
    bool letter = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x1FFF && cp != 0xD7 && cp != 0xF7) ||
                  (cp >= 0x2C00 && cp <= 0xFB1C) || (cp >= 0x10000 && cp < 0x1F000) || cp >= 0x20000;
    return letter ? 0 : -1;
}

// The direction of a paragraph: that of its first strongly directional character (Unicode's rule P2).
static kbts_direction paragraph_direction(const char *utf8) {
    for (const char *p = utf8; *p;) {
        int d = strong(gs_utf8_next(&p));
        if (d >= 0) return d ? KBTS_DIRECTION_RTL : KBTS_DIRECTION_LTR;
    }
    return KBTS_DIRECTION_LTR;
}

typedef struct { int first, count; bool rtl; int strong; } run_span;  // strong: 1 right-to-left, 0 left-to-right, -1 neither

// Shapes one stretch of text that uses one font, appending its runs and glyphs to the line.
static void shape_stretch(gs_fontset *fs, int font, kbts_direction paragraph, const char *text, int start, int len, gs_line *l,
                          int *cap, run_span **runs, int *nruns, int *runs_cap) {
    kbts_shape_context *ctx = context(fs, font);
    if (!ctx) return;
    gs_font *f = fs->font[font];
    float scale = em_scale(f, l->px);
    kbts_ShapeBegin(ctx, paragraph, KBTS_LANGUAGE_DONT_KNOW);
    kbts_ShapeUtf8WithUserId(ctx, text + start, len, start, KBTS_USER_ID_GENERATION_MODE_SOURCE_INDEX);
    kbts_ShapeEnd(ctx);
    kbts_run run;
    while (kbts_ShapeRun(ctx, &run)) {
        bool rtl = run.Direction == KBTS_DIRECTION_RTL;
        int first = l->count;
        kbts_glyph *g;
        while (kbts_GlyphIteratorNext(&run.Glyphs, &g)) {
            if (l->count == *cap) l->glyphs = realloc(l->glyphs, sizeof *l->glyphs * (size_t)(*cap = *cap ? *cap * 2 : 32));
            kbts_shape_codepoint cp;
            int cluster = kbts_ShapeGetShapeCodepoint(ctx, g->UserIdOrCodepointIndex, &cp) ? cp.UserId : start;
            l->glyphs[l->count++] = (gs_glyph_pos){ f, g->Id, g->OffsetX * scale, -g->OffsetY * scale, g->AdvanceX * scale, cluster, rtl };
        }
        // The run's neutral ends (spaces and punctuation before its first or after its last strong
        // character) become runs of their own, so they are placed by the text around them rather than
        // carried with the run. Glyphs are in visual order: a right-to-left run's logical start is on its right.
        int end = l->count, lo = first, hi = end, dir = -1;
        for (; lo < hi; lo++) {
            const char *c = text + l->glyphs[lo].cluster;
            if ((dir = strong(gs_utf8_next(&c))) >= 0) break;
        }
        for (; hi > lo; hi--) {
            const char *c = text + l->glyphs[hi - 1].cluster;
            if (strong(gs_utf8_next(&c)) >= 0) break;
        }
        // In logical order: the start's neutrals, the strong core, the end's neutrals.
        int spans[3][2] = { { rtl ? hi : first, rtl ? end : lo }, { lo, hi }, { rtl ? first : hi, rtl ? lo : end } };
        int kinds[3] = { -1, dir, -1 };
        if (dir < 0) spans[0][0] = first, spans[0][1] = end, spans[1][0] = spans[1][1] = spans[2][0] = spans[2][1] = 0;
        for (int i = 0; i < 3; i++) {
            if (spans[i][1] <= spans[i][0]) continue;
            if (*nruns == *runs_cap) *runs = realloc(*runs, sizeof **runs * (size_t)(*runs_cap = *runs_cap ? *runs_cap * 2 : 8));
            (*runs)[(*nruns)++] = (run_span){ spans[i][0], spans[i][1] - spans[i][0], rtl, kinds[i] };
        }
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

gs_line *gs_fontset_shape(gs_fontset *fs, float px, const char *utf8) {
    int len = (int)strlen(utf8);
    gs_line *l = calloc(1, sizeof *l);
    if (!l || !len) return l;
    l->px = px;
    int *bounds, cap = 0, nruns = 0, runs_cap = 0;
    kbts_direction paragraph = paragraph_direction(utf8);
    int nb = graphemes(utf8, len, &bounds);
    l->rtl = paragraph == KBTS_DIRECTION_RTL;
    run_span *runs = NULL;
    // Consecutive graphemes using the same font are shaped together.
    for (int i = 0, start = 0, font = -1, stretch = 0; i <= nb; i++) {
        int f = i < nb ? choose(fs, utf8 + start, utf8 + bounds[i]) : -2;
        if (f != font) {
            if (font >= 0) shape_stretch(fs, font, paragraph, utf8, stretch, start - stretch, l, &cap, &runs, &nruns, &runs_cap);
            font = f, stretch = start;
        }
        if (i < nb) start = bounds[i];
    }
    free(bounds);
    // A run of neutral characters takes the direction of the strong text on both sides when they agree,
    // and the paragraph's otherwise (Unicode's rules N1 and N2), whatever its stretch alone suggested.
    for (int i = 0; i < nruns; i++) {
        if (runs[i].strong >= 0) { runs[i].rtl = runs[i].strong == 1; continue; }
        int before = -1, after = -1;
        for (int j = i - 1; j >= 0 && before < 0; j--) before = runs[j].strong;
        for (int j = i + 1; j < nruns && after < 0; j++) after = runs[j].strong;
        int sos = l->rtl ? 1 : 0;  // the paragraph's direction at its start and end
        if (before < 0) before = sos;
        if (after < 0) after = sos;
        runs[i].rtl = before == after ? before == 1 : l->rtl;
    }
    // Runs come in logical order; on the line they go in visual order. Glyphs within a run are already
    // left to right. A left-to-right line reverses each stretch of right-to-left runs; a right-to-left
    // line reverses the whole order, which puts left-to-right stretches back in reading order.
    int *order = malloc(sizeof *order * (size_t)(nruns ? nruns : 1));
    for (int i = 0; i < nruns; i++) order[i] = i;
    if (l->rtl)
        for (int i = 0; i < nruns / 2; i++) { int t = order[i]; order[i] = order[nruns - 1 - i], order[nruns - 1 - i] = t; }
    for (int i = 0; i < nruns;) {
        int j = i;
        bool flip = l->rtl ? !runs[order[i]].rtl : runs[order[i]].rtl;
        while (j < nruns && (l->rtl ? !runs[order[j]].rtl : runs[order[j]].rtl) == flip) j++;
        if (flip)
            for (int a = i, b = j - 1; a < b; a++, b--) { int t = order[a]; order[a] = order[b], order[b] = t; }
        i = j > i ? j : i + 1;
    }
    gs_glyph_pos *visual = malloc(sizeof *visual * (size_t)(l->count ? l->count : 1));
    float pen = 0;
    int k = 0;
    for (int i = 0; i < nruns; i++)
        for (int j = 0; j < runs[order[i]].count; j++) {
            gs_glyph_pos gp = l->glyphs[runs[order[i]].first + j];
            gp.x += pen;
            pen += gp.advance;
            visual[k++] = gp;
        }
    free(l->glyphs), free(order), free(runs);
    l->glyphs = visual;
    l->width = pen;
    return l;
}

void gs_line_free(gs_line *l) {
    if (l) free(l->glyphs), free(l);
}

const gs_line *gs_fontset_line(gs_fontset *fs, float px, const char *utf8) {
    int size4 = (int)lroundf(px * 4);
    uint32_t h = 2166136261u ^ (uint32_t)size4;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) h = (h ^ *p) * 16777619u;
    cached_line *c = &fs->cache[h % LINE_CACHE];
    if (c->line && c->size4 == size4 && !strcmp(c->text, utf8)) return c->line;
    free(c->text), gs_line_free(c->line);
    c->text = SDL_strdup(utf8), c->size4 = size4, c->line = gs_fontset_shape(fs, px, utf8);
    return c->line;
}

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
