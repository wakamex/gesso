// Checking an Ed25519 signature (RFC 8032), for trusting what a server hands out, such as an update's
// manifest, by a public key built into the program. Only verification: no signing, no keys made.
//
// It accepts exactly what ed25519-dalek's verify_strict accepts: S below the group order, A and R
// not of small order, and [S]B - [k]A equal to R as encoded (no cofactor), where k is SHA-512 of R,
// A and the message. tests/ed25519 holds the cases it is checked on: RFC 8032's, signatures made by
// Python's cryptography package, and edge cases (small-order and mixed-order points, encodings that
// are not canonical, S at and above the order) judged by ed25519-dalek 3.0.0 itself.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool gs_ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t len);
