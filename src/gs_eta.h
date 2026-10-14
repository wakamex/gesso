// The time left on a job made of countable units (updates, bytes, frames): an expected finish with
// its standard error, and a finish to show that moves only when the evidence says it is wrong. The
// policy alone, with no clock and no I/O: the app reports how many units are done at what time.
//
// The pace is the mean seconds a unit has taken over everything reported, which for a countdown
// beats a recent window: replaying 8 training runs of 1,500 updates, the shown finish was off by 80 s
// on average with the whole mean against 106 to 125 s with windows of 100 to 500 units, since a fast
// reaction to noise is what makes a countdown jump. Unit times drift and follow each other, so the
// variance of the time for k more units is k * L + k^2 * L / n, where n is the units measured and
// L their long-run variance per unit (Newey-West, Bartlett weights out to sqrt of the reports' count
// in lags); the shown finish moves when a new estimate is further from it than 2 standard errors of
// the time left (61 to 115 moves over those runs for other choices of L, 72 for this one, at the same
// accuracy). A pace that changes for good, as when other jobs take the processors, shows once the
// estimate moves past that bound.
//
// Reports may cover several units at once, or a fraction: the pace is then the seconds over the units
// of all the reports, and each report's departure from it is what the variance measures. Uneven steps
// timed by a reference, such as a previous run's step times, are units of reference seconds: report
// each finished step as its reference time in units, and the pace is this run's speed against it.
#pragma once
#include <stdbool.h>

typedef struct gs_eta gs_eta;

// A pace: seconds a unit, the long-run variance per unit, and the units it was measured over (0 for
// none). An earlier job's gs_eta_pace is the prior for the next one like it.
typedef struct { double mean, long_run, units; } gs_eta_pace;

// A job of `total` units, started at time `start` (seconds, in the clock the reports use). Its pace is
// `prior` until `trust` reports have arrived (20 is a good default; 1 for a few long steps), then its
// own.
gs_eta *gs_eta_new(double total, double start, gs_eta_pace prior, int trust);
void gs_eta_free(gs_eta *e);
// `done` units are done at time `t`; a report that does not add units or goes back in time is ignored.
void gs_eta_done(gs_eta *e, double done, double t);
gs_eta_pace gs_eta_pace_of(const gs_eta *e);  // this job's own pace, whatever `trust` says
// The seconds `units` more units take at a pace, and their standard error; false (both 0) for a pace
// measured over nothing. Phases with different paces add: the seconds sum, and the errors' squares.
bool gs_eta_time(gs_eta_pace p, double units, double *seconds, double *se);
// The expected finish (the last report's time plus the time left) and its standard error; false when
// there is no pace yet.
bool gs_eta_finish(const gs_eta *e, double *end, double *se);
// The finish to show: `*shown` is set to `end` when it is further from it than 2 standard errors or
// `least` seconds, whichever is more, or when it is not set (0). Returns true when it moved.
bool gs_eta_hold(double *shown, double end, double se, double least);
// The same for this job in one call, kept in it: the finish to count down to, or 0 when not known.
double gs_eta_shown(gs_eta *e, double least);
