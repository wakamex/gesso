// Seeded randomness: a small generator for sequences and a hash for per-position noise.
// Everything that looks random in an app should come from here, so a seed reproduces it exactly.
#pragma once
#include <stdint.h>

typedef struct { uint64_t s; } gs_rng;

static inline gs_rng gs_rng_seed(uint64_t seed) { return (gs_rng){ seed * 0x9E3779B97F4A7C15ull + 1 }; }

// SplitMix64.
static inline uint64_t gs_rng_u64(gs_rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Uniform in [0, 1).
static inline float gs_rng_f(gs_rng *r) { return (float)(gs_rng_u64(r) >> 40) / (float)(1 << 24); }

// A well-mixed 32-bit hash of a grid position and a seed.
static inline uint32_t gs_hash2(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

// Smooth value noise in [0, 1) at a real position, from the hash at the surrounding grid points.
static inline float gs_noise2(float x, float y, uint32_t seed) {
    int xi = (int)(x >= 0 ? x : x - 1), yi = (int)(y >= 0 ? y : y - 1);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float s = 1.0f / 4294967296.0f;
    float a = gs_hash2(xi, yi, seed) * s, b = gs_hash2(xi + 1, yi, seed) * s;
    float c = gs_hash2(xi, yi + 1, seed) * s, d = gs_hash2(xi + 1, yi + 1, seed) * s;
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// Mulberry32, bit for bit the common JavaScript generator (32-bit state, results in [0, 1)), so
// seeded music and instruments ported from JavaScript play exactly the same.
typedef struct { uint32_t s; } gs_mulberry;

static inline double gs_mulberry_next(gs_mulberry *m) {
    m->s += 0x6d2b79f5u;
    uint32_t t = (m->s ^ (m->s >> 15)) * (1u | m->s);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}
