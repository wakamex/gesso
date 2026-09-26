#include "gs_dsp.h"

#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846

void gs_fft(double *re, double *im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = (inverse ? 2 : -2) * PI / len, wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr, im[b] = im[a] - ti;
                re[a] += tr, im[a] += ti;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr, cr = ncr;
            }
        }
    }
}

static void set(gs_biquad *f, double b0, double b1, double b2, double a0, double a1, double a2) {
    f->b0 = b0 / a0, f->b1 = b1 / a0, f->b2 = b2 / a0, f->a1 = a1 / a0, f->a2 = a2 / a0;
}

void gs_biquad_lowpass(gs_biquad *f, double rate, double hz, double q_db) {
    double w0 = 2 * PI * hz / rate, cw = cos(w0), alpha = sin(w0) / (2 * pow(10, q_db / 20));
    set(f, (1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

void gs_biquad_bandpass(gs_biquad *f, double rate, double hz, double q) {
    double w0 = 2 * PI * hz / rate, cw = cos(w0), alpha = sin(w0) / (2 * q);
    set(f, alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
}

void gs_biquad_peak(gs_biquad *f, double rate, double hz, double bw_hz) {
    double w = 2 * PI * hz / rate, alpha = sin(w) * bw_hz / (2 * hz);
    set(f, alpha, 0, -alpha, 1 + alpha, -2 * cos(w), 1 - alpha);
}

void gs_resonator_init(gs_resonator *r, double rate, double hz, double bw_hz) {
    r->c = -exp(-2 * PI * bw_hz / rate);
    r->b = 2 * exp(-PI * bw_hz / rate) * cos(2 * PI * hz / rate);
    r->a = 1 - r->b - r->c;
    r->y1 = r->y2 = 0;
}

void gs_normalize(float *d, int n, float peak) {
    float m = 0;
    for (int i = 0; i < n; i++) m = fmaxf(m, fabsf(d[i]));
    if (m > 0)
        for (int i = 0; i < n; i++) d[i] = (float)(d[i] * ((double)peak / m));
}

void gs_crossfade_loop(float *d, int start, int end, int fade) {
    for (int i = 0; i < fade; i++) {
        double t = (double)i / fade, a = cos(t * PI / 2), b = sin(t * PI / 2);
        d[end - fade + i] = (float)(d[end - fade + i] * a + d[start - fade + i] * b);
    }
}

static long wrap(long i, long size) { return ((i % size) + size) % size; }

float *gs_pluck(double sr, gs_mulberry *rand, double note, const gs_pluck_opts *o, int *len) {
    double f = gs_note_hz(note) * pow(2, o->cents / 1200), period = sr / f;
    int n = (int)floor(o->seconds * sr + 0.5);
    float *out = calloc((size_t)n + 1, sizeof *out);
    // Stiffness: a first-order allpass in the loop delays lows more than highs, so upper partials run
    // sharp; the loop is shortened by its phase delay at the fundamental to stay in tune.
    double c = -o->stiff, w0 = 2 * PI * f / sr;
    double ap_delay = -(atan2(-sin(w0), c + cos(w0)) - atan2(-c * sin(w0), 1 + c * cos(w0))) / w0;
    double delay = period - 0.5 - ap_delay;
    long size = (long)ceil(delay) + 4;
    float *line = calloc((size_t)size, sizeof *line);
    double rho = pow(0.001, 1 / (o->t60 * f));  // loss per period for the requested T60
    int nexc = (int)ceil(period);
    float *exc = calloc((size_t)nexc, sizeof *exc);
    double p = 0.95 - 0.6 * o->bright, lp = 0;
    if (o->shape) {
        double k = 1 - exp(-2 * PI * o->shape / sr), top = o->pick * nexc;
        for (int i = 0; i < nexc; i++) {
            double step = i < top ? 1 - o->pick : -o->pick;  // zero mean over the period
            lp += k * (step + 0.03 * (gs_mulberry_next(rand) * 2 - 1) - lp);
            exc[i] = (float)lp;
        }
        double mean = 0;
        for (int i = 0; i < nexc; i++) mean += exc[i] / (double)nexc;
        for (int i = 0; i < nexc; i++) exc[i] = (float)(exc[i] - mean);
    } else {
        for (int i = 0; i < nexc; i++) lp = (1 - p) * (gs_mulberry_next(rand) * 2 - 1) + p * lp, exc[i] = (float)lp;
        int gap = (int)floor(o->pick * period + 0.5);
        if (gap < 1) gap = 1;
        for (int i = nexc - 1; i >= gap; i--) exc[i] = exc[i] - exc[i - gap];
    }
    double prev = 0, x1 = 0, y1 = 0, tx = 0, ty = 0;
    long w = 0, whole = (long)floor(delay - 0.5);
    double eta = (1 - (delay - whole)) / (1 + (delay - whole));
    for (int i = 0; i < n; i++) {
        double z;
        if (o->thiran) {
            double xin = line[wrap(w - whole, size)];
            z = eta * xin + tx - eta * ty;
            tx = xin, ty = z;
        } else {
            double r = w - delay, ri = floor(r), fr = r - ri;
            double a = line[wrap((long)ri, size)], b = line[wrap((long)ri + 1, size)];
            z = a + (b - a) * fr;
        }
        double loss = (1 - o->damp) * z + o->damp * prev;
        prev = z;
        double ap = c * loss + x1 - c * y1;
        x1 = loss, y1 = ap;
        double y = (i < nexc ? exc[i] : 0) + rho * ap;
        line[w % size] = (float)y;
        w++;
        out[i] = (float)y;
    }
    if (o->nbody) {
        gs_resonator m[8];
        for (int k = 0; k < o->nbody && k < 8; k++) gs_resonator_init(&m[k], sr, o->body[k][0], o->body[k][1]);
        for (int i = 0; i < n; i++) {
            double s = 0.5 * out[i];
            for (int k = 0; k < o->nbody && k < 8; k++) s += gs_resonator_run(&m[k], out[i]) * 1.5;
            out[i] = (float)s;
        }
    }
    if (o->npeaks) {
        gs_biquad m[8];
        for (int k = 0; k < o->npeaks && k < 8; k++) gs_biquad_peak(&m[k], sr, o->peaks[k][0], o->peaks[k][1]), m[k].x1 = m[k].x2 = m[k].y1 = m[k].y2 = 0;
        for (int i = 0; i < n; i++) {
            double s = out[i];
            for (int k = 0; k < o->npeaks && k < 8; k++) s += o->peaks[k][2] * gs_biquad_run(&m[k], out[i]);
            out[i] = (float)s;
        }
    }
    int fade = (int)floor(0.05 * sr + 0.5);  // the tail fades so the note never ends abruptly
    for (int i = 0; i < fade && i < n; i++) out[n - 1 - i] = (float)(out[n - 1 - i] * ((double)i / fade));
    gs_normalize(out, n, 0.5f);
    free(line);
    free(exc);
    *len = n;
    return out;
}

static double read_line(const float *line, long size, long w, double d) {
    double r = w - d, i = floor(r), fr = r - i;
    double a = line[wrap((long)i, size)], b = line[wrap((long)i + 1, size)];
    return a + (b - a) * fr;
}

float *gs_bowed(double sr, double hz, double seconds, const gs_bowed_opts *o, int *len) {
    int n = (int)floor(seconds * sr + 0.5);
    float *out = calloc((size_t)n + 1, sizeof *out);
    long size = (long)ceil(sr / 40) + 8, w = 0;
    float *neck = calloc((size_t)size, sizeof *neck), *bridge = calloc((size_t)size, sizeof *bridge);
    double pole = o->pole, beta = o->beta ? o->beta : 0.18, lp = 0, loop = sr / hz - pole / (1 - pole);
    for (int i = 0; i < n; i++) {
        double t = i / sr, d = loop * pow(2, -(o->cents ? o->cents(o->user, t) : 0) / 1200);
        double neck_out = read_line(neck, size, w, d * (1 - beta)), bridge_out = read_line(bridge, size, w, d * beta);
        lp = 0.95 * (1 - pole) * bridge_out + pole * lp;
        double bridge_refl = -lp, nut_refl = -neck_out, string_vel = bridge_refl + nut_refl;
        double dv = o->velocity(o->user, t) - string_vel;
        double friction = fmin(1, pow(fabs(dv * (5 - 4 * o->pressure(o->user, t))) + 0.75, -4));  // the bow table
        double new_vel = dv * friction;
        neck[w % size] = (float)(bridge_refl + new_vel);
        bridge[w % size] = (float)(nut_refl + new_vel);
        w++;
        out[i] = (float)bridge_out;
    }
    free(neck);
    free(bridge);
    *len = n;
    return out;
}

double gs_note_hz(double note) { return 440 * pow(2, (note - 69) / 12); }
