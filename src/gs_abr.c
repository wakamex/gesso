#include "gs_abr.h"

#include <math.h>

#define SAFETY 0.7               // the share of the estimate a rendition may use (ExoPlayer's)
#define UP_BUFFERED 10.0         // seconds buffered before a step up (ExoPlayer's)
#define UP_LIVE_SHARE 0.75       // ...or this share of the time to the live edge, less a segment

static int fitting(const gs_abr *a, double estimate) {
    int level = 0;
    for (int i = 1; i < a->count; i++)
        if (a->bitrates[i] <= SAFETY * estimate) level = i;
    return level;
}

void gs_abr_init(gs_abr *a, const long long *bitrates, int count, double start) {
    *a = (gs_abr){ .count = count < 16 ? count : 16, .start = start };
    for (int i = 0; i < a->count; i++) a->bitrates[i] = bitrates[i];
    a->level = fitting(a, start);
}

void gs_abr_sample(gs_abr *a, double bytes, double seconds) {
    if (bytes <= 0 || seconds <= 0) return;
    a->samples[a->next_sample] = bytes * 8 / seconds;
    a->next_sample = (a->next_sample + 1) % 5;
    if (a->nsamples < 5) a->nsamples++;
}

double gs_abr_estimate(const gs_abr *a) {
    if (!a->nsamples) return a->start;
    double inverse = 0;
    for (int i = 0; i < a->nsamples; i++) inverse += 1 / a->samples[i];
    return a->nsamples / inverse;
}

int gs_abr_next(gs_abr *a, double buffered, double live, double segment) {
    if (a->count < 2) return a->level;
    int fit = fitting(a, gs_abr_estimate(a));
    double needed = live > 0 ? fmin(UP_BUFFERED, UP_LIVE_SHARE * (live - segment)) : UP_BUFFERED;
    if (fit < a->level || (fit > a->level && buffered >= needed)) a->level = fit, a->switches++;
    return a->level;
}

bool gs_abr_abandon(const gs_abr *a, double bytes, double expected, double elapsed, double flowing, double buffered, double segment) {
    if (a->level == 0 || elapsed < 0.5) return false;
    if (expected <= 0) expected = a->bitrates[a->level] / 8.0 * segment;  // (its rendition's rate for a segment's length)
    double budget = fmax(buffered - 1, segment);
    if (flowing < 0.25 || bytes <= 0) return elapsed > budget;  // too little flowing yet to judge the rate: only a deadline already missed
    double rate = bytes / flowing;  // bytes a second since the first byte
    double left = (expected - bytes) / rate;
    double lower = expected * (double)a->bitrates[a->level - 1] / (double)a->bitrates[a->level] / rate;
    return elapsed + left > budget && lower < left;
}

int gs_abr_give_up(gs_abr *a, double bytes, double seconds) {
    gs_abr_sample(a, bytes, seconds);
    int fit = fitting(a, gs_abr_estimate(a));
    if (a->level > 0) a->level = fit < a->level ? fit : a->level - 1, a->switches++;
    return a->level;
}
