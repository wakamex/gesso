// Audio output: one SDL playback stream, fed by adding together any number of sources.
// A source is a function that adds `frames` of interleaved stereo float samples into a buffer.
// The same mix can be rendered offline (gs_mix_render) for tests and exports.
#pragma once
#include <stdbool.h>

typedef void gs_mix_fn(void *user, float *lr, int frames);

bool gs_mix_open(int rate);  // opens the default playback device
void gs_mix_close(void);
int gs_mix_rate(void);

// Sources are called on the audio thread; wrap changes to their state in lock/unlock.
void gs_mix_add(gs_mix_fn *fn, void *user);
void gs_mix_remove(gs_mix_fn *fn, void *user);
void gs_mix_lock(void);
void gs_mix_unlock(void);
void gs_mix_set_volume(float v);

// Sums every source into lr (overwritten) with a soft limiter. Called by the device, or directly.
void gs_mix_render(float *lr, int frames);
