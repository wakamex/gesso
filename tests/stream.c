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
