// Runs gs_voice over a 16 kHz mono 16-bit WAV file, as WebRTC's module is run on 16-bit frames:
//   zig build bench-voice -- IN.wav OUT.wav [STAGES]
// STAGES is the sum of the stages wanted (1 high-pass, 2 noise suppression, 4 gain; all unless
// given). For comparing gs_voice with the original on the same file, and for trying it on recordings.
//   zig build bench-voice -- --speech IN.wav [STAGES]
// prints the voice detector's probability of speech for each 10 ms frame, one a line, for finding the
// pauses of a recording. STAGES are stages to run before the detector (3 for the filter and the
// suppressor, as WebRTC's gain controller hears it; none unless given). Any length; the WAV file's
// chunks are read, so a header longer than 44 bytes is fine.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_voice.h"

// Moves a WAV file to the start of its samples. False unless it is 16 kHz, mono and 16-bit.
static bool wav_samples(FILE *f) {
    unsigned char h[12], c[8], fmt[16];
    bool right = false;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    while (fread(c, 1, 8, f) == 8) {
        long size = (long)(c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24);
        if (!memcmp(c, "data", 4)) return right;
        if (!memcmp(c, "fmt ", 4) && size >= 16) {
            if (fread(fmt, 1, 16, f) != 16) return false;
            right = (fmt[0] | fmt[1] << 8) == 1 && (fmt[2] | fmt[3] << 8) == 1 && (fmt[4] | fmt[5] << 8 | fmt[6] << 16) == 16000 && (fmt[14] | fmt[15] << 8) == 16;
            size -= 16;
        }
        if (fseek(f, size + (size & 1), SEEK_CUR)) return false;
    }
    return false;
}

static int speech(int argc, char **argv) {
    FILE *in = argc > 2 ? fopen(argv[2], "rb") : NULL;
    if (!in || !wav_samples(in)) return fprintf(stderr, "bench-voice --speech IN.wav [STAGES]: a 16 kHz mono 16-bit WAV file\n"), 1;
    gs_voice *v = gs_voice_new(16000, GS_VOICE_SPEECH | (argc > 3 ? atoi(argv[3]) & (GS_VOICE_HIGH_PASS | GS_VOICE_NOISE) : 0));
    short pcm[160];
    while (fread(pcm, 2, 160, in) == 160) {
        float frame[160];
        for (int i = 0; i < 160; i++) frame[i] = (float)pcm[i] / 32768.f;
        gs_voice_frame(v, frame);
        printf("%.4f\n", gs_voice_speech(v));
    }
    gs_voice_free(v);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--speech")) return speech(argc, argv);
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
