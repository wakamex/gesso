#include "gs_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

struct gs_font {
    stbtt_fontinfo info;
    void *data;
};

gs_font *gs_font_load(const char *path) {
    size_t len;
    void *data = SDL_LoadFile(path, &len);
    if (!data) return NULL;
    gs_font *f = calloc(1, sizeof *f);
    f->data = data;
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        SDL_free(data);
        free(f);
        return NULL;
    }
    return f;
}

void gs_font_free(gs_font *f) {
    if (!f) return;
    SDL_free(f->data);
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
} slot;

#define SLOTS 4096  // open addressing; the atlas is cleared well before this fills

struct gs_glyphs {
    SDL_Renderer *r;
    SDL_Texture *tex;
    int size, shelf_x, shelf_y, shelf_h, used;
    slot slots[SLOTS];
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

void gs_glyphs_free(gs_glyphs *g) {
    if (!g) return;
    SDL_DestroyTexture(g->tex);
    free(g);
}

static void clear(gs_glyphs *g) {
    memset(g->slots, 0, sizeof g->slots);
    g->shelf_x = g->shelf_y = g->shelf_h = g->used = 0;
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
            SDL_UpdateTexture(g->tex, &dst, px, w * 4);
            free(px);
            free(cov);
            *s = (slot){ f, glyph, size4, g->shelf_x, g->shelf_y, w, hh, x0 - 1, y0 - 1 };
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
    if (!s || s->w <= 2) return;
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
