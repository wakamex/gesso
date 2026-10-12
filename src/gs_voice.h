// A microphone's signal cleaned for speech, as a browser cleans it: a port of the parts of WebRTC's
// audio processing module that Chromium applies when a page asks for a microphone with noise
// suppression and automatic gain, so that an app recording speech (to transcribe it, to send it)
// gets what a web app would. 16 kHz mono, in frames of 10 ms.
//
// An app that also speaks hears itself through the microphone. Echo cancellation takes that out,
// given what the app plays: it is WebRTC's own canceller (AEC3), compiled from WebRTC's sources
// rather than ported, and present only in a gesso built with -Decho, which fetches them. It behaves
// the same on every system, in place of each system's own (and differing) voice capture mode.
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
    GS_VOICE_ALL = 7,        // the three above
    GS_VOICE_ECHO = 8,       // echo cancellation, of what gs_voice_played is given (a build with -Decho)
    GS_VOICE_SPEECH = 16,    // the voice detector alone, for gs_voice_speech, where the gain is not wanted
};

typedef struct gs_voice gs_voice;

// rate must be 16000; NULL otherwise, and NULL for GS_VOICE_ECHO in a build without it.
gs_voice *gs_voice_new(int rate, int stages);
bool gs_voice_has_echo(void);  // whether this build has the echo canceller
void gs_voice_free(gs_voice *v);
// Processes one frame of 160 samples (10 ms) in place, samples from -1 to 1. The suppressor's output
// is 6 ms behind its input. The first seconds are a settling-in: the suppressor learns the noise
// over about two seconds, and the gain starts at 15 dB and moves by at most 6 dB a second.
void gs_voice_frame(gs_voice *v, float *frame);
// What the app is playing, for the echo canceller: one frame of 160 samples from -1 to 1, mono at
// 16 kHz, for every 10 ms sent to the speakers, silence included, about when it is sent. The
// canceller finds the delay between the two streams itself, up to a few hundred milliseconds, and
// follows it if it drifts; it needs the two to advance at the same rate. May be called from the
// thread that plays while another calls gs_voice_frame. Nothing without GS_VOICE_ECHO.
void gs_voice_played(gs_voice *v, const float *frame);
// How likely the last frame was speech, from 0 to 1, by the gain's voice detector (a small recurrent
// network, WebRTC's and RNNoise's): for telling when someone starts and stops talking. Above 0.95
// is what the gain itself takes for speech. Needs GS_VOICE_GAIN or GS_VOICE_SPEECH.
float gs_voice_speech(const gs_voice *v);
