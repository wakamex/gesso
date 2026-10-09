// Choosing a stream's rendition by the throughput its downloads measure (adaptive bitrate), for
// segmented streams such as HLS. The policy alone, with no network: the player reports each
// download, asks which rendition the next segment should come from, and asks during a slow download
// whether to give it up for a lower rendition.
//
// The estimate is the lower of a fast and a slow moving average of download throughput (half-lives
// of 3 and 9 seconds), each download weighted by the longer of its time and its media's: a slow
// download counts for long, so the estimate falls quickly, and a low rendition's small, quick
// downloads still count for their media, so it recovers in a few segments. The next segment takes the best rendition
// whose bitrate fits in 70% of it: lower at once when the estimate drops, higher one step at a time,
// with two segments buffered and 15 s since the last switch (twice as long after a step up that
// had to be undone, up to 2 minutes).
#pragma once
#include <stdbool.h>

typedef struct {
    long long bitrates[16];  // bits a second, ascending
    int count, level;        // the renditions; the one in use
    double start;            // the estimate before any download, bits a second
    double fast, slow, weight;  // the moving averages (bits a second) and the seconds of weight behind them
    double last_switch, last_up, up_wait;  // seconds, on the caller's clock
    int switches;
} gs_abr;

// `bitrates` ascending (up to 16 are used). `start` is the throughput to assume before the first
// download, such as the last session's estimate; the first level is the best that fits it.
void gs_abr_init(gs_abr *a, const long long *bitrates, int count, double start);
// A download: `bytes` in `seconds` (from its first byte to its last, or to giving it up), bringing
// `media` seconds of the stream.
void gs_abr_sample(gs_abr *a, double bytes, double seconds, double media);
double gs_abr_estimate(const gs_abr *a);  // bits a second
// The level for the next segment, given the seconds of media buffered, a segment's length and the
// time now in seconds; a change counts as a switch.
int gs_abr_next(gs_abr *a, double buffered, double segment, double now);
// During a download: whether to give it up and fetch the segment from the rendition below instead.
// It is given up when it will not finish within the time left (what is buffered less a second, or
// one segment's length if that is more) and the rendition below would finish sooner.
// `expected` is the download's size in bytes, or <= 0 when unknown.
bool gs_abr_abandon(const gs_abr *a, double bytes, double expected, double elapsed, double buffered, double segment);
// After giving a download up: counts what arrived and returns the level to fetch the segment from,
// at least one below (a switch, like a step down from gs_abr_next).
int gs_abr_give_up(gs_abr *a, double bytes, double seconds, double media, double now);
