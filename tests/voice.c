// gs_voice against WebRTC's own audio processing module (webrtc-audio-processing 2.1, with its
// high-pass filter, its noise suppression at the high level and its second gain controller's
// adaptive digital gain on, at 16 kHz) on tests/voice/speech.wav: a sentence from the FLEURS corpus
// with white noise 15 dB below the speech, made quiet. The expected numbers are the level of each
// quarter second of what the original gives, in dB under full scale.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_voice.h"
#include "test.h"

extern const char *test_streams;

enum { QUARTERS = 14, QUARTER = 4000 };

static void check_levels(const Sint16 *pcm, int stages, const double *expected) {
    gs_voice *v = gs_voice_new(16000, stages);
    CHECK(v != NULL);
    if (!v) return;
    for (int q = 0; q < QUARTERS; q++) {
        double sum = 0;
        for (int at = q * QUARTER; at < (q + 1) * QUARTER; at += 160) {
            float frame[160];
            for (int i = 0; i < 160; i++) frame[i] = (float)pcm[at + i] / 32768.f;
            gs_voice_frame(v, frame);
            for (int i = 0; i < 160; i++) sum += (double)frame[i] * frame[i];
        }
        double level = 10 * log10(sum / QUARTER + 1e-12);
        if (fabs(level - expected[q]) > 0.1) fprintf(stderr, "  stages %d, quarter second %d: %.2f dB, expected %.2f\n", stages, q, level, expected[q]), test_failures++;
    }
    gs_voice_free(v);
}

void test_voice(void) {
    CHECK(gs_voice_new(48000, GS_VOICE_ALL) == NULL);  // (16 kHz only)
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/../voice/speech.wav", test_streams);
    size_t len = 0;
    Uint8 *data = SDL_LoadFile(path, &len);
    CHECK(data != NULL && len >= 44 + QUARTERS * QUARTER * 2);
    if (!data || len < 44 + QUARTERS * QUARTER * 2) { SDL_free(data); return; }
    Sint16 *pcm = SDL_malloc(len - 44);
    memcpy(pcm, data + 44, len - 44);  // (a 44-byte header, then 16-bit samples)
    // The filter alone leaves speech as it is; the suppressor takes the noise between words down by
    // 18 dB; the gain brings the quiet speech up and, the noise being low by then, leaves the pauses.
    static const double high_pass[QUARTERS] = { -45.38, -45.44, -24.95, -23.40, -32.90, -27.01, -28.61, -33.96, -33.91, -34.28, -39.15, -45.23, -45.46, -45.37 };
    static const double suppressed[QUARTERS] = { -57.46, -60.39, -25.51, -23.69, -34.85, -28.27, -29.28, -36.95, -36.57, -36.41, -43.09, -63.15, -63.43, -63.32 };
    static const double all[QUARTERS] = { -43.14, -47.51, -13.34, -12.38, -25.25, -20.16, -22.47, -31.48, -30.42, -29.41, -35.28, -54.91, -55.18, -55.07 };
    check_levels(pcm, GS_VOICE_HIGH_PASS, high_pass);
    check_levels(pcm, GS_VOICE_HIGH_PASS | GS_VOICE_NOISE, suppressed);
    check_levels(pcm, GS_VOICE_ALL, all);
    SDL_free(pcm), SDL_free(data);
}
