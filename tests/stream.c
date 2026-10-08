#include <SDL3/SDL.h>
#include <math.h>

#include "gs_stream.h"
#include "test.h"

void test_stream(void) {
    gs_stream *s = gs_stream_new(1000, 1.0, 0.1);  // 1000 frames a second, 100 frames of prebuffer
    float in[2 * 50], out[2 * 80] = { 0 };
    for (int i = 0; i < 100; i++) in[i] = (float)i;
    CHECK(gs_stream_write(s, in, 50));
    gs_stream_render(s, out, 80);  // under the prebuffer: silence
    CHECK(out[0] == 0 && gs_stream_position(s) == 0);
    CHECK(gs_stream_write(s, in, 50));
    gs_stream_render(s, out, 80);
    CHECK(out[0] == 0 && out[3] == 3 && out[2 * 79 + 1] == 2 * 29 + 1);  // frames 0..49, then 0..29 again
    CHECK_NEAR(gs_stream_position(s), 0.08, 1e-9);
    CHECK_NEAR(gs_stream_buffered(s), 0.02, 1e-9);
    gs_stream_end(s);
    float rest[2 * 40] = { 0 };
    gs_stream_render(s, rest, 40);  // 20 frames left, then silence
    CHECK(rest[2 * 19 + 1] == 2 * 49 + 1 && rest[2 * 20] == 0);
    CHECK(gs_stream_finished(s));
    gs_stream_stop(s);
    CHECK(!gs_stream_write(s, in, 1));
    gs_stream_free(s);
}

// The clock: timestamps, discontinuities, flushing. Offline there is no device latency, so what is
// heard is what was played, advancing with time between mixing calls up to what was mixed.
void test_stream_clock(void) {
    gs_stream *s = gs_stream_new(1000, 1.0, 0.0);
    float buf[2 * 200] = { 0 };
    CHECK(gs_stream_clock(s) < 0);  // nothing timestamped heard yet
    gs_stream_write_at(s, buf, 100, 10.0);
    gs_stream_write_at(s, buf, 100, 10.1);  // follows on: no new anchor
    gs_stream_render(s, buf, 50);           // heard: frame 0 at this call
    SDL_Delay(30);
    double c = gs_stream_clock(s);
    CHECK(c >= 10.02 && c <= 10.05);  // about 30 frames on, at most the 50 mixed
    SDL_Delay(60);
    CHECK(fabs(gs_stream_clock(s) - 10.05) < 1e-9);  // held at what was mixed
    gs_stream_render(s, buf, 150);  // frames 50 to 200
    gs_stream_write_at(s, buf, 100, 500.0);  // a discontinuity at frame 200
    gs_stream_render(s, buf, 100);  // heard at this call: frame 200
    CHECK(fabs(gs_stream_clock(s) - 500.0) < 0.002);
    gs_stream_write_at(s, buf, 100, 500.1);
    gs_stream_flush(s);  // drops frames 300 to 400; the next write follows the flushed one's place
    CHECK(gs_stream_buffered(s) == 0);
    gs_stream_write_at(s, buf, 10, 777.0);
    gs_stream_render(s, buf, 10);  // heard at this call: frame 300, the first after the flush
    double after = gs_stream_clock(s);
    CHECK(after >= 777.0 && after <= 777.0101);
    gs_stream_free(s);

    // Rebuffering: a stream that runs dry waits for its prebuffer again.
    s = gs_stream_new(1000, 1.0, 0.1);
    gs_stream_rebuffer(s, true);
    gs_stream_write(s, buf, 100);
    gs_stream_render(s, buf, 100);  // plays the 100, runs dry
    gs_stream_write(s, buf, 50);
    float out[2 * 50] = { 0 };
    out[0] = 0;
    gs_stream_render(s, out, 50);
    CHECK(gs_stream_position(s) == 0.1);  // waited: under the prebuffer
    gs_stream_free(s);
}
