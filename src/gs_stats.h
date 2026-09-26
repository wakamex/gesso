// A performance overlay: frame rate, frame time, process memory and CPU, audio load and the
// renderer, drawn with SDL's built-in debug font. Call gs_stats_frame_begin at the start of a
// frame, gs_stats_frame_end once the frame is drawn but before presenting, then gs_stats_draw.
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

typedef struct {
    // Shown values, refreshed about twice a second.
    double fps;
    double frame_ms;     // average time to build a frame, not counting the wait for the display
    double frame_max_ms;  // slowest frame in the last period
    double cpu_percent;  // process CPU time over wall time, 100 = one full core; < 0 when unknown
    double audio_percent;  // time spent mixing over audio played; < 0 when no audio has played
    uint64_t ram_bytes;  // resident memory; 0 when unknown
    // Accumulators.
    uint64_t period_start, frame_start, work_ns, work_max_ns, cpu_start_ns;
    int frames;
} gs_stats;

void gs_stats_frame_begin(gs_stats *s);
void gs_stats_frame_end(gs_stats *s);
void gs_stats_draw(const gs_stats *s, SDL_Renderer *r, float x, float y);

// Process figures on their own, for logging: resident memory in bytes and CPU time in ns
// (0 when the platform does not report them).
uint64_t gs_process_ram(void);
uint64_t gs_process_cpu_ns(void);
