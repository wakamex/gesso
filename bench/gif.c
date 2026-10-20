// Opens a GIF and decodes every frame once, timing it:
//   zig build bench-gif -- FILE.gif
// For a large file, what opening costs (the first frame) and what each frame costs as it plays.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "gs_gif.h"

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-gif FILE.gif\n"), 2;
    size_t len = 0;
    void *data = SDL_LoadFile(argv[1], &len);
    SDL_Surface *target = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = target ? SDL_CreateSoftwareRenderer(target) : NULL;
    if (!data || !r) return fprintf(stderr, "bench-gif: %s\n", SDL_GetError()), 1;
    Uint64 t0 = SDL_GetTicksNS();
    gs_gif *g = gs_gif_open(data, len, r);
    Uint64 t1 = SDL_GetTicksNS();
    if (!g) return fprintf(stderr, "bench-gif: not a GIF that can be read\n"), 1;
    int w, h, frames = 1;
    gs_gif_size(g, &w, &h);
    printf("%zu bytes, %d by %d, opened with its first frame in %.1f ms\n", len, w, h, (t1 - t0) / 1e6);
    // Played as fast as it goes: each frame is made due at once, until it comes round to the first.
    gs_gif_play(g, true);
    Uint64 start = SDL_GetTicksNS(), slowest = 0;
    SDL_Texture *first = gs_gif_frame(g);
    for (;; frames++) {
        while (gs_gif_due_ms(g) > 0) SDL_Delay(0), SDL_DelayNS(gs_gif_due_ms(g) > 2 ? 1000000 : 100000);
        Uint64 a = SDL_GetTicksNS();
        SDL_Texture *t = gs_gif_frame(g);
        Uint64 b = SDL_GetTicksNS();
        if (b - a > slowest) slowest = b - a;
        if (t == first || frames > 100000) break;
    }
    printf("%d frames played through in %.1f s (their delays), decoding at most %.1f ms a frame\n", frames, (SDL_GetTicksNS() - start) / 1e9, slowest / 1e6);
    gs_gif_free(g);
    SDL_DestroyRenderer(r), SDL_DestroySurface(target), SDL_free(data);
    return 0;
}
