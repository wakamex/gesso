// Fonts and glyph drawing on an SDL renderer, at the screen's full resolution.
// gs_font wraps one TrueType/OpenType file (stb_truetype); gs_glyphs caches rasterized glyphs in
// one atlas texture per renderer and draws them with a position, angle, colour and optional
// left-to-right reveal. Text layout above lines (per-glyph variation, say) is the
// caller's: a game can vary every glyph, an app can use gs_text_draw as is, and gs_fontset_wrap breaks text into lines.
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

// True when a glyph drawn since the last gs_glyphs_begin_frame was left out because its upload waits
// for the next one; the frame is missing text until another is drawn.
bool gs_glyphs_waiting(const gs_glyphs *g);

// How glyphs' edges are blended. The renderer blends in the display's encoding rather than in light,
// which thins light text on dark: text lighter than what is behind it has its coverage corrected for
// that, by its lightness, so that it has the weight of its outline, as it has in a browser. `gamma`
// is the display's, 2.2 by default; 1 blends coverage as it is. Changing it empties the atlas.
void gs_glyphs_correction(gs_glyphs *g, float gamma);
// What the text is mostly drawn over (an app's background colour), which the correction depends on.
// NULL, the default, for unknown: the text is then taken to stand on its opposite, light text on dark.
// gs_ui gives its style's background each frame. Changing it empties the atlas.
void gs_glyphs_behind(gs_glyphs *g, const SDL_FColor *background);

// Draws one glyph with its origin (pen position on the baseline) at (x, y), rotated by `angle`
// degrees about that origin. `reveal` in [0, 1] shows only the left part of the glyph.
void gs_glyphs_draw(gs_glyphs *g, const gs_font *f, int glyph, float px, float x, float y, float angle,
                    SDL_FColor colour, float reveal);

// Plain UTF-8 text on one line, kerned, from the pen position (x, y). Returns the advance.
float gs_text_draw(gs_glyphs *g, const gs_font *f, float px, float x, float y, const char *utf8, SDL_FColor colour);
float gs_text_width(const gs_font *f, float px, const char *utf8);

// Next code point from UTF-8, advancing *s; returns 0 at the end.
uint32_t gs_utf8_next(const char **s);

// A chain of fonts tried in order for each character: a main font, then fallbacks for other scripts
// and for emoji. Fallbacks load on first need, so large CJK fonts cost memory only once such text
// appears. Text is shaped with kb_text_shape: Arabic letters join, Indic scripts reorder and combine,
// ligatures and kerning apply, and a line mixing right-to-left and left-to-right text is laid out in
// visual order by the Unicode Bidirectional Algorithm (numbers, bracket pairs and punctuation placed
// as a browser places them; explicit embedding and isolate controls are not acted on). Lines drawn each frame are shaped once and kept.
typedef struct gs_fontset gs_fontset;
gs_fontset *gs_fontset_new(void);
gs_fontset *gs_fontset_system(void);  // the platform's UI font with fallbacks for most scripts and emoji
// The same at a weight: 400 regular, 500 medium, 600 semibold, 700 bold. macOS's San Francisco and
// the Noto fonts Fedora installs are variable fonts and are drawn at exactly the weight asked for,
// fallback scripts included. Windows has Segoe UI as files, regular, semibold and bold, so 500 is
// drawn regular; a Linux system with Noto as separate files gets the file of that weight where it is
// installed, and otherwise DejaVu's bold for 600 and up and the regular face below that.
gs_fontset *gs_fontset_system_weight(int weight);
// The system's monospace face first, then the same fallbacks: SF Mono, Consolas (regular, and bold
// for 600 and up), Noto Sans Mono or the first of Adwaita Mono, Liberation Mono and DejaVu Sans Mono.
// Its digits are all one width, so a running timer does not shift.
gs_fontset *gs_fontset_system_mono(int weight);
// Sets the weight a set's variable fonts are drawn at (those with a weight axis and TrueType
// outlines), for an app's own fonts; other fonts are unchanged. Call it before the set is first used.
void gs_fontset_weight(gs_fontset *fs, int weight);
// One font's weight, along its weight axis: false for a font without one. Before it is first drawn.
bool gs_font_set_weight(gs_font *f, int weight);
void gs_fontset_add(gs_fontset *fs, const char *path);  // missing files are skipped
// The same, but the font is tried before the others: an app's own emoji or text font, shipped beside
// it, takes its characters ahead of the system's (Windows' emoji font draws a flag as two letters and
// a family as separate people, which a font with those sequences as glyphs does not).
void gs_fontset_prepend(gs_fontset *fs, const char *path);
void gs_fontset_free(gs_fontset *fs);
// The vertical metrics, as gs_font_vmetrics gives them (the descent is below zero), of the set's
// first text font: the one Latin text is drawn with, which for a system set the app does not know
// the file of. False when the set has no text font that loads.
bool gs_fontset_vmetrics(gs_fontset *fs, float px, float *ascent, float *descent, float *line_gap);
float gs_fontset_draw(gs_glyphs *g, gs_fontset *fs, float px, float x, float y, const char *utf8, SDL_FColor colour);
float gs_fontset_width(gs_fontset *fs, float px, const char *utf8);

