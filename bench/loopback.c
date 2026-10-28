// Records what the speakers play for a while and prints, each half second, its level and how much of
// it was recorded rather than filled with silence:
//   zig build bench-loopback -- [SECONDS] [tone] [SINK]
// With `tone`, the bench itself plays a quiet 1 kHz tone (60 dB under full scale) through the default
// output, and prints the tone's level in what was recorded too, which shows the recording to be of
// that output. SINK (Linux) records that sink's monitor in place of the default output's.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_loopback.h"

enum { RATE = 16000, FRAME = 160 };

static void SDLCALL tone(void *data, SDL_AudioStream *s, int needed, int total) {
    (void)total;
    static double phase;
    float block[1024];
    for (int n = needed / (int)sizeof(float); n > 0;) {
        int k = SDL_min(n, 1024);
        for (int i = 0; i < k; i++) block[i] = 0.001f * (float)sin(phase), phase += 2 * SDL_PI_D * 1000 / *(int *)data;
        SDL_PutAudioStreamData(s, block, k * (int)sizeof(float)), n -= k;
    }
}

static double db(double power) { return 10 * log10(power + 1e-20); }

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    bool play = argc > 2 && !strcmp(argv[2], "tone");
    const char *sink = argc > (play ? 3 : 2) ? argv[play ? 3 : 2] : NULL;
    if (!SDL_Init(SDL_INIT_AUDIO)) return printf("SDL_Init: %s\n", SDL_GetError()), 1;
    char error[256];
    gs_loopback *l = gs_loopback_open(sink, RATE, error, sizeof error);
    if (!l) return printf("gs_loopback_open: %s\n", error), 1;
    static int out_rate = 48000;
    if (play) {
        const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, out_rate };
        SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, tone, &out_rate);
        if (!s) return printf("no output to play the tone on: %s\n", SDL_GetError()), 1;
        SDL_ResumeAudioStreamDevice(s);
    }
    double power = 0, re = 0, im = 0;
    int recorded = 0, count = 0;
    Uint64 next = SDL_GetTicksNS();
    for (int frame = 0; frame < seconds * 100; frame++) {
        next += 10000000;
        Uint64 now = SDL_GetTicksNS();
        if (next > now) SDL_DelayNS(next - now);
        float x[FRAME];
        recorded += gs_loopback_read(l, x, FRAME);
        for (int i = 0; i < FRAME; i++, count++) {
            power += (double)x[i] * x[i];
            double w = 2 * SDL_PI_D * 1000 * count / RATE;
            re += x[i] * cos(w), im -= x[i] * sin(w);
        }
        if (count == RATE / 2) {
            // (a sine of amplitude a has power a^2/2; its bin, a*n/2)
            double tone_amp = 2 * sqrt(re * re + im * im) / count;
            printf("level %6.1f dBFS, 1 kHz %6.1f dBFS, recorded %3d%%\n", db(power / count * 2), db(tone_amp * tone_amp), 100 * recorded / count);
            fflush(stdout);
            power = re = im = 0, recorded = count = 0;
        }
    }
    gs_loopback_error(l, error, sizeof error);
    if (error[0]) printf("stopped: %s\n", error);
    gs_loopback_close(l);
    SDL_Quit();
    return 0;
}
