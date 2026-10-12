// The echo canceller of WebRTC's audio processing module (AEC3), as it is, behind four C calls for
// gs_voice. Built only with -Decho, from the WebRTC sources the build fetches.
#include <memory>
#include <optional>

#include "api/audio/echo_canceller3_config.h"
#include "modules/audio_processing/aec3/echo_canceller3.h"
#include "modules/audio_processing/audio_buffer.h"

namespace {
const int kRate = 16000;
}

struct gs_echo {
    webrtc::EchoCanceller3 canceller;
    webrtc::AudioBuffer played, captured;
    gs_echo() : canceller(webrtc::EchoCanceller3Config(), std::nullopt, kRate, 1, 1), played(kRate, 1, kRate, 1, kRate, 1), captured(kRate, 1, kRate, 1, kRate, 1) {}
};

extern "C" {

gs_echo *gs_echo_new(void) { return new (std::nothrow) gs_echo(); }

void gs_echo_free(gs_echo *e) { delete e; }

// Frames are 160 samples from -1 to 1.
void gs_echo_played(gs_echo *e, const float *frame) {
    const float *const channels[1] = { frame };
    e->played.CopyFrom(channels, webrtc::StreamConfig(kRate, 1));
    e->canceller.AnalyzeRender(&e->played);
}

void gs_echo_captured(gs_echo *e, float *frame) {
    const webrtc::StreamConfig config(kRate, 1);
    const float *const in[1] = { frame };
    float *const out[1] = { frame };
    e->captured.CopyFrom(in, config);
    e->canceller.AnalyzeCapture(&e->captured);
    e->canceller.ProcessCapture(&e->captured, false);
    e->captured.CopyTo(config, out);
}

}