// A shaped line: glyphs in visual order, each placed relative to the line's start on the baseline,
// with the byte offset in the text of the character cluster it came from.
typedef struct {
    const gs_font *font;
    int glyph;
    float x, y, advance;
    int cluster;
    bool rtl;  // from a right-to-left run
} gs_glyph_pos;

typedef struct {
    gs_glyph_pos *glyphs;
    int count;
    float width, px;
    bool rtl;  // the line's own direction (from its first strong character)
} gs_line;

gs_line *gs_fontset_shape(gs_fontset *fs, float px, const char *utf8);  // free with gs_line_free
// The same with the line's direction given instead of read from its first strong character, for a
// line that is part of a longer paragraph whose direction it takes (GS_DIR_AUTO is the first).
enum { GS_DIR_AUTO, GS_DIR_LTR, GS_DIR_RTL };
gs_line *gs_fontset_shape_dir(gs_fontset *fs, float px, const char *utf8, int dir);
const gs_line *gs_fontset_line_dir(gs_fontset *fs, float px, const char *utf8, int dir);
// One line of a longer text: the bytes [start, end) of utf8, ordered as part of the paragraph they are
// in, since what a line's last comma or its first bracket does depends on the text beyond the line.
// Draw a wrapped text's lines with these. Clusters count from `start`.
gs_line *gs_fontset_shape_part(gs_fontset *fs, float px, const char *utf8, int start, int end, int dir);
const gs_line *gs_fontset_line_part(gs_fontset *fs, float px, const char *utf8, int start, int end, int dir);  // the kept copy
void gs_line_free(gs_line *l);
// The fontset's kept copy: valid until the next gs_fontset call.
const gs_line *gs_fontset_line(gs_fontset *fs, float px, const char *utf8);
float gs_line_draw(gs_glyphs *g, const gs_line *l, float x, float y, SDL_FColor colour);
// Where a text cursor goes for a byte offset into the line's text, from the line's start.
float gs_line_caret(const gs_line *l, int offset);
// The byte offset of the cursor position nearest to x.
int gs_line_hit(const gs_line *l, float x, const char *utf8);

// The next or previous grapheme boundary from a byte offset: the steps a text cursor takes, so an
// emoji sequence or a letter with its marks moves and deletes as one.
int gs_utf8_grapheme_next(const char *utf8, int offset);
int gs_utf8_grapheme_prev(const char *utf8, int offset);

// Text wrapped to a width, in the units of `px` (pixels, for a line to be drawn). A line ends where
// Unicode allows one to break (after a space or hyphen, between CJK characters), at a newline, or
// inside a word too wide for a line. Each line is the byte range [start, end) of the text it shows,
// without the spaces that hang at a soft break or the newline that ends it, and the width of that
// range from the shaped glyphs' advances (a line shaped alone is the same to within kerning). A
// line's start is where the previous one's hanging spaces end, so every byte of the text is in a
// line or in whitespace between two. There is always one line, an empty text's included, and one
// after a final newline.
typedef struct { int start, end; float width; bool rtl; } gs_wrap_line;  // rtl: its paragraph's direction, the same for all its lines
typedef struct { gs_wrap_line *lines; int count; bool cut; } gs_wrap;  // cut: lines were left out (gs_fontset_wrap_lines)
gs_wrap *gs_fontset_wrap(gs_fontset *fs, float px, const char *utf8, float width);  // free with gs_wrap_free
// The same held to `max_lines` lines. When the text has more, `cut` is set and the last line kept
// ends early enough for an ellipsis to follow it within the width: draw that line's range and then
// "…". Its width counts the ellipsis.
gs_wrap *gs_fontset_wrap_lines(gs_fontset *fs, float px, const char *utf8, float width, int max_lines);
void gs_wrap_free(gs_wrap *w);
