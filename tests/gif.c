// gs_gif on tests/gifs (made by tests/gifs/make.py): the frames it composes match Pillow's, frame for
// frame, through disposal to the background and to the frame before, a transparent pixel and local
// colour tables; it plays at the file's delays and loops; a GIF of one frame is a still.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_gif.h"
#include "test.h"

static bool same_as_size(SDL_Renderer *r, gs_gif *g, const uint8_t *want, int W, int H) {  // the frame drawn, read back, against Pillow's
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetTextureBlendMode(gs_gif_frame(g), SDL_BLENDMODE_NONE);  // (its pixels as they are, transparent ones too)
    SDL_RenderTexture(r, gs_gif_frame(g), NULL, &(SDL_FRect){ 0, 0, (float)W, (float)H });
    SDL_SetTextureBlendMode(gs_gif_frame(g), SDL_BLENDMODE_BLEND);
    SDL_Surface *s = SDL_RenderReadPixels(r, &(SDL_Rect){ 0, 0, W, H }), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    bool same = c != NULL;
    for (int y = 0; c && y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = (const uint8_t *)c->pixels + y * c->pitch + x * 4, *q = want + (y * W + x) * 4;
            bool clear = q[3] == 0 && p[3] == 0;  // (a transparent pixel's colour does not matter)
            if (!clear && memcmp(p, q, 4)) {
                if (same) fprintf(stderr, "  at %d,%d: %d %d %d %d, Pillow %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3]);
                same = false;
            }
        }
    SDL_DestroySurface(c), SDL_DestroySurface(s);
    return same;
}
static bool same_as(SDL_Renderer *r, gs_gif *g, const uint8_t *want) { return same_as_size(r, g, want, 8, 8); }

// Waits until the next frame is due, up to `ms`, and moves on to it.
static void until_next(gs_gif *g, int ms) {
    for (Uint64 t = SDL_GetTicks(); (int)(SDL_GetTicks() - t) < ms && gs_gif_due_ms(g) > 0;) SDL_Delay(1);
    gs_gif_frame(g);
}

void test_gif(void) {
    size_t len = 0, still_len = 0, want_len = 0;
    void *anim = SDL_LoadFile("tests/gifs/anim.gif", &len), *still = SDL_LoadFile("tests/gifs/still.gif", &still_len);
    uint8_t *want = SDL_LoadFile("tests/gifs/anim.rgba", &want_len);
    CHECK(anim && still && want && want_len == 4 * 8 * 8 * 4);
    SDL_Surface *target = SDL_CreateSurface(160, 120, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = target ? SDL_CreateSoftwareRenderer(target) : NULL;
    CHECK(r);
    if (!anim || !still || !want || !r) return;

    CHECK(!gs_gif_open("not a gif at all", 16, r));
    gs_gif *s = gs_gif_open(still, still_len, r);
    int w = 0, h = 0;
    CHECK(s && !gs_gif_animated(s));
    gs_gif_size(s, &w, &h);
    CHECK(w == 5 && h == 3);
    gs_gif_play(s, true);
    CHECK(!gs_gif_playing(s) && gs_gif_due_ms(s) == -1);  // (a still does not play)
    gs_gif_free(s);

    gs_gif *g = gs_gif_open(anim, len, r);
    CHECK(g && gs_gif_animated(g));
    if (!g) return;
    CHECK(gs_gif_due_ms(g) == -1 && same_as(r, g, want));  // its first frame, before it plays
    gs_gif_play(g, true);
    CHECK(gs_gif_due_ms(g) > 40 && gs_gif_due_ms(g) <= 50);  // (the first frame's 50 ms)
    // Each frame in turn, as Pillow composes it, and after the last the first again.
    for (int i = 1; i <= 4; i++) {
        until_next(g, 200);
        CHECK(same_as(r, g, want + (i % 4) * 256));
        if (!same_as(r, g, want + (i % 4) * 256)) fprintf(stderr, "  frame %d\n", i % 4);
    }
    // Paused, it stays; rested, it is back on its first frame with its decoder let go.
    gs_gif_play(g, false);
    CHECK(gs_gif_due_ms(g) == -1);
    until_next(g, 200);
    CHECK(same_as(r, g, want));
    gs_gif_play(g, true), until_next(g, 200), until_next(g, 200);
    CHECK(same_as(r, g, want + 2 * 256));
    gs_gif_rest(g);
    CHECK(!gs_gif_playing(g) && same_as(r, g, want));
    gs_gif_free(g);

    // Interlaced frames of noise in 256 colours, which fill the decoder's dictionary: as Pillow reads them.
    size_t noise_len = 0, noise_want_len = 0;
    void *noise = SDL_LoadFile("tests/gifs/noise.gif", &noise_len);
    uint8_t *noise_want = SDL_LoadFile("tests/gifs/noise.rgba", &noise_want_len);
    g = noise && noise_want_len == 3 * 160 * 120 * 4 ? gs_gif_open(noise, noise_len, r) : NULL;
    CHECK(g);
    for (int i = 0; g && i < 3; i++) {
        if (i) gs_gif_play(g, true), until_next(g, 300), gs_gif_play(g, false);
        CHECK(same_as_size(r, g, noise_want + (size_t)i * 160 * 120 * 4, 160, 120));
    }
    gs_gif_free(g), SDL_free(noise), SDL_free(noise_want);

    // ffmpeg's frames: data that ends inside a sub-block, unchanged pixels left transparent.
    size_t moving_len = 0, moving_want_len = 0;
    void *moving = SDL_LoadFile("tests/gifs/moving.gif", &moving_len);
    uint8_t *moving_want = SDL_LoadFile("tests/gifs/moving.rgba", &moving_want_len);
    g = moving && moving_want_len == 5 * 64 * 48 * 4 ? gs_gif_open(moving, moving_len, r) : NULL;
    CHECK(g);
    for (int i = 0; g && i < 5; i++) {
        if (i) gs_gif_play(g, true), until_next(g, 300), gs_gif_play(g, false);
        CHECK(same_as_size(r, g, moving_want + (size_t)i * 64 * 48 * 4, 64, 48));
    }
    gs_gif_free(g), SDL_free(moving), SDL_free(moving_want);
    SDL_DestroyRenderer(r), SDL_DestroySurface(target);
    SDL_free(anim), SDL_free(still), SDL_free(want);
}
