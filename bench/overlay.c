// Shows gs_desk's overlay window on the desktop for a few seconds: a rounded card at the bottom of
// the main display that stays above other windows and never takes the keyboard.
//   zig build bench-overlay -- [SECONDS] [window|window-raised]
// For checking on each system by eye that it floats over other apps, on every workspace, and that
// typing still goes to the app behind it. Prints the window's id for tools such as xprop.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_desk.h"
#include "gs_draw.h"
#include "gs_text.h"

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    // `window` or `window-raised` after the seconds: an ordinary window from an app kept out of the Dock,
    // brought forward with gs_window_activate or with SDL_RaiseWindow alone, reporting whether it got
    // the keyboard. On macOS only the first does, when the program runs as a background app bundle.
    if (argc > 2) {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
        SDL_Window *win = SDL_CreateWindow("gesso window", 360, 200, SDL_WINDOW_HIDDEN);
        SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
        if (!ren) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
        // It waits four seconds first, as an app in the tray waits for its menu: time to put another app in front.
        for (Uint64 end = SDL_GetTicks() + 4000; SDL_GetTicks() < end; SDL_Delay(16)) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
        }
        SDL_ShowWindow(win);
        if (!SDL_strcmp(argv[2], "window")) gs_window_activate(win); else SDL_RaiseWindow(win);
        for (Uint64 end = SDL_GetTicks() + 1000 * (Uint64)seconds; SDL_GetTicks() < end;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
            SDL_SetRenderDrawColor(ren, 24, 24, 27, 255);
            SDL_RenderClear(ren);
            SDL_RenderPresent(ren);
            SDL_Delay(16);
        }
        printf("%s: the window %s the keyboard\n", argv[2], SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS ? "has" : "does not have");
        SDL_Quit();
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    SDL_Window *w = gs_overlay_window("gesso overlay", 360, 72);
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, NULL) : NULL;
    if (!r) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    SDL_Rect area;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area)) SDL_SetWindowPosition(w, area.x + (area.w - 360) / 2, area.y + area.h - 72 - 40);
    gs_overlay_show(w, true);
    gs_fontset *fs = gs_fontset_system_weight(600);
    gs_glyphs *g = gs_glyphs_new(r, 1024);
    printf("driver %s, window %u, X11 window 0x%lx\n", SDL_GetCurrentVideoDriver(), SDL_GetWindowID(w),
           (unsigned long)SDL_GetNumberProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    fflush(stdout);
    for (Uint64 end = SDL_GetTicks() + 1000 * (Uint64)seconds; SDL_GetTicks() < end;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) end = 0;
        int pw, ph, ww;
        SDL_GetCurrentRenderOutputSize(r, &pw, &ph);
        SDL_GetWindowSize(w, &ww, NULL);
        float s = ww > 0 ? (float)pw / (float)ww : 1;
        gs_glyphs_begin_frame(g);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColorFloat(r, 0, 0, 0, 0);
        SDL_RenderClear(r);
        gs_draw_round(r, (SDL_FRect){ 0, 0, (float)pw, (float)ph }, 16 * s, gs_rgba(0x18181b, 0.96f));
        gs_draw_outline(r, (SDL_FRect){ 0, 0, (float)pw, (float)ph }, 16 * s, s, gs_rgba(0xffffff, 0.12f));
        gs_draw_circle(r, 28 * s, (float)ph / 2, 7 * s, gs_rgba(0xfb4d5c, 1));
        gs_fontset_draw(g, fs, 15 * s, 48 * s, (float)ph / 2 + 5 * s, SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS ? "This overlay has the keyboard (it should not)" : "An overlay that never takes the keyboard", gs_rgba(0xf4f4f5, 1));
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    gs_overlay_show(w, false);
    gs_glyphs_free(g), gs_fontset_free(fs);
    SDL_Quit();
    return 0;
}
