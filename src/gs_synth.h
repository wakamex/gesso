// A small SoundFont 2 / SF3 synthesizer, sample by sample in plain C, ported from a Web Audio
// player and checked against it. Supported: key and velocity zones, preset and instrument
// generators and modulators evaluated at note-on, volume and modulation envelopes, vibrato,
// low-pass filter, pan, tuning, loops, sample offsets and a reverb send. Not supported: the
// modulation LFO, chorus, pitch bend, controller changes during a note.
// SF3 samples (Ogg Vorbis) are decoded at load with stb_vorbis. The reverb is algorithmic
// (Freeverb-style), its level calibrated against a convolution reverb.
// Where the SoundFont spec leaves room it follows SpessaSynth: its default modulators and 0.4
// scaling of initialAttenuation.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct gs_bank gs_bank;
typedef struct gs_synth gs_synth;

gs_bank *gs_bank_load(const void *data, size_t len);  // NULL if the file is not a SoundFont
void gs_bank_free(gs_bank *b);

gs_synth *gs_synth_new(const gs_bank *b, int rate);
void gs_synth_free(gs_synth *s);
void gs_synth_midi(gs_synth *s, uint8_t status, uint8_t a, uint8_t b);  // takes effect at once
void gs_synth_all_off(gs_synth *s);
void gs_synth_render(gs_synth *s, float *lr, int frames);  // adds into interleaved stereo
int gs_synth_active_voices(const gs_synth *s);
void gs_synth_set_reverb(gs_synth *s, float mix);  // 1 = the built-in room, 0 = dry
