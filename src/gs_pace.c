#include "gs_pace.h"

#include <math.h>
#include <stdio.h>

static double display_hz(SDL_Window *win) {
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60;
}

static bool try_vsync(SDL_Renderer *r, int v) {
    int got = 0;
    return SDL_SetRenderVSync(r, v) && SDL_GetRenderVSync(r, &got) && got == v;
}

void gs_pace_set(gs_pace *p, SDL_Window *win, SDL_Renderer *ren, bool vsync, double cap) {
    p->win = win, p->ren = ren, p->want_vsync = vsync, p->cap = cap;
    p->display_hz = display_hz(win);
    p->vsync_suspect = false;
    p->count_from = SDL_GetTicksNS() + SDL_NS_PER_SECOND, p->counted = 0;
    p->deadline = 0;
    double hz = p->display_hz, want = cap == GS_PACE_DISPLAY ? hz : cap;

    // With vsync, prefer every nth refresh over sleeping: it is the only way to get even frames.
    // A cap within 25% below a divisor of the refresh rate uses it (60 on 144 Hz runs at 72, 30 on
    // 60 Hz at 30); a cap further off (40 on 60 Hz) keeps vsync every refresh and the limiter holds it.
    p->vsync = 0;
    int n = want > 0 && want < hz ? (int)floor(hz / want + 0.01) : 1;
    if (n < 1) n = 1;
    // Direct3D presents at most every 4th refresh: SDL takes a longer interval, but then every
    // present fails and the window keeps its last frame. Longer ones go to the limiter.
    bool divisor = (want <= 0 || hz / n <= want * 1.25) && n <= 4;
    if (vsync) {
        if (n > 1 && divisor && try_vsync(ren, n)) p->vsync = n;
        else if (try_vsync(ren, SDL_RENDERER_VSYNC_ADAPTIVE)) p->vsync = SDL_RENDERER_VSYNC_ADAPTIVE;
        else if (try_vsync(ren, 1)) p->vsync = 1;
    }
    if (!p->vsync) SDL_SetRenderVSync(ren, 0);

    double synced = p->vsync > 0 ? hz / p->vsync : p->vsync < 0 ? hz : 0;  // the rate vsync gives
    p->target_fps = want <= 0 ? 0 : synced && (want >= synced - 0.5 || (p->vsync > 1 && divisor)) ? 0 : want;
#ifdef __EMSCRIPTEN__
    // The browser paces frames itself (requestAnimationFrame) and a sleep would busy-wait, so a
    // cap goes to SDL's main loop instead of the limiter.
    if (p->target_fps > 0) {
        char rate[16];
        snprintf(rate, sizeof rate, "%d", (int)p->target_fps);
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, rate);
    } else {
        SDL_ResetHint(SDL_HINT_MAIN_CALLBACK_RATE);  // the browser's own rate
    }
    p->target_fps = 0;
#endif
}

void gs_pace_wait(gs_pace *p) {
#ifdef __EMSCRIPTEN__
    (void)p;  // paced by the browser, see gs_pace_set
#else
    uint64_t now = SDL_GetTicksNS();

    // Vsync that SDL reports but the driver ignores shows up as frames well above the display
    // rate over a whole second; from then on the limiter holds the display rate (or the cap).
    double expect = p->vsync > 0 ? p->display_hz / p->vsync : p->display_hz;
    if (p->vsync && !p->vsync_suspect && now >= p->count_from) {
        p->counted += 1;
        if (now - p->count_from >= SDL_NS_PER_SECOND) {
            double fps = p->counted / ((now - p->count_from) / 1e9);
            if (fps > expect * 1.25) p->vsync_suspect = true;
            p->count_from = now, p->counted = 0;
        }
    }

    double target = p->target_fps;
    if (p->vsync_suspect && (target <= 0 || target > expect)) target = expect;
    SDL_WindowFlags f = SDL_GetWindowFlags(p->win);
    if (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) target = GS_PACE_HIDDEN_FPS;

    if (target > 0) {
        uint64_t period = (uint64_t)(1e9 / target);
        if (!p->deadline || now > p->deadline + period) p->deadline = now;  // first frame, or fell behind
        p->deadline += period;
        if (p->deadline > now) SDL_DelayPrecise(p->deadline - now);
    } else {
        p->deadline = 0;
    }
#endif
}

void gs_pace_describe(const gs_pace *p, char *buf, size_t size) {
    char v[32], c[48];
    if (p->vsync_suspect) snprintf(v, sizeof v, "vsync ignored by driver");
    else if (p->vsync < 0) snprintf(v, sizeof v, "vsync adaptive");
    else if (p->vsync == 1) snprintf(v, sizeof v, "vsync");
    else if (p->vsync > 1) snprintf(v, sizeof v, "vsync every %d (%.0f fps)", p->vsync, p->display_hz / p->vsync);
    else snprintf(v, sizeof v, p->want_vsync ? "no vsync available" : "vsync off");
    if (p->cap == GS_PACE_UNLIMITED) snprintf(c, sizeof c, "no cap");
    else if (p->cap == GS_PACE_DISPLAY) snprintf(c, sizeof c, "cap %.0f Hz display", p->display_hz);
    else snprintf(c, sizeof c, "cap %.0f fps", p->cap);
    snprintf(buf, size, "%s, %s", v, c);
}
