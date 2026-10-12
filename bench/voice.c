// Runs gs_voice over a 16 kHz mono 16-bit WAV file, as WebRTC's module is run on 16-bit frames:
//   zig build bench-voice -- IN.wav OUT.wav [STAGES]
// STAGES is the sum of the stages wanted (1 high-pass, 2 noise suppression, 4 gain; all unless
// given). For comparing gs_voice with the original on the same file, and for trying it on recordings.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gs_voice.h"

int main(int argc, char **argv) {
    if (argc < 3) return fprintf(stderr, "usage: bench-voice IN.wav OUT.wav [STAGES]\n"), 1;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    char head[44];
    if (!in || !out || fread(head, 1, 44, in) != 44) return fprintf(stderr, "cannot open the files\n"), 1;
    fwrite(head, 1, 44, out);
    gs_voice *v = gs_voice_new(16000, argc > 3 ? atoi(argv[3]) : GS_VOICE_ALL);
    short pcm[160];
    float gain = argc > 4 ? (float)atof(argv[4]) : 1;  // (a fixed gain after the stages, for experiments)
    while (fread(pcm, 2, 160, in) == 160) {
        float frame[160];
        for (int i = 0; i < 160; i++) frame[i] = (float)pcm[i] / 32768.f;
        gs_voice_frame(v, frame);
        for (int i = 0; i < 160 && gain != 1; i++) frame[i] *= gain;
        for (int i = 0; i < 160; i++) {  // (rounded as the original rounds its output)
            float x = SDL_clamp(frame[i] * 32768.f, -32768.f, 32767.f);
            pcm[i] = (short)(x + SDL_copysignf(0.5f, x));
        }
        fwrite(pcm, 2, 160, out);
    }
    fclose(out);
    gs_voice_free(v);
    return 0;
}
