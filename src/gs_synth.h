// A small SoundFont 2 / SF3 synthesizer, sample by sample in plain C, ported from a Web Audio
// player and checked against it. Supported: key and velocity zones, preset and instrument
// generators and modulators evaluated at note-on, volume and modulation envelopes, vibrato,
// low-pass filter, pan, tuning, loops, sample offsets and a reverb send. Not supported: the
// modulation LFO, chorus, pitch bend, controller changes during a note.
// SF3 samples (Ogg Vorbis) are decoded at load with stb_vorbis. The reverb is algorithmic
// (Freeverb-style), its level and decay calibrated against a convolution hall of the same length.
// Where the SoundFont spec leaves room it follows SpessaSynth: its default modulators and 0.4
// scaling of initialAttenuation.
//
// Banks can also be built in code (procedural instruments): add sample slots and presets, then
// offer renderings ("takes") for the slots from any thread. Each slot keeps up to GS_BANK_TAKES
// takes and every note plays one at random, so re-rendering slots while music plays keeps notes
// from ever repeating exactly.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Generator numbers from the SoundFont 2.04 specification, for zones built in code.
enum {
    GS_GEN_START_OFFSET = 0, GS_GEN_START_LOOP_OFFSET = 2, GS_GEN_END_LOOP_OFFSET = 3, GS_GEN_START_COARSE = 4,
    GS_GEN_VIB_TO_PITCH = 6, GS_GEN_MOD_ENV_TO_PITCH = 7, GS_GEN_FILTER_FC = 8, GS_GEN_FILTER_Q = 9,
    GS_GEN_MOD_ENV_TO_FILTER = 11, GS_GEN_END_COARSE = 12, GS_GEN_REVERB_SEND = 16, GS_GEN_PAN = 17,
    GS_GEN_DELAY_VIB = 23, GS_GEN_FREQ_VIB = 24, GS_GEN_DELAY_MOD = 25, GS_GEN_ATTACK_MOD = 26,
    GS_GEN_HOLD_MOD = 27, GS_GEN_DECAY_MOD = 28, GS_GEN_SUSTAIN_MOD = 29, GS_GEN_RELEASE_MOD = 30,
    GS_GEN_KEY_TO_MOD_HOLD = 31, GS_GEN_KEY_TO_MOD_DECAY = 32, GS_GEN_DELAY_VOL = 33, GS_GEN_ATTACK_VOL = 34,
    GS_GEN_HOLD_VOL = 35, GS_GEN_DECAY_VOL = 36, GS_GEN_SUSTAIN_VOL = 37, GS_GEN_RELEASE_VOL = 38,
    GS_GEN_KEY_TO_HOLD = 39, GS_GEN_KEY_TO_DECAY = 40, GS_GEN_INSTRUMENT = 41, GS_GEN_KEY_RANGE = 43,
    GS_GEN_VEL_RANGE = 44, GS_GEN_START_LOOP_COARSE = 45, GS_GEN_KEYNUM = 46, GS_GEN_VELOCITY = 47,
    GS_GEN_ATTENUATION = 48, GS_GEN_END_LOOP_COARSE = 50, GS_GEN_COARSE_TUNE = 51, GS_GEN_FINE_TUNE = 52,
    GS_GEN_SAMPLE_ID = 53, GS_GEN_SAMPLE_MODES = 54, GS_GEN_SCALE_TUNING = 56, GS_GEN_EXCLUSIVE_CLASS = 57,
    GS_GEN_ROOT_KEY = 58, GS_GEN_COUNT = 61,
};

#define GS_BANK_TAKES 3

typedef struct gs_bank gs_bank;
typedef struct gs_synth gs_synth;

gs_bank *gs_bank_load(const void *data, size_t len);  // NULL if the file is not a SoundFont
gs_bank *gs_bank_new(void);                           // an empty bank, to build in code
void gs_bank_free(gs_bank *b);

// Building (before any synth plays the bank): a sample slot without audio yet, returning its index,
// then presets whose zones point at slots. A preset added for a program the bank already has is found
// after the existing one, so added presets fill gaps rather than replace.
int gs_bank_add_sample(gs_bank *b, int rate, int root);
typedef struct {
    uint8_t key_lo, key_hi;
    int sample;
    int ngens;
    struct { uint8_t gen; int16_t value; } gens[16];
} gs_zone_spec;
void gs_bank_add_preset(gs_bank *b, int bank, int program, const gs_zone_spec *zones, int nzones);

// Offers a rendering of a slot, from any thread. The bank takes `data`, which must be malloc'd with
// room for len + 1 floats. Loop points are in frames (both 0 without a loop). A synth installs it at
// its next render, retiring the slot's oldest take once no voice plays it.
void gs_bank_offer_take(gs_bank *b, int sample, float *data, int len, int loop_start, int loop_end);
int gs_bank_sample_count(const gs_bank *b);
uint32_t gs_bank_last_used(const gs_bank *b, int sample);  // SDL_GetTicks() of its latest note, 0 if never
void gs_bank_collect(gs_bank *b);                          // frees retired takes; call from the main thread

gs_synth *gs_synth_new(gs_bank *b, int rate);
void gs_synth_free(gs_synth *s);
void gs_synth_midi(gs_synth *s, uint8_t status, uint8_t a, uint8_t b);  // takes effect at once
// Starts a note and returns a tag for its voices (0 when nothing sounds), so exactly these voices can
// be released later even if the same key is struck again meanwhile.
uint32_t gs_synth_note_on(gs_synth *s, int channel, int note, int velocity);
void gs_synth_note_off_tag(gs_synth *s, uint32_t tag);
void gs_synth_release_all(gs_synth *s);  // every sounding voice begins its release
void gs_synth_all_off(gs_synth *s);      // silence at once and reset the channels
void gs_synth_set_mute(gs_synth *s, int channel, bool on);  // fades a channel out or in over about 50 ms
void gs_synth_set_reverb_mute(gs_synth *s, bool on);
// The room: reverb decay close to a convolution hall `seconds` long, at `mix` (1 = as calibrated),
// glided from the current room over about `fade` seconds (0 = at once). The default is 2.2 s at 1.
void gs_synth_set_room(gs_synth *s, double seconds, double mix, double fade);
void gs_synth_render(gs_synth *s, float *lr, int frames);  // adds into interleaved stereo
int gs_synth_active_voices(const gs_synth *s);
