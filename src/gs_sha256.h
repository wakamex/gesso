// SHA-256 (FIPS 180-4), for checking downloads against published checksums.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} gs_sha256;

void gs_sha256_init(gs_sha256 *s);
void gs_sha256_update(gs_sha256 *s, const void *data, size_t n);
void gs_sha256_final(gs_sha256 *s, uint8_t digest[32]);
void gs_sha256_hex(const uint8_t digest[32], char hex[65]);  // lowercase
bool gs_sha256_file(const char *path, char hex[65]);         // false if it cannot be read
