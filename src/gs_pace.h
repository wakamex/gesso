// Frame pacing: vsync when the driver gives it, and a frame limiter whenever it does not.
//
// Call gs_pace_set once the renderer exists (and again when the player changes settings), then
// gs_pace_wait right after every SDL_RenderPresent. The limiter covers vsync that is unsupported,
// turned off by the player or the driver's control panel, or reported but not actually
// happening; frame caps below the display rate; and minimised or hidden windows, which drop to
// GS_PACE_HIDDEN_FPS to save power.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define GS_PACE_DISPLAY (-1.0)   // cap: the display's refresh rate
#define GS_PACE_UNLIMITED 0.0    // cap: no limit
#define GS_PACE_HIDDEN_FPS 10.0

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    bool want_vsync;
    double cap;           // frames per second, or GS_PACE_DISPLAY or GS_PACE_UNLIMITED
    // What is in effect.
    int vsync;            // 0 off, n = every nth refresh, -1 adaptive (from SDL_GetRenderVSync)
    double display_hz;
    double target_fps;    // what the limiter holds to; 0 = not limiting
    bool vsync_suspect;   // vsync is reported on but frames come faster than the display
    // Limiter state.
    uint64_t deadline;
    // Checking vsync: frames counted over whole seconds, from a second after gs_pace_set (a driver
    // queues the first few presents without waiting, which would look like vsync ignored).
    uint64_t count_from;
    int counted;
} gs_pace;

void gs_pace_set(gs_pace *p, SDL_Window *win, SDL_Renderer *ren, bool vsync, double cap);
void gs_pace_wait(gs_pace *p);

// A short description of the pacing in effect, such as "vsync adaptive, 60 fps cap".
void gs_pace_describe(const gs_pace *p, char *buf, size_t size);
