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

// An exponentially weighted average by download time, as hls.js keeps: each sample counts for
// its seconds, so a long download weighs more than a short one.
static double blend(double average, double sample, double seconds, double half_life) {
    double keep = pow(0.5, seconds / half_life);
    return average * keep + sample * (1 - keep);
}

void gs_abr_sample(gs_abr *a, double bytes, double seconds) {
    if (bytes <= 0 || seconds <= 0) return;
    double rate = bytes * 8 / seconds;
    a->fast = blend(a->fast, rate, seconds, FAST_HALF_LIFE);
    a->slow = blend(a->slow, rate, seconds, SLOW_HALF_LIFE);
    a->weight += seconds;
}

double gs_abr_estimate(const gs_abr *a) {
    if (a->weight <= 0) return a->start;
    // The averages start from zero; dividing by the weight they have gathered removes that pull.
    double fast = a->fast / (1 - pow(0.5, a->weight / FAST_HALF_LIFE));
    double slow = a->slow / (1 - pow(0.5, a->weight / SLOW_HALF_LIFE));
    return fast < slow ? fast : slow;
}

int gs_abr_next(gs_abr *a, double buffered, double segment, double now) {
    if (a->count < 2) return a->level;
    int fit = fitting(a, gs_abr_estimate(a));
    if (fit < a->level) {
        if (now - a->last_up < a->up_wait) a->up_wait = fmin(a->up_wait * 2, UP_WAIT_MAX);  // the step up did not hold
        a->level = fit, a->last_switch = now, a->switches++;
    } else if (fit > a->level && buffered >= 2 * segment && now - a->last_switch >= a->up_wait) {
        if (now - a->last_up >= 2 * a->up_wait) a->up_wait = UP_WAIT;  // the last step up held: back to the usual wait
        a->level++, a->last_switch = a->last_up = now, a->switches++;
    }
    return a->level;
}

bool gs_abr_abandon(const gs_abr *a, double bytes, double expected, double elapsed, double buffered, double segment) {
    if (a->level == 0 || elapsed < 0.5) return false;
    if (expected <= 0) expected = a->bitrates[a->level] / 8.0 * segment;  // (its rendition's rate for a segment's length)
    double rate = bytes / elapsed;  // bytes a second so far
    if (rate <= 0) return elapsed > fmax(buffered - 1, segment);
    double left = (expected - bytes) / rate;
    double budget = fmax(buffered - 1, segment);
    double lower = expected * (double)a->bitrates[a->level - 1] / (double)a->bitrates[a->level] / rate;
    return elapsed + left > budget && lower < left;
}
