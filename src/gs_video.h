// H.264 video: access units in, decoded frames shown on an SDL texture when a clock reaches them.
// Hardware decoding is used where the system offers it, with frames handed to the renderer without a
// copy through the CPU: VAAPI through EGL on Linux (SDL's opengles2 renderer), Direct3D 11 on Windows
// (direct3d11), VideoToolbox on macOS (metal). Elsewhere, or when the device refuses the stream,
// libavcodec decodes in software. Needs gesso built with -Dvideo.
//
// One thread decodes (gs_video_decode, gs_video_flush) while the rendering thread shows frames
// (gs_video_frame); decoded frames wait in a short queue between them, and the decoder blocks while it is full.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gs_video gs_video;

// Call before creating the window. Picks the decoding path for this system, sets the SDL hints it needs,
// and returns the renderer to create (for SDL_CreateRenderer), or NULL for SDL's default. With
// `hardware` false, or where no hardware path is usable, video decodes in software on any renderer.
const char *gs_video_prepare(bool hardware);

// `queue` decoded frames may wait to be shown (at least 2). `threads` caps software decoding's
// threads, each of which holds a frame in flight (0: up to 4, fewer on small machines).
gs_video *gs_video_new(SDL_Renderer *r, int queue, int threads);
void gs_video_free(gs_video *v);

// Decodes one H.264 access unit in Annex B form, presented at `pts` seconds. False once stopped or
// after an error the decoder cannot recover from.
bool gs_video_decode(gs_video *v, const uint8_t *data, size_t len, double pts);
void gs_video_flush(gs_video *v);  // drops queued frames and the decoder's references (at a discontinuity)
void gs_video_stop(gs_video *v);   // unblocks and refuses the decoder; any thread

// On the rendering thread: shows the newest frame whose time has come at `clock` (seconds, on the
// timeline of the pts given to gs_video_decode), counting skipped ones as dropped, and returns the
// texture with the part of it holding the picture in *src. NULL before the first frame.
SDL_Texture *gs_video_frame(gs_video *v, double clock, SDL_FRect *src);

typedef struct {
    const char *path;      // "vaapi", "d3d11va", "videotoolbox" or "software"
    int width, height;     // of the last frame shown
    int queued;            // frames decoded and waiting
    long long decoded, shown, dropped;
    double next_pts;       // of the oldest waiting frame; < 0 when none waits
    // Over the last whole second: how far each new frame's time was from the clock when it was shown
    // (mean and largest, in ms; positive when shown late), and how far the time between new frames
    // strayed from the time between their timestamps (mean, in ms): the jitter a viewer sees.
    double error_ms, error_max_ms, jitter_ms;
} gs_video_info;

gs_video_info gs_video_get_info(gs_video *v);

const char *gs_video_library(void);  // the FFmpeg release decoding video, such as "9.0.2", for an about screen
