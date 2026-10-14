#include "gs_eta.h"

#include <math.h>
#include <stdlib.h>

struct gs_eta {
    double total, start, last_done, last_t, shown;
    gs_eta_pace prior;
    int trust;
    double *seconds, *units;  // each report's
    int n, cap;
    double sum_seconds, sum_units;
};

gs_eta *gs_eta_new(double total, double start, gs_eta_pace prior, int trust) {
    gs_eta *e = calloc(1, sizeof *e);
    if (e) e->total = total, e->start = e->last_t = start, e->prior = prior, e->trust = trust < 1 ? 1 : trust;
    return e;
}

void gs_eta_free(gs_eta *e) {
    if (e) free(e->seconds), free(e->units), free(e);
}

void gs_eta_done(gs_eta *e, double done, double t) {
    if (done <= e->last_done || t < e->last_t) return;
    if (e->n == e->cap) {
        int cap = e->cap ? e->cap * 2 : 256;
        double *s = realloc(e->seconds, sizeof *s * (size_t)cap), *u = s ? realloc(e->units, sizeof *u * (size_t)cap) : NULL;
        if (s) e->seconds = s;
        if (!u) return;
        e->units = u, e->cap = cap;
    }
    e->seconds[e->n] = t - e->last_t, e->units[e->n] = done - e->last_done, e->n++;
    e->sum_seconds += t - e->last_t, e->sum_units += done - e->last_done;
    e->last_done = done, e->last_t = t;
}

gs_eta_pace gs_eta_pace_of(const gs_eta *e) {
    gs_eta_pace p = { 0, 0, 0 };
    if (!e->n || e->sum_units <= 0) return p;
    p.mean = e->sum_seconds / e->sum_units, p.units = e->sum_units;
    // Newey-West on each report's departure from the pace, per report, then per unit.
    int n = e->n, lags = (int)sqrt((double)n);
    if (lags > n - 1) lags = n - 1;
    double s = 0;
    for (int j = 0; j <= lags; j++) {
        double c = 0;
        for (int i = j; i < n; i++) c += (e->seconds[i] - p.mean * e->units[i]) * (e->seconds[i - j] - p.mean * e->units[i - j]);
        s += (j ? 2 * (1 - (double)j / (lags + 1)) : 1) * c / n;
    }
    p.long_run = fmax(s, 1e-9) / (e->sum_units / n);
    return p;
}

bool gs_eta_time(gs_eta_pace p, double units, double *seconds, double *se) {
    *seconds = *se = 0;
    if (p.units <= 0) return false;
    if (units > 0) *seconds = units * p.mean, *se = sqrt(units * p.long_run + units * units * p.long_run / p.units);
    return true;
}

bool gs_eta_finish(const gs_eta *e, double *end, double *se) {
    double left;
    if (!gs_eta_time(e->n >= e->trust ? gs_eta_pace_of(e) : e->prior, e->total - e->last_done, &left, se)) return *end = 0, false;
    *end = e->last_t + left;
    return true;
}

bool gs_eta_hold(double *shown, double end, double se, double least) {
    if (*shown && fabs(end - *shown) <= fmax(2 * se, least)) return false;
    *shown = end;
    return true;
}

double gs_eta_shown(gs_eta *e, double least) {
    double end, se;
    if (gs_eta_finish(e, &end, &se)) gs_eta_hold(&e->shown, end, se, least);
    return e->shown;
}
