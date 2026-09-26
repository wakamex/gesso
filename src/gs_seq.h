// A timeline of timed events played sample-accurately: notes and MIDI messages for one synth, and
// calls into any other code (a parameter change, a sound effect) at exact times. A feeder can keep
// the timeline filled ahead, the way a generative score schedules its next bars; other sound sources
// render in step with the events. gs_seq_render is a gs_mix_fn.
//
// Everything that adds events from outside the audio thread must hold gs_mix_lock; the feeder and
// event calls already run on it.
#pragma once
#include <stdint.h>

#include "gs_mix.h"
#include "gs_synth.h"

typedef struct gs_seq gs_seq;
typedef void gs_seq_fn(void *user, double value);
typedef void gs_seq_feed_fn(void *user, gs_seq *q, double until);

gs_seq *gs_seq_new(gs_synth *synth, int rate);
void gs_seq_free(gs_seq *q);
double gs_seq_time(const gs_seq *q);  // seconds, at the next frame to render; starts at 0

// A note at `time`, returning an id for gs_seq_note_off (ids are never 0).
uint32_t gs_seq_note(gs_seq *q, double time, int channel, int note, int velocity);
void gs_seq_note_off(gs_seq *q, double time, uint32_t id);  // releases exactly that note's voices
void gs_seq_midi(gs_seq *q, double time, uint8_t status, uint8_t a, uint8_t b);
void gs_seq_call(gs_seq *q, double time, gs_seq_fn *fn, void *user, double value);
void gs_seq_clear_notes(gs_seq *q);  // drops pending notes and note-offs (calls and MIDI stay)

// `fn` is asked to add events up to `until` (the current time plus `ahead`) before every render.
void gs_seq_set_feeder(gs_seq *q, gs_seq_feed_fn *fn, void *user, double ahead);
void gs_seq_add_source(gs_seq *q, gs_mix_fn *fn, void *user);  // rendered with the synth
void gs_seq_render(void *q, float *lr, int frames);
