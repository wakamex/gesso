// Plays a movie file through gs_movie in a window, through a fixed course, and prints at each step
// which way it is decoded, the decoder's counts, and the colour in the middle of the picture:
//   zig build bench-movie -Dvideo -- FILE [software]
// The course: the still, a second of playing, a pause, a seek to three quarters while paused,
// playing to the end, playing again, and rest. For checking hardware decoding on a system; with
// tests/movies/clip.mp4 the colours are red for the first second and blue for the second.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "gs_movie.h"

static SDL_Renderer *r;
static gs_movie *m;
static bool drawn;

static void frame(void) {
    int w, h, mw, mh;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    gs_movie_size(m, &mw, &mh);
    float scale = SDL_min((float)w / (float)mw, (float)h / (float)mh);
    SDL_SetRenderDrawColor(r, 0, 80, 0, 255);
    SDL_RenderClear(r);
    drawn = gs_movie_draw(m, (SDL_FRect){ ((float)w - (float)mw * scale) / 2, ((float)h - (float)mh * scale) / 2, (float)mw * scale, (float)mh * scale });
}

static void report(const char *step) {
    int w, h;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    frame();
    Uint8 red = 0, green = 0, blue = 0;
    SDL_Surface *s = SDL_RenderReadPixels(r, &(SDL_Rect){ w / 2, h / 2, 1, 1 });
    if (s) SDL_ReadSurfacePixel(s, 0, 0, &red, &green, &blue, NULL);
    SDL_DestroySurface(s);
    gs_video_info i = gs_movie_get_info(m);
    printf("%-34s at %5.2f s  %s  path %-12s decoded %4lld shown %4lld dropped %3lld  showing %5.2f s  middle %3d %3d %3d\n", step, gs_movie_position(m),
           !drawn ? "nothing drawn" : gs_movie_playing(m) ? "playing" : gs_movie_ended(m) ? "ended  " : "paused ", i.path, i.decoded, i.shown, i.dropped, i.shown_pts, red, green, blue);
    fflush(stdout);
    SDL_RenderPresent(r);
}

// Draws frames for `seconds`, or until `done` says so; false if the time ran out first.
static bool run(double seconds, bool (*done)(void)) {
    for (Uint64 start = SDL_GetTicksNS(); (double)(SDL_GetTicksNS() - start) / 1e9 < seconds;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) return false;
        frame();
        SDL_RenderPresent(r);
        if (done && done()) return true;
        SDL_Delay(4);
    }
    return !done;
}

static bool have_still(void) { return drawn; }
static bool settled(void) { return !gs_movie_waiting(m); }
static bool ended(void) { return gs_movie_ended(m); }

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-movie FILE [software]\n"), 1;
    const char *name = gs_video_prepare(!(argc > 2 && !strcmp(argv[2], "software")));
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    SDL_Window *w = SDL_CreateWindow("bench-movie", 640, 480, 0);
    r = w ? SDL_CreateRenderer(w, name) : NULL;
    if (!r) return fprintf(stderr, "no renderer: %s\n", SDL_GetError()), 1;
    SDL_SetRenderVSync(r, 1);
    size_t len = 0;
    void *data = SDL_LoadFile(argv[1], &len);
    m = data ? gs_movie_open(data, len, r, 48000) : NULL;
    if (!m) return fprintf(stderr, "not a movie gesso plays: %s\n", argv[1]), 1;
    int mw, mh;
    gs_movie_size(m, &mw, &mh);
    printf("renderer %s (asked for %s), movie %d by %d, %.2f s, %s\n", SDL_GetRendererName(r), name ? name : "the default", mw, mh, gs_movie_length(m), gs_movie_has_sound(m) ? "with sound" : "no sound");
    const double length = gs_movie_length(m);
    if (!run(5, have_still)) printf("the still did not come in 5 s\n");
    run(0.2, NULL);
    report("the still, its decoder let go");
    gs_movie_play(m, true);
    run(SDL_min(1.0, length / 2), NULL);
    report("played for a while");
    gs_movie_play(m, false);
    run(0.3, NULL);
    report("paused");
    gs_movie_seek(m, length * 0.75);
    if (!run(10, settled)) printf("the seek did not settle in 10 s\n");
    report("sought to three quarters, paused");
    gs_movie_seek(m, length * 0.1);
    if (!run(10, settled)) printf("the seek did not settle in 10 s\n");
    report("sought back to a tenth, paused");
    gs_movie_seek(m, length * 0.75);
    gs_movie_play(m, true);
    if (!run(length + 5, ended)) printf("it did not end\n");
    report("played to its end");
    gs_movie_play(m, true);
    run(SDL_min(0.5, length / 4), NULL);
    report("played again");
    gs_movie_rest(m);
    run(0.2, NULL);
    report("at rest");
    gs_movie_free(m);
    SDL_free(data);
    SDL_Quit();
    return 0;
}
