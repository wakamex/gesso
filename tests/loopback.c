// gs_loopback's queue, driven as a recording thread and a reader in step with a 10 ms microphone
// frame would drive it, with each sample its own index so the delay between them can be read off.
#include <SDL3/SDL.h>

#include "gs_loopback_queue.h"
#include "test.h"

enum { RATE = 16000, FRAME = 160, SLACK = RATE * GS_LOOPBACK_SLACK_MS / 1000 };

static float ring[RATE], made, out[FRAME];
static gs_loopback_queue q;

static void record(int n) {
    for (int i = 0; i < n; i++, made++) gs_loopback_put(&q, &made, 1);
}

// Samples between the last one recorded and the last one read: how far the reference lags.
static int lag(void) { return (int)(made - 1 - out[FRAME - 1]); }

void test_loopback(void) {
    q = (gs_loopback_queue){ .ring = ring, .cap = RATE, .rate = RATE };
    // What was recorded before the first read goes: the reader starts within the slack of the newest.
    record(RATE / 10);
    record(FRAME);
    CHECK(gs_loopback_take(&q, out, FRAME) == FRAME);
    CHECK(lag() <= SLACK);
    // A backlog that forms later (a late batch from the system) and never drains goes within a second.
    for (int step = 0; step < 100; step++) record(step == 10 ? FRAME + RATE / 10 : FRAME), gs_loopback_take(&q, out, FRAME);
    CHECK(lag() <= SLACK);
    // Uneven delivery, two frames every other step, reads whole frames throughout and keeps the lag.
    int short_reads = 0, most = 0;
    for (int step = 0; step < 300; step++) {
        record(step % 2 ? 0 : 2 * FRAME);
        short_reads += gs_loopback_take(&q, out, FRAME) < FRAME;
        most = SDL_max(most, lag());
    }
    CHECK(short_reads == 0 && most <= FRAME + SLACK);
}
