// Renders a test page of mixed scripts and emoji with the system's fonts, headless, to a PNG:
//   zig build bench-text -- OUT.png
// For checking shaping, fallback and colour emoji on each system by eye.
//   zig build bench-text -- OUT.png FONT.ttf [GAMMA]
// renders one font from 12 to 16 pixels, light on dark and dark on light, for judging the weight and
// smoothness of small text; GAMMA is gs_glyphs_correction's (1: coverage as it is).
#include <SDL3/SDL.h>
#include <stdio.h>

#include "gs_draw.h"
#include "gs_text.h"

static const char *const lines[] = {
    "Latin: Office ffi fl \xE2\x80\x94 Caf\xC3\xA9 na\xC3\xAFve, Twitch titles",                   // ligatures, accents
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE9\x85\x8D\xE4\xBF\xA1 \xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xE4\xB8\xAD\xE6\x96\x87\xE7\x9B\xB4\xE6\x92\xAD",  // 日本語の配信 한국어 中文直播
    "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85 Arabic \xD8\xA8\xD8\xAB \xD9\x85\xD8\xA8\xD8\xA7\xD8\xB4\xD8\xB1",  // السلام عليكم Arabic بث مباشر
    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 \xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\x9F\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xAE \xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7",  // हिन्दी स्ट्रीम क्ष
    "Emoji \xF0\x9F\x98\x80 \xF0\x9F\x94\xA5 \xF0\x9F\x8E\xAE \xE2\x9D\xA4\xEF\xB8\x8F \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 \xF0\x9F\x87\xA8\xF0\x9F\x87\xA6",  // 😀 🔥 🎮 ❤️ 👍🏽 👨‍👩‍👧 🇨🇦
};

// One font at the sizes of an interface's text, on a dark half and a light half.
static int sample(int argc, char **argv) {
    static const char *const text[] = { "Conversation titles and message previews, 0123456789", "The quick brown fox jumps over the lazy dog: (il1 O0 rn m)" };
    SDL_Window *w = SDL_CreateWindow("bench-text", 900, 330, 0);
    SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
    gs_glyphs *g = gs_glyphs_new(r, 1024), *on_light = gs_glyphs_new(r, 1024);  // (one for each background)
    if (argc > 3) gs_glyphs_correction(g, (float)SDL_atof(argv[3])), gs_glyphs_correction(on_light, (float)SDL_atof(argv[3]));
    gs_fontset *fs = gs_fontset_new();
    gs_fontset_add(fs, argv[2]);
    const SDL_FColor dark = { 0x1e / 255.f, 0x1e / 255.f, 0x1e / 255.f, 1 }, white = { 1, 1, 1, 1 };
    gs_glyphs_behind(g, &dark), gs_glyphs_behind(on_light, &white);
    for (int frame = 0; frame < 2; frame++) {
        gs_glyphs_begin_frame(g), gs_glyphs_begin_frame(on_light);
        SDL_SetRenderDrawColor(r, 0x1e, 0x1e, 0x1e, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderFillRect(r, &(SDL_FRect){ 450, 0, 450, 330 });
        float y = 24;
        for (int px = 12; px <= 16; px++)
            for (int i = 0; i < 2; i++, y += (float)px + 8) {
                char line[160];
                SDL_snprintf(line, sizeof line, "%d  %s", px, text[i]);
                SDL_SetRenderClipRect(r, &(SDL_Rect){ 0, 0, 446, 330 });
                gs_fontset_draw(g, fs, (float)px, 10, y, line, white);
                SDL_SetRenderClipRect(r, &(SDL_Rect){ 450, 0, 450, 330 });
                gs_fontset_draw(on_light, fs, (float)px, 460, y, line, dark);
                SDL_SetRenderClipRect(r, NULL);
            }
    }
    bool ok = gs_draw_save_png(r, argv[1]);
    gs_fontset_free(fs), gs_glyphs_free(g), gs_glyphs_free(on_light);
    SDL_Quit();
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-text OUT.png [FONT.ttf [GAMMA]]\n"), 1;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    if (argc > 2) return sample(argc, argv);
    SDL_Window *w = SDL_CreateWindow("bench-text", 900, 60 + 56 * (int)SDL_arraysize(lines), 0);
    SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
    gs_glyphs *g = gs_glyphs_new(r, 2048);
    gs_fontset *fs = gs_fontset_system();
    uint64_t start = SDL_GetTicksNS();
    for (int frame = 0; frame < 3; frame++) {  // (a glyph first drawn in one frame appears in the next)
        gs_glyphs_begin_frame(g);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderClear(r);
        for (size_t i = 0; i < SDL_arraysize(lines); i++)
            gs_fontset_draw(g, fs, 30, 20, 60 + 56 * (float)i, lines[i], (SDL_FColor){ 0.1f, 0.1f, 0.1f, 1 });
        if (frame == 0) printf("first frame, shaping included: %.2f ms\n", (SDL_GetTicksNS() - start) / 1e6);
    }
    bool ok = gs_draw_save_png(r, argv[1]);
    gs_fontset_free(fs), gs_glyphs_free(g);
    SDL_Quit();
    return ok ? 0 : 1;
}
