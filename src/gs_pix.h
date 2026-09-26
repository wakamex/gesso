// Indexed-colour pictures in the VGA manner: a frame of palette indices, dithered once from a
// truecolour source, shown through a palette that can change every frame (colour cycling, fades).
#pragma once
#include <stdint.h>

typedef struct { uint8_t r, g, b; } gs_rgb;

// Index of the palette colour closest to (r, g, b), by a weighted RGB distance.
int gs_pix_nearest(const gs_rgb *pal, int n, int r, int g, int b);

// Floyd-Steinberg dithering of w*h RGB bytes onto the first n palette colours, serpentine, so the
// same input always gives the same indices.
void gs_pix_dither(const uint8_t *rgb, int w, int h, const gs_rgb *pal, int n, uint8_t *out);

// Writes RGBA bytes for w*h indices through the palette; pitch is bytes per output row.
void gs_pix_expand(const uint8_t *idx, int w, int h, const gs_rgb *pal, uint8_t *rgba, int pitch);

// A cycling ramp: palette entries [first, first + len) show `ramp` rotated by one entry every
// `seconds`, the classic way to animate fire, water or light without new frames.
typedef struct {
    int first, len;
    float seconds;
    gs_rgb ramp[16];
} gs_cycle;

void gs_pix_cycle(gs_rgb *pal, const gs_cycle *c, double t);

// For each of the first n colours, the index of that colour lightened by `amount` (0..1), for
// highlighting pixels without leaving the palette. Entries from n to 255 map to themselves.
void gs_pix_lighten_lut(const gs_rgb *pal, int n, float amount, uint8_t lut[256]);
