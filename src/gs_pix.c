#include "gs_pix.h"

#include <stdlib.h>
#include <string.h>

int gs_pix_nearest(const gs_rgb *pal, int n, int r, int g, int b) {
    int best = 0, bestd = 0x7fffffff;
    for (int i = 0; i < n; i++) {
        int rm = (r + pal[i].r) / 2, dr = r - pal[i].r, dg = g - pal[i].g, db = b - pal[i].b;
        int d = (((512 + rm) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rm) * db * db) >> 8);
        if (d < bestd) bestd = d, best = i;
    }
    return best;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void gs_pix_dither(const uint8_t *rgb, int w, int h, const gs_rgb *pal, int n, uint8_t *out) {
    // Errors for this row and the next, with a spare column on each side.
    int *err = calloc((size_t)(w + 2) * 3 * 2, sizeof *err);
    int *cur = err, *next = err + (w + 2) * 3;
    for (int y = 0; y < h; y++) {
        int dir = y & 1 ? -1 : 1;
        for (int k = 0; k < w; k++) {
            int x = dir > 0 ? k : w - 1 - k;
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            int *e = cur + (x + 1) * 3;
            int r = clamp255(p[0] + e[0] / 16), g = clamp255(p[1] + e[1] / 16), b = clamp255(p[2] + e[2] / 16);
            int i = gs_pix_nearest(pal, n, r, g, b);
            out[(size_t)y * w + x] = (uint8_t)i;
            int d[3] = { r - pal[i].r, g - pal[i].g, b - pal[i].b };
            for (int c = 0; c < 3; c++) {
                cur[(x + 1 + dir) * 3 + c] += d[c] * 7;
                next[(x + 1 - dir) * 3 + c] += d[c] * 3;
                next[(x + 1) * 3 + c] += d[c] * 5;
                next[(x + 1 + dir) * 3 + c] += d[c];
            }
        }
        int *t = cur; cur = next; next = t;
        memset(next, 0, (size_t)(w + 2) * 3 * sizeof *next);
    }
    free(err);
}

void gs_pix_expand(const uint8_t *idx, int w, int h, const gs_rgb *pal, uint8_t *rgba, int pitch) {
    for (int y = 0; y < h; y++) {
        uint8_t *o = rgba + (size_t)y * pitch;
        const uint8_t *s = idx + (size_t)y * w;
        for (int x = 0; x < w; x++, o += 4) {
            gs_rgb c = pal[s[x]];
            o[0] = c.r, o[1] = c.g, o[2] = c.b, o[3] = 255;
        }
    }
}

void gs_pix_cycle(gs_rgb *pal, const gs_cycle *c, double t) {
    int step = c->seconds > 0 ? (int)(t / c->seconds) : 0;
    for (int i = 0; i < c->len; i++) pal[c->first + i] = c->ramp[(i + step) % c->len];
}

void gs_pix_lighten_lut(const gs_rgb *pal, int n, float amount, uint8_t lut[256]) {
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
    for (int i = 0; i < n; i++) {
        int r = pal[i].r + (int)((255 - pal[i].r) * amount);
        int g = pal[i].g + (int)((255 - pal[i].g) * amount);
        int b = pal[i].b + (int)((255 - pal[i].b) * amount);
        lut[i] = (uint8_t)gs_pix_nearest(pal, n, r, g, b);
    }
}
