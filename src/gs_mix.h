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

// Copies the most recent `frames` (up to 8192) of mixed output, as heard now (audio still queued for
// the device is skipped), before the volume, into lr. Returns how many frames it copied: fewer only
// just after opening. For scopes, meters and visualizers.
int gs_mix_recent(float *lr, int frames);

// Time spent in gs_mix_render over the duration of the audio it produced, since the last call
// (0.01 = mixing takes 1% of real time). Negative when nothing was rendered.
double gs_mix_load(void);
