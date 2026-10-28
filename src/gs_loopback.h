// What the speakers play: the mix the default output device plays, from every app (this one's own
// included), recorded alongside the microphone. Its use is as an echo canceller's reference
// (gs_voice with GS_VOICE_ECHO, through gs_voice_played): an always-on listener then hears only the
// user while a podcast or a call plays from another app, which a canceller given only the app's own
// playback cannot take out.
//
// - Windows: WASAPI loopback on the default render device, which needs no permission. When the
//   default device changes, or the one in use goes away, it moves to the new default.
// - Linux: the monitor of the default sink through PulseAudio (PipeWire serves the same calls), with
//   libpulse-simple opened at run time, so nothing is needed to build.
// - macOS: not yet, and gs_loopback_open says so.
//
// A thread of its own records; gs_loopback_read gives the samples as mono floats at the asked rate.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct gs_loopback gs_loopback;

// Starts recording what `device` plays (NULL for the default output, followed when it changes; on
// Linux the name of a sink, whose monitor is recorded), at `rate` samples a second, mono. NULL, with
// why in `error`, where the system has no way to, or the device cannot be opened.
gs_loopback *gs_loopback_open(const char *device, int rate, char *error, size_t error_size);
// The next `frames` samples, oldest first, always all of them: what was recorded, and silence where
// nothing was (the system sends nothing while nothing plays), so that read once for each block of
// microphone samples it stays in step with them. When the app has fallen behind by more than 200 ms,
// the oldest samples go. Returns how many of them were recorded rather than silence.
int gs_loopback_read(gs_loopback *l, float *out, int frames);
// Why recording stopped, or "" while it goes on (a device that went away and could not be replaced).
void gs_loopback_error(gs_loopback *l, char *error, size_t size);
void gs_loopback_close(gs_loopback *l);
