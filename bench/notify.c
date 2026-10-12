// Shows a desktop notification and waits for it to be clicked:
//   zig build bench-notify -- [TITLE] [BODY] [SECONDS]
// For checking on each system by eye that it appears under the app's name and that a click comes
// back. On macOS it must run from an app bundle (tools/bundle-macos.sh).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_notify.h"

static SDL_AtomicInt clicked;

static void on_click(const char *tag) {
    printf("clicked %s\n", tag);
    fflush(stdout);
    SDL_SetAtomicInt(&clicked, 1);
}

int main(int argc, char **argv) {
    int seconds = argc > 3 ? atoi(argv[3]) : 20;
    if (!SDL_Init(SDL_INIT_VIDEO)) SDL_Init(SDL_INIT_EVENTS);  // (video where there is a desktop: macOS hears of a click through its events)
    if (!gs_notify_init("gesso bench", "dev.wakamex.gesso.bench", NULL, on_click)) return printf("no way to notify here\n"), 1;
    bool shown = gs_notify_show("bench", argc > 1 ? argv[1] : "A notification from gesso", argc > 2 ? argv[2] : "Click it, and the click comes back to the program.");
    printf(shown ? "shown\n" : "not shown\n");
    fflush(stdout);
    for (Uint64 end = SDL_GetTicks() + 1000 * (Uint64)seconds; shown && SDL_GetTicks() < end && !SDL_GetAtomicInt(&clicked);) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}  // (macOS delivers a click through the main thread's events)
        SDL_Delay(50);
    }
    if (!SDL_GetAtomicInt(&clicked)) gs_notify_close("bench");
    SDL_Quit();
    return shown ? 0 : 1;
}
