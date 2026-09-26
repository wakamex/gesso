#include "gs_sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void block(gs_sha256 *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (rotr(a[4], 6) ^ rotr(a[4], 11) ^ rotr(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (rotr(a[0], 2) ^ rotr(a[0], 13) ^ rotr(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, sizeof a[0] * 7);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}

void gs_sha256_init(gs_sha256 *s) {
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0, s->fill = 0;
}

void gs_sha256_update(gs_sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, k);
        s->fill += k, p += k, n -= k;
        if (s->fill == 64) block(s, s->buf), s->fill = 0;
    }
}

void gs_sha256_final(gs_sha256 *s, uint8_t digest[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    gs_sha256_update(s, &pad, 1);
    while (s->fill != 56) gs_sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    gs_sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) digest[4 * i + k] = (uint8_t)(s->h[i] >> (24 - 8 * k));
}

void gs_sha256_hex(const uint8_t digest[32], char hex[65]) {
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
}

bool gs_sha256_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    gs_sha256 s;
    gs_sha256_init(&s);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) gs_sha256_update(&s, buf, n);
    fclose(f);
    uint8_t d[32];
    gs_sha256_final(&s, d);
    gs_sha256_hex(d, hex);
    return true;
}
