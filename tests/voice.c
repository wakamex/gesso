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

static double energy(const float *x, int n) {
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)x[i] * x[i];
    return sum;
}

// The voice detector: sure of speech in the middle of the sentence, and of none in the noise before it.
static void check_speech(const Sint16 *pcm) {
    gs_voice *v = gs_voice_new(16000, GS_VOICE_SPEECH);
    CHECK(v != NULL);
    if (!v) return;
    double before = 0, during = 0;
    for (int at = 0; at < 3 * QUARTER + 2000; at += 160) {
        float frame[160];
        for (int i = 0; i < 160; i++) frame[i] = (float)pcm[at + i] / 32768.f;
        gs_voice_frame(v, frame);
        if (at >= 3200 && at < 8000) before = SDL_max(before, gs_voice_speech(v));
        if (at >= 2 * QUARTER + 2000) during = SDL_max(during, gs_voice_speech(v));
        CHECK(frame[0] == (float)pcm[at] / 32768.f);  // (the detector alone changes nothing)
    }
    CHECK(before < 0.3);
    CHECK(during > 0.95);
    gs_voice_free(v);
}

// The echo canceller: the app plays the sentence, and the microphone hears it 30 ms later at half
// its level with a little noise of the room's. From one second in, while the sentence is being
// said, the echo is taken out of what comes out; then someone talks over silence from the app, and
// is let through.
static void check_echo(const Sint16 *pcm, int samples) {
    CHECK(gs_voice_new(16000, GS_VOICE_ECHO) != NULL || !gs_voice_has_echo());
    if (!gs_voice_has_echo()) return;
    gs_voice *v = gs_voice_new(16000, GS_VOICE_ECHO);
    enum { DELAY = 480, SECOND = 16000 };
    unsigned seed = 1;
    double heard = 0, left = 0, said = 0, passed = 0;
    for (int at = 0; at + 160 <= 6 * SECOND; at += 160) {
        const bool talking = at >= 4 * SECOND;
        float played[160], frame[160], clean[160];
        for (int i = 0; i < 160; i++) {
            const int t = at + i;
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((float)(seed >> 8) / 8388608.f - 1.f) * 0.0003f;
            played[i] = talking ? 0 : 3.f * (float)pcm[t % samples] / 32768.f;
            const float echo = talking || t < DELAY ? 0 : 0.5f * 3.f * (float)pcm[(t - DELAY) % samples] / 32768.f;
            clean[i] = talking ? 3.f * (float)pcm[samples - 1 - t % samples] / 32768.f : 0;  // (the sentence backwards: speech-like, and not what was played)
            frame[i] = echo + clean[i] + noise;
        }
        const double in = energy(frame, 160);
        gs_voice_played(v, played);
        gs_voice_frame(v, frame);
        if (at >= SECOND && at < 3 * SECOND) heard += in, left += energy(frame, 160);
        if (at >= 4 * SECOND + SECOND / 2) said += in, passed += energy(frame, 160);
    }
    const double echo_down = 10 * log10(heard / left), talker_change = 10 * log10(passed / said);
    CHECK(echo_down > 20);              // (the echo, down by more than 20 dB)
    CHECK(fabs(talker_change) < 3);     // (the talker, within 3 dB)
    if (echo_down <= 20 || fabs(talker_change) >= 3) fprintf(stderr, "  echo down %.1f dB, talker changed %.1f dB\n", echo_down, talker_change);
    gs_voice_free(v);
}

// The echo canceller given what the speakers play as its reference (gs_loopback's use): a podcast
// from another app comes back through the microphone 100 ms later, louder than the talker, and
// nobody talks for four seconds; then someone does. How many frames the voice detector takes for
// speech while only the podcast plays, with the reference and without it.
static int podcast_speech(const Sint16 *pcm, int samples, bool reference, double *talker) {
    gs_voice *v = gs_voice_new(16000, GS_VOICE_ECHO | GS_VOICE_SPEECH);
    enum { DELAY = 1600, SECOND = 16000 };
    unsigned seed = 7;
    int speech = 0;
    *talker = 0;
    for (int at = 0; at + 160 <= 7 * SECOND; at += 160) {
        const bool talking = at >= 5 * SECOND;
        float played[160], frame[160];
        for (int i = 0; i < 160; i++) {
            const int t = at + i;
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((float)(seed >> 8) / 8388608.f - 1.f) * 0.0003f;
            const float podcast = t < DELAY ? 0 : 0.7f * 3.f * (float)pcm[(t - DELAY) % samples] / 32768.f;
            played[i] = 3.f * (float)pcm[t % samples] / 32768.f;
            frame[i] = podcast + (talking ? 3.f * (float)pcm[samples - 1 - t % samples] / 32768.f : 0) + noise;
        }
        gs_voice_played(v, reference ? played : (float[160]){ 0 });
        gs_voice_frame(v, frame);
        if (at >= 2 * SECOND && at < 5 * SECOND) speech += gs_voice_speech(v) > 0.5f;
        if (at >= 5 * SECOND + SECOND / 2) *talker = SDL_max(*talker, gs_voice_speech(v));
    }
    gs_voice_free(v);
    return speech;
}

static void check_podcast(const Sint16 *pcm, int samples) {
    if (!gs_voice_has_echo()) return;
    double talker_with, talker_without;
    const int with = podcast_speech(pcm, samples, true, &talker_with), without = podcast_speech(pcm, samples, false, &talker_without);
    fprintf(stderr, "  podcast frames taken for speech, of 300: %d without the reference, %d with it\n", without, with);
    CHECK(without > 100);    // (the podcast alone sets the detector off)
    CHECK(with < 15);        // (with what the speakers play, hardly ever)
    CHECK(talker_with > 0.95);  // (and the talker is still heard)
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
    check_speech(pcm);
    check_echo(pcm, (int)(len - 44) / 2);
    check_podcast(pcm, (int)(len - 44) / 2);
    SDL_free(pcm), SDL_free(data);
}
