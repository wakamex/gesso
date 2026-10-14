// gs_eta on made-up jobs: unit times of about 1.24 s that follow each other (an AR(1) series with
// coefficient 0.35, as training updates on a shared machine do), a pace that doubles, uneven steps
// timed by a reference.
#include <math.h>
#include <stdlib.h>

#include "gs_eta.h"
#include "test.h"

static unsigned seed;
static double noise(void) {  // about normal, mean 0 and deviation 1
    double s = 0;
    for (int i = 0; i < 12; i++) seed = seed * 1664525u + 1013904223u, s += (seed >> 8) / 16777216.0;
    return s - 6;
}

// A job of 1,500 units from time 0, reported one by one, whose pace becomes `later` times as slow
// from unit 750. Returns how often the shown finish moved after the first, and sets the true end and
// the shown finish at units 100 and 1,400.
static int replay(double later, double *end, double *at100, double *at1400) {
    gs_eta *e = gs_eta_new(1500, 0, (gs_eta_pace){ 1.5, 0.1, 1500 }, 20);
    double t = 0, x = 0, last = 0;
    int moves = -1;
    for (int u = 1; u <= 1500; u++) {
        x = 0.35 * x + 0.2 * noise();
        t += (1.24 + x) * (u > 750 ? later : 1);
        gs_eta_done(e, u, t);
        double shown = gs_eta_shown(e, 0);
        if (shown != last) moves++, last = shown;
        if (u == 100) *at100 = shown;
        if (u == 1400) *at1400 = shown;
    }
    *end = t;
    gs_eta_free(e);
    return moves;
}

void test_eta(void) {
    // Before `trust` reports the prior times the job; from then on its own pace.
    gs_eta *e = gs_eta_new(100, 10, (gs_eta_pace){ 2, 0.25, 50 }, 3);
    double end, se;
    CHECK(gs_eta_finish(e, &end, &se) && end == 210 && fabs(se - sqrt(100 * 0.25 + 100 * 100 * 0.25 / 50)) < 1e-9);
    gs_eta_done(e, 1, 11), gs_eta_done(e, 2, 12);
    CHECK(gs_eta_finish(e, &end, &se) && end == 12 + 98 * 2);
    gs_eta_done(e, 2, 13), gs_eta_done(e, 3, 11.5);  // (no units, and back in time: both ignored)
    gs_eta_done(e, 3, 13);
    CHECK(gs_eta_finish(e, &end, &se) && end == 13 + 97 * 1 && fabs(se - sqrt(97 * 1e-9 + 97 * 97 * 1e-9 / 3)) < 1e-12);  // (all three took 1 s: no variance)
    gs_eta_free(e);

    // The long-run variance is Newey-West's: for times 1, 2, 4, 3 (mean 2.5, two lags) it is
    // (2.25 + 0.25 + 2.25 + 0.25) / 4 + 2 * (2/3 * (0.75 - 0.75 + 0.75) / 4 + 1/3 * (-2.25 - 0.25) / 4) = 1.0833...
    e = gs_eta_new(10, 0, (gs_eta_pace){ 0 }, 1);
    CHECK(!gs_eta_finish(e, &end, &se) && gs_eta_shown(e, 0) == 0);  // (no prior, nothing reported)
    gs_eta_done(e, 1, 1), gs_eta_done(e, 2, 3), gs_eta_done(e, 3, 7), gs_eta_done(e, 4, 10);
    gs_eta_pace p = gs_eta_pace_of(e);
    CHECK(p.mean == 2.5 && p.units == 4 && fabs(p.long_run - (5.0 / 4 + 2 * (2.0 / 3 * 0.75 / 4 + 1.0 / 3 * -2.5 / 4))) < 1e-9);
    gs_eta_free(e);
    // Reports of two units at a time give the same pace as the units one by one would at a steady rate.
    e = gs_eta_new(10, 0, (gs_eta_pace){ 0 }, 1);
    gs_eta_done(e, 2, 3), gs_eta_done(e, 4, 6), gs_eta_done(e, 6, 9);
    CHECK(gs_eta_pace_of(e).mean == 1.5 && gs_eta_pace_of(e).units == 6);
    gs_eta_free(e);

    // A steady job: the shown finish settles near the end and moves a few times, on noise alone.
    double at100, at1400;
    for (int run = 0; run < 8; run++) {
        seed = 1 + (unsigned)run;
        int moves = replay(1, &end, &at100, &at1400);
        CHECK(moves <= 12);
        CHECK(fabs(at100 - end) < 150 && fabs(at1400 - end) < 30);
        if (moves > 12 || fabs(at100 - end) >= 150 || fabs(at1400 - end) >= 30) fprintf(stderr, "  run %d: %d moves, off by %.0f s at 100 and %.0f s at 1400\n", run, moves, at100 - end, at1400 - end);
    }
    // A job whose pace doubles halfway: no estimate foresees it, and the shown finish follows once it shows.
    seed = 99;
    int moves = replay(2, &end, &at100, &at1400);
    CHECK(moves >= 5 && at1400 > end - 120 && at100 < end - 600);

    // Uneven steps timed by a reference (a previous run took 40 s, 5 s, then six of 40 s): each step
    // done is its reference seconds in units, and this run, a quarter slower, finishes a quarter later.
    const double ref[8] = { 40, 5, 40, 40, 40, 40, 40, 40 };
    e = gs_eta_new(285, 100, (gs_eta_pace){ 1, 0, 285 }, 1);
    CHECK(gs_eta_shown(e, 20) == 385);
    double t = 100, done = 0, first = 0;
    for (int i = 0; i < 8; i++) {
        t += ref[i] * 1.25 * (i % 2 ? 1.05 : 0.95), done += ref[i];
        gs_eta_done(e, done, t);
        if (i == 0) first = gs_eta_shown(e, 20);
        else gs_eta_shown(e, 20);
    }
    CHECK(fabs(first - (100 + 285 * 1.25 * 0.95)) < 0.01);
    CHECK(fabs(gs_eta_shown(e, 20) - t) < 20);
    gs_eta_free(e);
}
