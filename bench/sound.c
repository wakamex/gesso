// Opens a sound file with gs_sound and plays it through, without a device, as fast as it decodes:
//   zig build bench-sound [-Dvideo] -- FILE [SEEK_SECONDS]
// Reports its length, how much came out, and how long decoding took, for trying files of each kind
// (AAC needs -Dvideo). With a second argument it seeks there first.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_sound.h"

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-sound FILE [SEEK_SECONDS]\n"), 1;
    size_t len = 0;
    void *data = SDL_LoadFile(argv[1], &len);
    if (!data) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    gs_sound *s = gs_sound_open(data, len, 48000);
    if (!s) return printf("%s: not a sound gs_sound plays\n", argv[1]), 1;
    double from = argc > 2 ? atof(argv[2]) : 0, peak = 0;
    if (from > 0) gs_sound_seek(s, from);
    gs_sound_play(s, true);
    uint64_t start = SDL_GetTicksNS();
    long frames = 0;
    while (!gs_sound_ended(s) && frames < 48000L * 3600) {
        float lr[960] = { 0 };
        gs_sound_render(s, lr, 480);
        for (int i = 0; i < 960; i++) peak = SDL_max(peak, SDL_fabs(lr[i]));
        frames += 480;
    }
    double took = (double)(SDL_GetTicksNS() - start) / 1e9;
    printf("%s: %.3f s long, played %.3f s from %.3f, peak %.2f, decoded in %.3f s (%.0f times real time)\n", argv[1], gs_sound_length(s),
           gs_sound_position(s) - from, from, peak, took, took > 0 ? (gs_sound_position(s) - from) / took : 0);
    gs_sound_free(s);
    SDL_free(data);
    return 0;
}
