// gs_abr's choices for made-up downloads: a ladder like Twitch's (in bits a second), 2 s segments.
#include "gs_abr.h"
#include "test.h"

static const long long ladder[] = { 230000, 630000, 1330000, 3000000, 8000000 };  // 160p, 360p, 480p, 720p60, 1080p60

// One 2 s segment of the current level fetched at `rate` bits a second.
static void download(gs_abr *a, double rate) {
    double bytes = ladder[a->level] / 8.0 * 2;
    gs_abr_sample(a, bytes, bytes * 8 / rate, 2);
}

void test_abr(void) {
    gs_abr a;
    // The first level is the best within 70% of the starting estimate.
    gs_abr_init(&a, ladder, 5, 2.5e6);
    CHECK(a.level == 2);
    gs_abr_init(&a, ladder, 5, 20e6);
    CHECK(a.level == 4);
    gs_abr_init(&a, ladder, 5, 100e3);
    CHECK(a.level == 0);

    // A fast link: it steps up one rendition at a time, each after the wait, and only with two segments buffered.
    gs_abr_init(&a, ladder, 5, 2.5e6);
    double now = 0;
    for (int i = 0; i < 5; i++) download(&a, 50e6);
    CHECK(gs_abr_estimate(&a) > 40e6);
    CHECK(gs_abr_next(&a, 1.0, 2, now) == 2);  // too little buffered
    CHECK(gs_abr_next(&a, 6.0, 2, now) == 3);  // one step
    CHECK(gs_abr_next(&a, 6.0, 2, now + 5) == 3);  // not before the wait
    CHECK(gs_abr_next(&a, 6.0, 2, now + 15) == 4);

    // The link drops to 2 Mbit/s: it falls at once, past more than one rendition, and steps up again
    // only after twice the wait, since the last step up did not hold.
    now = 20;
    for (int i = 0; i < 4; i++) download(&a, 2e6);
    CHECK(gs_abr_estimate(&a) < 4e6);
    CHECK(gs_abr_next(&a, 6.0, 2, now) == 2);
    CHECK(a.up_wait == 30);
    for (int i = 0; i < 20; i++) download(&a, 50e6);
    CHECK(gs_abr_next(&a, 6.0, 2, now + 20) == 2);
    CHECK(gs_abr_next(&a, 6.0, 2, now + 30) == 3);

    // Back from a slow spell on the lowest rendition: its small downloads count for their media, so
    // two segments at 4 Mbit/s are enough to step up again.
    gs_abr_init(&a, ladder, 5, 2.5e6);
    for (int i = 0; i < 30; i++) download(&a, 0.8e6);
    CHECK(gs_abr_next(&a, 6.0, 2, 200) == 0);
    for (int i = 0; i < 2; i++) download(&a, 4e6);
    CHECK(gs_abr_next(&a, 6.0, 2, 230) == 1);

    // Falls fast, rises slowly: one slow download outweighs several fast ones.
    gs_abr_init(&a, ladder, 5, 2.5e6);
    for (int i = 0; i < 10; i++) download(&a, 10e6);
    double before = gs_abr_estimate(&a);
    download(&a, 1e6);
    CHECK(gs_abr_estimate(&a) < before * 0.8);

    // Giving up a download: hpbook's case, 1080p60 at 2.1 Mbit/s, where a 2 MB segment would take 7.6 s.
    gs_abr_init(&a, ladder, 5, 20e6);
    double rate = 2.1e6 / 8, expected = 2e6;
    CHECK(!gs_abr_abandon(&a, rate * 0.3, expected, 0.3, 0.3, 0, 2));  // too early to judge
    CHECK(gs_abr_abandon(&a, rate * 1.0, expected, 1.0, 1.0, 0, 2));   // will miss its 2 s and 720p60 is far quicker
    CHECK(!gs_abr_abandon(&a, rate * 1.0, expected, 1.0, 1.0, 12, 2)); // with 12 s buffered there is time
    CHECK(gs_abr_abandon(&a, rate * 1.0, 0, 1.0, 1.0, 0, 2));          // unknown size: judged from the bitrate
    // A slow first byte on a fast link (Twitch's first segment can take most of a second to start):
    // 0.6 s waiting, then 0.3 s at 20 Mbit/s is not a slow link.
    CHECK(!gs_abr_abandon(&a, 20e6 / 8 * 0.3, expected, 0.9, 0.3, 0, 2));
    CHECK(!gs_abr_abandon(&a, 0, expected, 0.9, 0, 0, 2));             // no byte yet, deadline not missed
    CHECK(gs_abr_abandon(&a, 0, expected, 2.5, 0, 0, 2));              // no byte by the deadline
    // Giving up drops at least one rendition, even when the average still looks good.
    for (int i = 0; i < 10; i++) download(&a, 50e6);
    CHECK(gs_abr_give_up(&a, 10000, 0.5, 0.1, 100) == 3);
    a.level = 0;
    CHECK(!gs_abr_abandon(&a, 1, expected, 5.0, 5.0, 0, 2));      // nothing lower to go to
    CHECK(gs_abr_give_up(&a, 1, 5.0, 0.1, 101) == 0);

    // One rendition: nothing to choose.
    gs_abr_init(&a, ladder, 1, 1e6);
    download(&a, 100e6);
    CHECK(gs_abr_next(&a, 10, 2, 100) == 0);
}
