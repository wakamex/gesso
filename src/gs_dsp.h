// Building blocks for synthesized sound: an FFT, Web Audio's filters, resonators, and two physical
// models of strings (a plucked string after extended Karplus-Strong, a bowed string after the
// Synthesis ToolKit's Bowed). Instruments and sound effects are recipes over these; everything random
// takes a seeded gs_mulberry, so a recipe and a seed always give the same sound.
#pragma once
#include <stdbool.h>

#include "gs_rand.h"

// In place, radix 2 (n a power of two), unscaled in both directions.
void gs_fft(double *re, double *im, int n, bool inverse);

// A biquad filter, direct form I. The setters follow Web Audio's BiquadFilterNode.
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } gs_biquad;
void gs_biquad_lowpass(gs_biquad *f, double rate, double hz, double q_db);  // Q in decibels
void gs_biquad_bandpass(gs_biquad *f, double rate, double hz, double q);    // Q as a ratio
void gs_biquad_peak(gs_biquad *f, double rate, double hz, double bw_hz);    // band-pass, 0 dB at its centre
static inline double gs_biquad_run(gs_biquad *f, double x) {
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1, f->x1 = x, f->y2 = f->y1, f->y1 = y;
    return y;
}

// A two-pole resonator with unity gain at DC (Klatt), for formants and body modes.
typedef struct { double a, b, c, y1, y2; } gs_resonator;
void gs_resonator_init(gs_resonator *r, double rate, double hz, double bw_hz);
static inline double gs_resonator_run(gs_resonator *r, double x) {
    double y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1, r->y1 = y;
    return y;
}

void gs_normalize(float *d, int n, float peak);                       // largest sample to `peak`
void gs_crossfade_loop(float *d, int start, int end, int fade);       // so [start, end) loops seamlessly
double gs_note_hz(double note);  // equal temperament, A4 = 440 Hz

// A plucked string (extended Karplus-Strong). The excitation is a pluck's shape at `shape` Hz (a
// step at the pick point, low-passed, with a trace of noise), or when 0 a burst of filtered noise
// combed at the pick point. `stiff` adds dispersion (upper partials run sharp), `damp` is the loop
// filter's averaging (0.5 the classic two-point average), `thiran` tunes the loop with an allpass.
// `body` adds low-pass resonances ([Hz, bandwidth]), `peaks` band-pass ones ([Hz, bandwidth, gain]).
typedef struct {
    double seconds, t60, pick, bright, stiff, cents, shape, damp;
    bool thiran;
    const double (*body)[2];
    int nbody;
    const double (*peaks)[3];
    int npeaks;
} gs_pluck_opts;
float *gs_pluck(double rate, gs_mulberry *rand, double note, const gs_pluck_opts *o, int *len);  // normalized to 0.5, malloc'd

// A bowed string: two delay lines meeting at the bow, whose hair grips and slips on a friction curve
// set by the bow's pressure. The callbacks give the bow's speed (its sign the direction), pressure
// (0..1) and a pitch offset in cents at time t; they are called in the order cents, velocity,
// pressure for every sample. The bow sits at `beta` of the string from the bridge; `pole` sets the
// loss at the bridge (lower is brighter). Returns the velocity at the bridge, malloc'd.
typedef struct {
    double (*velocity)(void *user, double t);
    double (*pressure)(void *user, double t);
    double (*cents)(void *user, double t);  // may be NULL
    void *user;
    double beta, pole;
} gs_bowed_opts;
float *gs_bowed(double rate, double hz, double seconds, const gs_bowed_opts *o, int *len);
