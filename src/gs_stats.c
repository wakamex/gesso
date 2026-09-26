#define _POSIX_C_SOURCE 200809L  // clock_gettime, sysconf
#include "gs_stats.h"

#include <stdio.h>

#include "gs_mix.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

uint64_t gs_process_ram(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
}

uint64_t gs_process_cpu_ns(void) {
    FILETIME create, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0;
    uint64_t k = (uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime;
    uint64_t u = (uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime;
    return (k + u) * 100;  // 100 ns units
}

#elif defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>

uint64_t gs_process_ram(void) { return emscripten_get_heap_size(); }  // the wasm heap
uint64_t gs_process_cpu_ns(void) { return 0; }

#elif defined(__APPLE__)
#include <mach/mach.h>
#include <time.h>

uint64_t gs_process_ram(void) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? info.resident_size : 0;
}

uint64_t gs_process_cpu_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

#else  // Linux and other POSIX systems with /proc
#include <time.h>
#include <unistd.h>

uint64_t gs_process_ram(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long size, resident;
    int ok = f && fscanf(f, "%lu %lu", &size, &resident) == 2;
    if (f) fclose(f);
    return ok ? (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
}

uint64_t gs_process_cpu_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) ? 0 : (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#endif

#define PERIOD_NS 500000000u

void gs_stats_frame_begin(gs_stats *s) {
    uint64_t now = SDL_GetTicksNS();
    if (!s->period_start) {
        s->period_start = now;
        s->cpu_start_ns = gs_process_cpu_ns();
        s->cpu_percent = s->audio_percent = -1;
        gs_mix_load();  // start the audio measurement period here too
    }
    s->frame_start = now;
    uint64_t span = now - s->period_start;
    if (span >= PERIOD_NS && s->frames) {
        uint64_t cpu = gs_process_cpu_ns();
        s->fps = s->frames * 1e9 / span;
        s->frame_ms = s->work_ns / 1e6 / s->frames;
        s->frame_max_ms = s->work_max_ns / 1e6;
        s->cpu_percent = cpu ? (cpu - s->cpu_start_ns) * 100.0 / span : -1;
        s->audio_percent = gs_mix_load() * 100;
        s->ram_bytes = gs_process_ram();
        s->period_start = now;
        s->cpu_start_ns = cpu;
        s->frames = 0;
        s->work_ns = s->work_max_ns = 0;
    }
}

void gs_stats_frame_end(gs_stats *s) {
    uint64_t work = SDL_GetTicksNS() - s->frame_start;
    s->work_ns += work;
    if (work > s->work_max_ns) s->work_max_ns = work;
    s->frames++;
}

void gs_stats_draw(const gs_stats *s, SDL_Renderer *r, float x, float y) {
    char lines[6][64];
    int n = 0;
    if (s->fps > 0) {
        snprintf(lines[n++], 64, "%.0f fps", s->fps);
        snprintf(lines[n++], 64, "frame %.2f ms (max %.2f)", s->frame_ms, s->frame_max_ms);
    } else {
        snprintf(lines[n++], 64, "measuring...");
    }
    if (s->cpu_percent >= 0) snprintf(lines[n++], 64, "cpu %.1f%% of a core", s->cpu_percent);
    if (s->audio_percent >= 0) snprintf(lines[n++], 64, "audio %.2f%% of real time", s->audio_percent);
    if (s->ram_bytes) snprintf(lines[n++], 64, "ram %.1f MB", s->ram_bytes / 1048576.0);
    const char *name = SDL_GetRendererName(r);
    snprintf(lines[n++], 64, "renderer %s", name ? name : "?");

    // Scale the 8-pixel font so it stays readable on large screens.
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
    float scale = oh >= 1400 ? 3 : oh >= 700 ? 2 : 1, sx, sy;
    SDL_GetRenderScale(r, &sx, &sy);
    SDL_SetRenderScale(r, scale, scale);
    const float ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE, pad = 4, lead = ch + 3;
    size_t widest = 0;
    for (int i = 0; i < n; i++) widest = SDL_strlen(lines[i]) > widest ? SDL_strlen(lines[i]) : widest;
    SDL_FRect box = { x / scale, y / scale, widest * ch + 2 * pad, n * lead - 3 + 2 * pad };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 235, 225, 200, 255);
    for (int i = 0; i < n; i++) SDL_RenderDebugText(r, box.x + pad, box.y + pad + i * lead, lines[i]);
    SDL_SetRenderScale(r, sx, sy);
}
