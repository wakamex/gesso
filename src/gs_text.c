#include "gs_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

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
    size_t len;  // of the mapping, or 0 for a file read into memory (the web build)
};

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
    *len = 0;
    return SDL_LoadFile(path, NULL);
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
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        font_unmap(data, len);
        free(f);
        return NULL;
    }
    return f;
}

void gs_font_free(gs_font *f) {
    if (!f) return;
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
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&f->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
            int w = x1 - x0 + 2, hh = y1 - y0 + 2;  // one clear pixel each side for linear filtering
            if (w > g->size || hh > g->size) return NULL;
            if (g->shelf_x + w > g->size) g->shelf_x = 0, g->shelf_y += g->shelf_h, g->shelf_h = 0;
            if (g->shelf_y + hh > g->size) break;
            unsigned char *cov = calloc((size_t)w * hh, 1);
            stbtt_MakeGlyphBitmap(&f->info, cov + w + 1, w - 2, hh - 2, w, scale, scale, glyph);
            uint32_t *px = malloc((size_t)w * hh * 4);
            for (int k = 0; k < w * hh; k++) px[k] = SDL_Swap32LE(0x00FFFFFFu | (uint32_t)cov[k] << 24);
            SDL_Rect dst = { g->shelf_x, g->shelf_y, w, hh };
            free(cov);
            if (g->deferred) {
                if (g->nuploads == g->uploads_cap)
                    g->uploads = realloc(g->uploads, sizeof *g->uploads * (size_t)(g->uploads_cap = g->uploads_cap ? g->uploads_cap * 2 : 64));
                g->uploads[g->nuploads++] = (upload){ dst, px };
            } else {
                SDL_UpdateTexture(g->tex, &dst, px, w * 4);
                free(px);
            }
            *s = (slot){ f, glyph, size4, g->shelf_x, g->shelf_y, w, hh, x0 - 1, y0 - 1, g->deferred };
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
    SDL_SetTextureColorModFloat(g->tex, colour.r, colour.g, colour.b);
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

// ---- Font chains ----

#define MAX_FONTS 12

struct gs_fontset {
    char *path[MAX_FONTS];
    gs_font *font[MAX_FONTS];
    bool tried[MAX_FONTS];
    int n;
};

gs_fontset *gs_fontset_new(void) { return calloc(1, sizeof(gs_fontset)); }

void gs_fontset_add(gs_fontset *fs, const char *path) {
    SDL_PathInfo info;
    if (fs->n == MAX_FONTS || !SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return;
    fs->path[fs->n++] = SDL_strdup(path);
}

gs_fontset *gs_fontset_system(void) {
    gs_fontset *fs = gs_fontset_new();
#if defined(_WIN32)
    const char *root = SDL_getenv("SystemRoot");
    if (!root) root = SDL_getenv("SYSTEMROOT");
    static const char *const names[] = { "segoeui.ttf", "seguisym.ttf", "YuGothM.ttc", "msyh.ttc", "malgun.ttf", "Nirmala.ttc", "Nirmala.ttf", "tahoma.ttf", "arial.ttf" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char path[512];
        SDL_snprintf(path, sizeof path, "%s\\Fonts\\%s", root ? root : "C:\\Windows", names[i]);
        gs_fontset_add(fs, path);
    }
#elif defined(__APPLE__)
    static const char *const paths[] = { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                         "/System/Library/Fonts/AppleSDGothicNeo.ttc", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
#else
    static const char *const paths[] = {
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "/usr/share/fonts/google-droid-sans-fonts/DroidKufi-Regular.ttf",
        "/usr/share/fonts/google-droid-sans-fonts/DroidSansDevanagari-Regular.ttf", "/usr/share/fonts/adobe-source-code-pro-fonts/SourceCodePro-Regular.otf" };
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) gs_fontset_add(fs, paths[i]);
#endif
    return fs;
}

void gs_fontset_free(gs_fontset *fs) {
    if (!fs) return;
    for (int i = 0; i < fs->n; i++) SDL_free(fs->path[i]), gs_font_free(fs->font[i]);
    free(fs);
}

static gs_font *font_at(gs_fontset *fs, int i) {
    if (!fs->tried[i]) fs->tried[i] = true, fs->font[i] = gs_font_load(fs->path[i]);
    return fs->font[i];
}

// The first font with a glyph for cp (loading fallbacks as needed), or the main font's missing glyph.
static gs_font *pick(gs_fontset *fs, uint32_t cp, int *glyph) {
    for (int i = 0; i < fs->n; i++) {
        gs_font *f = font_at(fs, i);
        if (f && (*glyph = gs_font_glyph(f, cp))) return f;
    }
    for (int i = 0; i < fs->n; i++)
        if (fs->font[i]) return *glyph = 0, fs->font[i];
    return NULL;
}

static float fontset_run(gs_glyphs *g, gs_fontset *fs, float px, float x, float y, const char *utf8, SDL_FColor colour) {
    float pen = x;
    gs_font *prev_font = NULL;
    int prev = 0;
    for (uint32_t cp; (cp = gs_utf8_next(&utf8));) {
        int gl;
        gs_font *f = pick(fs, cp, &gl);
        if (!f) break;
        if (prev && f == prev_font) pen += gs_font_kern(f, prev, gl, px);
        if (g) gs_glyphs_draw(g, f, gl, px, pen, y, 0, colour, 1);
        pen += gs_font_advance(f, gl, px);
        prev = gl, prev_font = f;
    }
    return pen - x;
}

float gs_fontset_draw(gs_glyphs *g, gs_fontset *fs, float px, float x, float y, const char *utf8, SDL_FColor colour) {
    return fontset_run(g, fs, px, x, y, utf8, colour);
}

float gs_fontset_width(gs_fontset *fs, float px, const char *utf8) { return fontset_run(NULL, fs, px, 0, 0, utf8, (SDL_FColor){ 0 }); }
