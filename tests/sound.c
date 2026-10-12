// gs_sound on tests/sounds (made by its make.sh): each is two seconds, a 440 Hz tone for the first
// second and 880 Hz for the second, so the pitch heard says where in the sound playing is.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_sound.h"
#include "test.h"

extern const char *test_streams;

// Plays `seconds` of the sound as the mixer would and returns the pitch of what came out, from the
// times it crosses zero going up, or 0 for silence.
static double play(gs_sound *s, double seconds, int *frames_out) {
    float lr[480 * 2];
    int crossings = 0, frames = 0, want = (int)(seconds * 48000);
    float before = 0, peak = 0;
    while (frames < want && !gs_sound_ended(s)) {
        int n = SDL_min(480, want - frames);
        memset(lr, 0, sizeof lr);
        gs_sound_render(s, lr, n);
        for (int i = 0; i < n; i++) {
            if (before <= 0 && lr[2 * i] > 0) crossings++;
            before = lr[2 * i], peak = SDL_max(peak, SDL_fabsf(lr[2 * i]));
        }
        frames += n;
    }
    if (frames_out) *frames_out = frames;
    return peak > 0.01f && frames ? crossings / (frames / 48000.0) : 0;
}

static void check_sound(const char *name, bool playable) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s/../sounds/%s", test_streams, name);
    size_t len = 0;
    void *data = SDL_LoadFile(path, &len);
    CHECK(data != NULL);
    if (!data) return;
    gs_sound *s = gs_sound_open(data, len, 48000);
    if (!playable) {  // (AAC without libavcodec: said plainly, not played wrongly)
        CHECK(s == NULL);
        SDL_free(data);
        return;
    }
    CHECK(s != NULL);
    if (!s) { fprintf(stderr, "  %s did not open\n", name); SDL_free(data); return; }
    CHECK_NEAR(gs_sound_length(s), 2.0, 0.06);
    // Paused, it gives nothing and stays where it is.
    CHECK(play(s, 0.1, NULL) == 0 && gs_sound_position(s) == 0 && !gs_sound_playing(s));
    gs_sound_play(s, true);
    play(s, 0.3, NULL);  // (past the encoder's fade-in)
    CHECK_NEAR(play(s, 0.4, NULL), 440, 12);
    CHECK_NEAR(gs_sound_position(s), 0.7, 0.001);
    play(s, 0.6, NULL);
    CHECK_NEAR(play(s, 0.4, NULL), 880, 20);
    // To its end: it stops there, having played its length.
    int frames = 0;
    play(s, 5, &frames);
    CHECK(gs_sound_ended(s) && !gs_sound_playing(s));
    CHECK_NEAR(gs_sound_position(s), gs_sound_length(s), 0.01);
    // A seek lands on the tone of that second, forwards and back, and playing goes on from there.
    gs_sound_seek(s, 1.5), gs_sound_play(s, true);
    CHECK(!gs_sound_ended(s));
    CHECK_NEAR(play(s, 0.3, NULL), 880, 25);
    CHECK_NEAR(gs_sound_position(s), 1.8, 0.001);
    gs_sound_seek(s, 0.25);
    CHECK_NEAR(play(s, 0.5, NULL), 440, 15);
    gs_sound_play(s, false);
    CHECK(play(s, 0.1, NULL) == 0);
    CHECK_NEAR(gs_sound_position(s), 0.75, 0.001);
    // Played again after its end, it starts over.
    gs_sound_play(s, true), play(s, 5, NULL);
    CHECK(gs_sound_ended(s));
    gs_sound_play(s, true);
    CHECK(gs_sound_position(s) == 0 && gs_sound_playing(s));
    play(s, 0.3, NULL);
    CHECK_NEAR(play(s, 0.4, NULL), 440, 12);
    gs_sound_free(s);
    SDL_free(data);
}

void test_sound(void) {
    if (!test_streams) return;
    bool aac = false;
#ifdef GS_TEST_VIDEO
    aac = true;
#endif
    check_sound("tone.mp3", true);
    check_sound("tone.ogg", true);
    check_sound("tone.m4a", aac);
    check_sound("tone.aac", aac);
    unsigned char noise[4096];
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = (unsigned char)(i * 131 + 7);
    CHECK(gs_sound_open(noise, sizeof noise, 48000) == NULL);
    CHECK(gs_sound_open("RIFF....WAVEfmt ", 16, 48000) == NULL);
}
