// Turns the system's media controls on for a small window and prints each command that comes:
//   zig build bench-nowplaying -- [TITLE] [ARTIST] [SECONDS]
// For checking on each system that the media keys and the media overlay reach a program whose window
// is in the background. Play and pause flip what the overlay shows; stop ends it.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_nowplaying.h"

int main(int argc, char **argv) {
    const char *title = argc > 1 ? argv[1] : "A song from gesso", *artist = argc > 2 ? argv[2] : "gesso bench";
    int seconds = argc > 3 ? atoi(argv[3]) : 60;
    if (!SDL_Init(SDL_INIT_VIDEO)) return printf("SDL_Init: %s\n", SDL_GetError()), 1;
    SDL_Window *win = SDL_CreateWindow("gs_nowplaying bench", 320, 120, 0);
    Uint32 type = gs_nowplaying_init(win, "gesso bench", "gessobench");
    if (!type) return printf("no media controls here\n"), 1;
    int state = GS_NOWPLAYING_PLAYING;
    gs_nowplaying_set(title, artist, state);
    printf("on: press the media keys, or use the system's media overlay\n");
    fflush(stdout);
    static const char *const names[] = { "", "play", "pause", "toggle", "next", "previous", "stop" };
    for (Uint64 end = SDL_GetTicks() + 1000 * (Uint64)seconds; SDL_GetTicks() < end && state != GS_NOWPLAYING_STOPPED;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 200)) continue;
        if (e.type == SDL_EVENT_QUIT) break;
        if (e.type != type) continue;
        printf("%s\n", e.user.code > 0 && e.user.code <= 6 ? names[e.user.code] : "?");
        fflush(stdout);
        int next = e.user.code == GS_NOWPLAYING_PLAY ? GS_NOWPLAYING_PLAYING : e.user.code == GS_NOWPLAYING_PAUSE ? GS_NOWPLAYING_PAUSED
                 : e.user.code == GS_NOWPLAYING_TOGGLE ? (state == GS_NOWPLAYING_PLAYING ? GS_NOWPLAYING_PAUSED : GS_NOWPLAYING_PLAYING)
                 : e.user.code == GS_NOWPLAYING_STOP ? GS_NOWPLAYING_STOPPED : state;
        if (e.user.code == GS_NOWPLAYING_NEXT || e.user.code == GS_NOWPLAYING_PREVIOUS) {  // another track, still playing
            static char track[128];
            static int n;
            SDL_snprintf(track, sizeof track, "%s, track %d", argc > 1 ? argv[1] : "A song from gesso", e.user.code == GS_NOWPLAYING_NEXT ? ++n : --n);
            title = track;
            gs_nowplaying_set(title, artist, state);
        } else if (next != state) gs_nowplaying_set(title, artist, state = next);
    }
    gs_nowplaying_quit();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
