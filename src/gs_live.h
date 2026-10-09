// A live HLS player: a media playlist's segments fetched as they appear, demuxed (gs_ts or gs_mp4),
// decoded (gs_aac, and gs_video when asked for video) and played through gs_mix, with the video shown
// by the audio's clock. Every stream is put on one continuous timeline: at a discontinuity (an ad,
// say) the new segment's times are shifted to continue where the last ended, so the clock never jumps.
// It starts a few segments behind the live edge (at the start of a complete playlist), skips ahead if
// it falls out of the playlist, asks the app for a fresh playlist URL when the old one stops working,
// and rebuffers when the network stalls. Given a master playlist, it plays its video renditions and
// moves between them by the throughput it measures (gs_abr), at segment boundaries, giving up a
// segment's download for a lower rendition when it would arrive too late.
// The app opens the mixer (gs_mix_open) and sets the volume there. Needs gesso built with -Dvideo.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "gs_hls.h"
#include "gs_video.h"

typedef struct gs_live gs_live;

typedef struct {
    const char *url;               // a media playlist, played as it is, or a master playlist (adaptive)
    const char *agent;             // NULL for a default
    const char *const *headers;    // for playlist and segment requests, NULL-terminated, or NULL
    bool video;                    // false plays the audio alone
    SDL_Renderer *renderer;        // where video is shown (with video)
    double delay;                  // seconds behind the live edge to start at; 0 for about three segments
    double buffer;                 // seconds of media fetched ahead at most; 0 for 8
    double start_bandwidth;        // bits a second to assume before the first download (adaptive); 0 for 2.5 Mbit/s
    // The app's hooks, called from the player's threads; any may be NULL.
    bool (*renew)(void *user, char *url, size_t size);     // the playlist URL stopped working: write a fresh one (of the same kind), or false to give up
    void (*segment)(void *user, const gs_hls_segment *s);  // each new segment, with its tags, before it is fetched
    void (*changed)(void *user);                           // something to show changed (the state, a first picture)
    void *user;
} gs_live_config;

typedef enum { GS_LIVE_STARTING, GS_LIVE_PLAYING, GS_LIVE_BUFFERING, GS_LIVE_ENDED, GS_LIVE_FAILED } gs_live_state;

typedef struct {
    gs_live_state state;
    double clock;             // the presentation time heard now, on the player's timeline; < 0 before sound
    double buffered;          // seconds of decoded audio waiting
    double queued;            // seconds of compressed media fetched and waiting
    double behind;            // seconds behind the live edge
    int segments, discontinuities, skips, stalls, renewals, switches;
    double bandwidth;         // the throughput estimate in bits a second (adaptive); 0 for a media playlist
    char message[160];        // what went wrong, when something did
    gs_video_info video;      // with video
} gs_live_info;

gs_live *gs_live_start(const gs_live_config *c);  // NULL if it cannot start (no mixer, no decoder)
void gs_live_stop(gs_live *l);                     // stops and frees; waits for its threads
gs_live_info gs_live_get_info(gs_live *l);
// The rendition playing (adaptive): false for a media playlist, or before the master is read.
bool gs_live_rendition(gs_live *l, gs_hls_variant *out);
// With video, on the rendering thread each frame: the texture to draw and the part holding the
// picture, or NULL before the first picture.
SDL_Texture *gs_live_frame(gs_live *l, SDL_FRect *src);
