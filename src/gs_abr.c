#include "gs_abr.h"

#include <math.h>

#define FAST_HALF_LIFE 3.0
#define SLOW_HALF_LIFE 9.0
#define SAFETY 0.7        // the share of the estimate a rendition may use
#define UP_WAIT 15.0      // seconds after a switch before stepping up
#define UP_WAIT_MAX 120.0

static int fitting(const gs_abr *a, double estimate) {
    int level = 0;
    for (int i = 1; i < a->count; i++)
        if (a->bitrates[i] <= SAFETY * estimate) level = i;
    return level;
}

void gs_abr_init(gs_abr *a, const long long *bitrates, int count, double start) {
    *a = (gs_abr){ .count = count < 16 ? count : 16, .start = start, .up_wait = UP_WAIT, .last_switch = -INFINITY, .last_up = -INFINITY };
    for (int i = 0; i < a->count; i++) a->bitrates[i] = bitrates[i];
    a->level = fitting(a, start);
}

// An exponentially weighted average in which each sample counts for its weight in seconds. (By
// download time alone, as hls.js weighs, a low rendition's small, quick downloads barely move it,
// and recovering from a slow spell took half a minute; by media alone, falling is slow.)
static double blend(double average, double sample, double weight, double half_life) {
    double keep = pow(0.5, weight / half_life);
    return average * keep + sample * (1 - keep);
}

void gs_abr_sample(gs_abr *a, double bytes, double seconds, double media) {
    if (bytes <= 0 || seconds <= 0 || media <= 0) return;
    double rate = bytes * 8 / seconds, weight = seconds > media ? seconds : media;
    a->fast = blend(a->fast, rate, weight, FAST_HALF_LIFE);
    a->slow = blend(a->slow, rate, weight, SLOW_HALF_LIFE);
    a->weight += weight;
}

double gs_abr_estimate(const gs_abr *a) {
    if (a->weight <= 0) return a->start;
    // The averages start from zero; dividing by the weight they have gathered removes that pull.
    double fast = a->fast / (1 - pow(0.5, a->weight / FAST_HALF_LIFE));
    double slow = a->slow / (1 - pow(0.5, a->weight / SLOW_HALF_LIFE));
    return fast < slow ? fast : slow;
}

static void step_down(gs_abr *a, int level, double now) {
    if (now - a->last_up < a->up_wait) a->up_wait = fmin(a->up_wait * 2, UP_WAIT_MAX);  // the step up did not hold
    a->level = level, a->last_switch = now, a->switches++;
}

int gs_abr_next(gs_abr *a, double buffered, double segment, double now) {
    if (a->count < 2) return a->level;
    int fit = fitting(a, gs_abr_estimate(a));
    if (fit < a->level) {
        step_down(a, fit, now);
    } else if (fit > a->level && buffered >= 2 * segment && now - a->last_switch >= a->up_wait) {
        if (now - a->last_up >= 2 * a->up_wait) a->up_wait = UP_WAIT;  // the last step up held: back to the usual wait
        a->level++, a->last_switch = a->last_up = now, a->switches++;
    }
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

int gs_abr_give_up(gs_abr *a, double bytes, double seconds, double media, double now) {
    gs_abr_sample(a, bytes, seconds, media);
    int fit = fitting(a, gs_abr_estimate(a));
    if (a->level > 0) step_down(a, fit < a->level ? fit : a->level - 1, now);
    return a->level;
}
