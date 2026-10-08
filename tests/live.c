// gs_live playing the synthetic playlists from files (file:// through libcurl; not on Windows, where
// WinHTTP has no file URLs), with the mix pulled offline in place of an audio device.
#include <SDL3/SDL.h>
#include <math.h>

#include "test.h"

#if defined(GS_TEST_VIDEO) && !defined(_WIN32)
#include "gs_live.h"
#include "gs_mix.h"

extern const char *test_streams;

static void play(const char *playlist) {
    char url[1200];
    SDL_snprintf(url, sizeof url, "file://%s/%s", test_streams, playlist);
    gs_live *l = gs_live_start(&(gs_live_config){ .url = url });  // (complete playlists play from their start)
    CHECK(l != NULL);
    if (!l) return;
    static float lr[2 * 1024];
    double frames = 0, crossings = 0, last_clock = -1;
    float last = 0;
    bool clock_steady = true;
    uint64_t until = SDL_GetTicks() + 20000;
    gs_live_info i;
    while ((i = gs_live_get_info(l)).state != GS_LIVE_ENDED && SDL_GetTicks() < until) {
        // Mixing offline runs faster than the decoder: mix only what has been decoded, as a device would
        // in real time, so the stream does not run dry.
        if (i.state == GS_LIVE_STARTING || i.state == GS_LIVE_BUFFERING || (i.buffered < 0.5 && i.queued > 0)) {
            if (i.state != GS_LIVE_PLAYING) gs_mix_render(lr, 1024);  // (lets the stream start, without counting)
            SDL_Delay(1);
            continue;
        }
        gs_mix_render(lr, 1024);
        double clock = gs_live_get_info(l).clock;
        if (last_clock >= 0 && (clock < last_clock - 0.001 || clock - last_clock > 0.05)) {  // no jump at the discontinuity
            fprintf(stderr, "  clock went from %.4f to %.4f\n", last_clock, clock);
            clock_steady = false;
        }
        last_clock = clock;
        for (int k = 0; k < 1024; k++) {  // (the exact zeros of a stream waiting for data are not the tone)
            if (lr[2 * k] == 0 && lr[2 * k + 1] == 0) continue;
            if ((lr[2 * k] >= 0) != (last >= 0)) crossings++;
            last = lr[2 * k];
            frames++;
        }
    }
    CHECK(i.state == GS_LIVE_ENDED);
    CHECK(i.segments == 6 && i.discontinuities == 1);
    CHECK(fabs(frames / 48000 - 12) < 0.3);         // both halves, the 44.1 kHz one resampled
    CHECK(fabs(crossings / (frames / 48000) - 880) < 30);
    CHECK(clock_steady && fabs(last_clock - 12) < 0.3);
    gs_live_stop(l);
}

void test_live(void) {
    if (!test_streams) return;
    play("ts.m3u8");
    play("mp4.m3u8");
}
#else
void test_live(void) {}
#endif
