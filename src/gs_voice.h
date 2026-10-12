// A microphone's signal cleaned for speech, as a browser cleans it: a port of the parts of WebRTC's
// audio processing module that Chromium applies when a page asks for a microphone with noise
// suppression and automatic gain, so that an app recording speech (to transcribe it, to send it)
// gets what a web app would. 16 kHz mono, in frames of 10 ms. Echo cancellation, which needs what
// the speakers are playing, is not part of it.
//
// It is tuned for a listener. A speech recogniser trained on noisy audio may gain nothing from it:
// on 324 sentences of the FLEURS corpus transcribed by Parakeet TDT 0.6b v3, the word error rate
// with all three stages was 6.26% against 6.27% without for the recordings as they are, and 9.28%
// against 8.35% with white noise 15 dB below the speech (the 95% interval of that difference is
// -0.03 to 2.24 points). The loss comes with the gain: the filter and the suppressor alone scored
// 8.32%. The gain starts at 15 dB, and speech that is already loud is pressed against the limiter
// until it has come down.
#pragma once
#include <stdbool.h>

enum {
    GS_VOICE_HIGH_PASS = 1,  // a 100 Hz high-pass filter: rumble and any offset
    GS_VOICE_NOISE = 2,      // noise suppression, at WebRTC's "high" level (up to 18 dB)
    GS_VOICE_GAIN = 4,       // automatic gain and a limiter: speech brought near full scale, as a voice detector finds it
    GS_VOICE_ALL = 7,
};

typedef struct gs_voice gs_voice;

gs_voice *gs_voice_new(int rate, int stages);  // rate must be 16000; NULL otherwise
void gs_voice_free(gs_voice *v);
// Processes one frame of 160 samples (10 ms) in place, samples from -1 to 1. The suppressor's output
// is 6 ms behind its input. The first seconds are a settling-in: the suppressor learns the noise
// over about two seconds, and the gain starts at 15 dB and moves by at most 6 dB a second.
void gs_voice_frame(gs_voice *v, float *frame);
