// gs_movie on tests/movies (made by its make.sh): two seconds each, red for the first second and
// blue for the second, so the colour seen says where in the movie playing is.
#include <SDL3/SDL.h>

#include "test.h"

#ifdef GS_TEST_VIDEO
#include "gs_movie.h"

extern const char *test_streams;

static SDL_Renderer *r;

// Draws the movie's frame for this moment and returns 'r' or 'b' for what is seen in the middle of
// it, or 0 when there is nothing to draw yet.
static char seen(gs_movie *m) {
    SDL_SetRenderDrawColor(r, 0, 255, 0, 255);
    SDL_RenderClear(r);
    if (!gs_movie_draw(m, (SDL_FRect){ 0, 0, 320, 320 })) return 0;
    SDL_Surface *s = SDL_RenderReadPixels(r, &(SDL_Rect){ 160, 160, 1, 1 });
    Uint8 red = 0, green = 0, blue = 0;
    if (s) SDL_ReadSurfacePixel(s, 0, 0, &red, &green, &blue, NULL);
    SDL_DestroySurface(s);
    return red > 150 && blue < 100 ? 'r' : blue > 150 && red < 100 ? 'b' : '?';
}

// Keeps drawing until `want` is seen, for at most `seconds`.
static bool until(gs_movie *m, char want, double seconds) {
    for (Uint64 start = SDL_GetTicks(); SDL_GetTicks() - start < (Uint64)(seconds * 1000); SDL_Delay(10))
        if (seen(m) == want) return true;
    return false;
}

static void *load(const char *name, size_t *len) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/../movies/%s", test_streams, name);
    return SDL_LoadFile(path, len);
}

void test_movie(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return;  // (no video at all: nothing to test here)
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 320);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 320);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    SDL_Window *w = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    r = w ? SDL_CreateRenderer(w, NULL) : NULL;
    CHECK(r != NULL);
    size_t len = 0, turned_len = 0;
    void *clip = load("clip.mp4", &len), *turned = load("turned.mp4", &turned_len);
    CHECK(clip && turned);
    if (!r || !clip || !turned) { SDL_free(clip), SDL_free(turned); SDL_Quit(); return; }
    CHECK(gs_movie_open("not a movie", 11, r, 48000) == NULL);
    CHECK(gs_movie_open(clip, len / 3, r, 48000) == NULL);  // (cut short: its sample table is at its end)

    // Opened, it knows its size and length, and shows its first frame without being played.
    gs_movie *m = gs_movie_open(clip, len, r, 48000);
    CHECK(m != NULL);
    if (m) {
        int width = 0, height = 0;
        gs_movie_size(m, &width, &height);
        CHECK(width == 320 && height == 240);
        CHECK_NEAR(gs_movie_length(m), 2.0, 0.04);
        CHECK(gs_movie_has_sound(m) && !gs_movie_playing(m) && !gs_movie_ended(m));
        CHECK(gs_movie_waiting(m) && !SDL_strcmp(gs_movie_get_info(m).path, "none"));
        CHECK(until(m, 'r', 3));
        CHECK(gs_movie_position(m) == 0);
        // Played, it reaches the blue second about a second in; paused, it stays where it is.
        gs_movie_play(m, true);
        CHECK(gs_movie_playing(m));
        CHECK(seen(m) == 'r');
        CHECK(until(m, 'b', 1.6));
        CHECK(gs_movie_position(m) > 0.9 && gs_movie_position(m) < 1.5);
        gs_movie_play(m, false);
        double at = gs_movie_position(m);
        SDL_Delay(100);
        CHECK(seen(m) == 'b' && gs_movie_position(m) == at);
        // A seek back shows the red second again, and playing goes on from there to the end.
        gs_movie_seek(m, 0.4);
        CHECK(until(m, 'r', 2));
        CHECK_NEAR(gs_movie_position(m), 0.4, 0.001);
        // Paused, a seek to a place between key frames is reached by drawing until it stops waiting.
        gs_movie_seek(m, 0.9);
        CHECK(gs_movie_waiting(m));
        for (Uint64 start = SDL_GetTicks(); gs_movie_waiting(m) && SDL_GetTicks() - start < 3000; SDL_Delay(5)) seen(m);
        CHECK(!gs_movie_waiting(m) && seen(m) == 'r');
        CHECK_NEAR(gs_movie_get_info(m).shown_pts, 0.9, 0.05);
        CHECK(!SDL_strcmp(gs_movie_get_info(m).path, "software"));
        gs_movie_seek(m, 1.5);
        CHECK(until(m, 'b', 2));
        gs_movie_play(m, true);
        for (Uint64 start = SDL_GetTicks(); !gs_movie_ended(m) && SDL_GetTicks() - start < 1500; SDL_Delay(10)) seen(m);
        // At its end it rests on its first frame, and plays from the start when played again.
        CHECK(gs_movie_ended(m) && !gs_movie_playing(m) && gs_movie_position(m) == 0);
        CHECK(seen(m) == 'r');
        gs_movie_play(m, true);
        CHECK(!gs_movie_ended(m) && gs_movie_position(m) < 0.2);
        // Looping, it is red again after its length has passed.
        gs_movie_set_loop(m, true), gs_movie_set_muted(m, true);
        gs_movie_seek(m, 1.6);
        CHECK(until(m, 'b', 2));
        CHECK(until(m, 'r', 1.5));
        // (it went round: its clock is back below where it was sent; on a slow machine the clock may
        // already be past the red second by the time a red frame is seen, so the bound is the seek's)
        CHECK(gs_movie_playing(m) && gs_movie_position(m) < 1.6);
        gs_movie_rest(m);
        CHECK(!gs_movie_playing(m) && gs_movie_position(m) == 0 && seen(m) == 'r');
        // The picture follows the sound: with 1.2 s of its sound taken by the mixer at once, it is in
        // the blue second.
        gs_movie_set_loop(m, false), gs_movie_set_muted(m, false);
        gs_movie_play(m, true);
        static float lr[2 * 4800];
        for (int i = 0; i < 12; i++) SDL_memset(lr, 0, sizeof lr), gs_movie_render(m, lr, 4800);
        seen(m);
        CHECK(gs_movie_position(m) > 1.1 && gs_movie_position(m) < 1.5);
        CHECK(until(m, 'b', 1));
        gs_movie_free(m);
    }

    // A recording marked as turned is as tall as it is stored wide, and is drawn turned.
    m = gs_movie_open(turned, turned_len, r, 48000);
    CHECK(m != NULL);
    if (m) {
        int width = 0, height = 0, turns = -1;
        gs_movie_size(m, &width, &height);
        CHECK(width == 240 && height == 320 && !gs_movie_has_sound(m));
        CHECK(until(m, 'r', 3));
        SDL_FRect src;
        CHECK(gs_movie_frame(m, &src, &turns) != NULL && turns == 1);
        gs_movie_free(m);
    }
    SDL_free(clip), SDL_free(turned);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
#else
void test_movie(void) {}
#endif
