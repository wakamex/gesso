// A performance overlay: frame rate, frame time, process memory and CPU, audio load, the size of
// the program's file, the renderer and the window's size in pixels, drawn with SDL's built-in debug font. Call gs_stats_frame_begin at the start of a
// frame, gs_stats_frame_end once the frame is drawn but before presenting, then gs_stats_draw.
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

#define GS_STATS_CPU_WINDOW 8  // periods of half a second

typedef struct {
    // Shown values, refreshed about twice a second.
    double fps;
    double frame_ms;     // average time to build a frame, not counting the wait for the display
    double frame_max_ms;  // slowest frame in the last period
    double cpu_percent;  // process CPU time over wall time in the last CPU_WINDOW periods, 100 = one core; < 0 when unknown
    double audio_percent;  // time spent mixing over audio played; < 0 when no audio has played
    uint64_t ram_bytes;  // resident memory; 0 when unknown
    // Accumulators.
    uint64_t period_start, frame_start, work_ns, work_max_ns;
    int frames;
    // CPU over a sliding window: process CPU time is coarse on some systems (15.6 ms ticks on
    // Windows, 3% of a half-second period), so it is averaged over several periods.
    uint64_t cpu_ns[GS_STATS_CPU_WINDOW + 1], cpu_at[GS_STATS_CPU_WINDOW + 1];
    int cpu_n;
} gs_stats;

void gs_stats_frame_begin(gs_stats *s);
void gs_stats_frame_end(gs_stats *s);
// `note` (may be NULL) adds lines (split at newlines), such as gs_pace_describe's. (x, y) is the box's top left;
// a negative x or y measures from the right or bottom edge instead. The box always stays on screen.
void gs_stats_draw(const gs_stats *s, SDL_Renderer *r, float x, float y, const char *note);

// Process figures on their own, for logging: resident memory in bytes and CPU time in ns
// (0 when the platform does not report them).
uint64_t gs_process_ram(void);
uint64_t gs_process_cpu_ns(void);
uint64_t gs_program_bytes(void);  // the running program's file (0 on the web, where there is none)
