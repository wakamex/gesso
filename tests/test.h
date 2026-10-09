// gesso's tests: one executable (zig build test) running each module's checks.
#pragma once
#include <math.h>
#include <stdio.h>

extern int test_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond), test_failures++; \
    } while (0)
#define CHECK_NEAR(a, b, eps) \
    do { \
        double a_ = (a), b_ = (b); \
        if (!(fabs(a_ - b_) <= (eps))) fprintf(stderr, "%s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, a_, b_), test_failures++; \
    } while (0)

// The tests, one per module (tests/<module>.c).
void test_abr(void);
void test_hls(void);
void test_http(void);
void test_json(void);
void test_live(void);
void test_media(void);
void test_oauth(void);
void test_secret(void);
void test_stream(void);
void test_stream_clock(void);
void test_text(void);
void test_ui(void);
