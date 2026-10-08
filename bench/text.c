// Renders a test page of mixed scripts and emoji with the system's fonts, headless, to a PNG:
//   zig build bench-text -- OUT.png
// For checking shaping, fallback and colour emoji on each system by eye.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "gs_text.h"
#include "stb_image_write.h"

static const char *const lines[] = {
    "Latin: Office ffi fl \xE2\x80\x94 Caf\xC3\xA9 na\xC3\xAFve, Twitch titles",                   // ligatures, accents
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE9\x85\x8D\xE4\xBF\xA1 \xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xE4\xB8\xAD\xE6\x96\x87\xE7\x9B\xB4\xE6\x92\xAD",  // 日本語の配信 한국어 中文直播
    "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85 Arabic \xD8\xA8\xD8\xAB \xD9\x85\xD8\xA8\xD8\xA7\xD8\xB4\xD8\xB1",  // السلام عليكم Arabic بث مباشر
    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80 \xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\x9F\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xAE \xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7",  // हिन्दी स्ट्रीम क्ष
    "Emoji \xF0\x9F\x98\x80 \xF0\x9F\x94\xA5 \xF0\x9F\x8E\xAE \xE2\x9D\xA4\xEF\xB8\x8F \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 \xF0\x9F\x87\xA8\xF0\x9F\x87\xA6",  // 😀 🔥 🎮 ❤️ 👍🏽 👨‍👩‍👧 🇨🇦
};

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-text OUT.png\n"), 1;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
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
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    bool ok = c && stbi_write_png(argv[1], c->w, c->h, 4, c->pixels, c->pitch);
    SDL_DestroySurface(c), SDL_DestroySurface(s);
    gs_fontset_free(fs), gs_glyphs_free(g);
    SDL_Quit();
    return ok ? 0 : 1;
}
