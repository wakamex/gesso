// Fonts and glyph drawing on an SDL renderer, at the screen's full resolution.
// gs_font wraps one TrueType/OpenType file (stb_truetype); gs_glyphs caches rasterized glyphs in
// one atlas texture per renderer and draws them with a position, angle, colour and optional
// left-to-right reveal. Text layout above single lines (wrapping, per-glyph variation) is the
// caller's: a game can vary every glyph, an app can use gs_text_draw as is.
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

typedef struct gs_font gs_font;
typedef struct gs_glyphs gs_glyphs;

gs_font *gs_font_load(const char *path);
void gs_font_free(gs_font *f);
int gs_font_glyph(const gs_font *f, uint32_t codepoint);  // 0 when the font lacks it
int gs_font_glyph_count(const gs_font *f);
// Metrics in pixels for a font drawn with an em of `px` pixels.
float gs_font_advance(const gs_font *f, int glyph, float px);
float gs_font_kern(const gs_font *f, int glyph, int next, float px);
void gs_font_vmetrics(const gs_font *f, float px, float *ascent, float *descent, float *line_gap);

gs_glyphs *gs_glyphs_new(SDL_Renderer *r, int atlas_size);
void gs_glyphs_free(gs_glyphs *g);
// Uploads the glyphs first drawn in the last frame; call it each frame before drawing anything.
// Once it has been called, a glyph new to the atlas is drawn from the next frame on, so no frame
// uploads to the atlas between its draws: on NVIDIA's Vulkan driver, SDL 3.4's Vulkan renderer loses
// what a frame drew before such an upload. Without it, glyphs are uploaded as they are first drawn.
void gs_glyphs_begin_frame(gs_glyphs *g);

// Draws one glyph with its origin (pen position on the baseline) at (x, y), rotated by `angle`
// degrees about that origin. `reveal` in [0, 1] shows only the left part of the glyph.
void gs_glyphs_draw(gs_glyphs *g, const gs_font *f, int glyph, float px, float x, float y, float angle,
                    SDL_FColor colour, float reveal);

// Plain UTF-8 text on one line, kerned, from the pen position (x, y). Returns the advance.
float gs_text_draw(gs_glyphs *g, const gs_font *f, float px, float x, float y, const char *utf8, SDL_FColor colour);
float gs_text_width(const gs_font *f, float px, const char *utf8);

// Next code point from UTF-8, advancing *s; returns 0 at the end.
uint32_t gs_utf8_next(const char **s);

// A chain of fonts tried in order for each character: a main font, then fallbacks for other scripts.
// Fallbacks load on first need, so large CJK fonts cost memory only once such text appears. Scripts
// that need shaping (Arabic, the Indic scripts) come out as separate, unjoined letters.
typedef struct gs_fontset gs_fontset;
gs_fontset *gs_fontset_new(void);
gs_fontset *gs_fontset_system(void);  // the platform's UI font with fallbacks for most scripts
void gs_fontset_add(gs_fontset *fs, const char *path);  // missing files are skipped
void gs_fontset_free(gs_fontset *fs);
float gs_fontset_draw(gs_glyphs *g, gs_fontset *fs, float px, float x, float y, const char *utf8, SDL_FColor colour);
float gs_fontset_width(gs_fontset *fs, float px, const char *utf8);
