// Choosing a stream's rendition by the throughput its downloads measure (adaptive bitrate), for
// segmented streams such as HLS. The policy alone, with no network: the player reports each
// download, asks which rendition the next segment should come from, and asks during a slow download
// whether to give it up for a lower rendition.
//
// The estimate is the harmonic mean of the last 5 downloads' throughput, as MPC (Yin et al.,
// SIGCOMM 2015) and Puffer's classical baseline predict: the mean suited to rates, pulled down hard
// by one slow download and little by one fast outlier. The choice is ExoPlayer's: the best rendition
// whose bitrate fits in 70% of the estimate, moved to directly, down at once and up once enough is
// buffered (10 s, or on a live stream 75% of the time to the live edge less one segment, if less).
#pragma once
#include <stdbool.h>

typedef struct {
    long long bitrates[16];  // bits a second, ascending
    int count, level;        // the renditions; the one in use
    double start;            // the estimate before any download, bits a second
    double samples[5];       // the last downloads' throughput, bits a second
    int nsamples, next_sample;
    int switches;
} gs_abr;

// `bitrates` ascending (up to 16 are used). `start` is the throughput to assume before the first
// download, such as the last session's estimate; the first level is the best that fits it.
void gs_abr_init(gs_abr *a, const long long *bitrates, int count, double start);
// A download: `bytes` in `seconds`, timed from its first byte to its last (or to giving it up).
void gs_abr_sample(gs_abr *a, double bytes, double seconds);
double gs_abr_estimate(const gs_abr *a);  // bits a second
// The level for the next segment, given the seconds of media buffered, the seconds from the play
// position to the live edge (<= 0 for a stream that is not live) and a segment's length; a change
// counts as a switch.
int gs_abr_next(gs_abr *a, double buffered, double live, double segment);
// During a download: whether to give it up and fetch the segment from the rendition below instead.
// It is given up when it will not finish within the time left (what is buffered less a second, or
// one segment's length if that is more) and the rendition below would finish sooner. `elapsed` is
// the time since the request and `flowing` the time since its first byte (0 before it), so a slow
// first byte does not pass for a slow link; `expected` is the size in bytes, or <= 0 when unknown.
bool gs_abr_abandon(const gs_abr *a, double bytes, double expected, double elapsed, double flowing, double buffered, double segment);
// After giving a download up: counts what arrived and returns the level to fetch the segment from,
// at least one below (a switch, like a step down from gs_abr_next).
int gs_abr_give_up(gs_abr *a, double bytes, double seconds);
