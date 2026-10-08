// Plays an H.264 Annex B file through gs_video in a window, looping it, and reports the decoding path,
// CPU, memory and dropped frames. For comparing decoding paths on a machine:
//   zig build bench-video -Dvideo -- FILE.h264 [--software] [--fps 60] [--seconds 30] [--log FILE] [--shot FILE.bmp]
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_pace.h"
#include "gs_stats.h"
#include "gs_video.h"

typedef struct {
    gs_video *video;
    const char *file;
    double fps;
    SDL_AtomicInt stop;
} feeder;

// Reads the file in blocks (so it does not count in the process's memory), splits it into access
// units with libavcodec's parser, and feeds them at their frame times, looping until stopped.
static int feed(void *user) {
    feeder *f = user;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    static uint8_t block[65536];
    long long n = 0;
    bool ok = true;
    while (ok && !SDL_GetAtomicInt(&f->stop)) {
        SDL_IOStream *io = SDL_IOFromFile(f->file, "rb");
        AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
        for (bool end = false; ok && !end && !SDL_GetAtomicInt(&f->stop);) {
            size_t got = SDL_ReadIO(io, block, sizeof block);
            end = got == 0;
            const uint8_t *p = block;
            do {
                uint8_t *au;
                int au_len;
                int used = av_parser_parse2(parser, ctx, &au, &au_len, end ? NULL : p, (int)got, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                p += used, got -= used;
                if (au_len) ok = gs_video_decode(f->video, au, au_len, n++ / f->fps);
            } while (ok && got);
        }
        av_parser_close(parser);
        SDL_CloseIO(io);
    }
    avcodec_free_context(&ctx);
    return 0;
}

int main(int argc, char **argv) {
    const char *file = NULL, *log_path = NULL, *shot = NULL;
    bool software = false;
    double fps = 60, seconds = 30;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--software")) software = true;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else file = argv[i];
    }
    SDL_PathInfo path_info;
    if (!file || !SDL_GetPathInfo(file, &path_info)) return fprintf(stderr, "usage: bench-video FILE.h264 [--software] [--fps N] [--seconds N] [--log FILE]\n"), 1;
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;

    const char *driver = gs_video_prepare(!software);
    SDL_Window *win = SDL_CreateWindow("bench-video", 1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, driver) : NULL;
    if (!r) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    gs_pace pace;
    gs_pace_set(&pace, win, r, true, GS_PACE_DISPLAY);
    feeder f = { .video = gs_video_new(r, 4, 0), .file = file, .fps = fps };
    if (!f.video) return fprintf(stderr, "gs_video_new failed\n"), 1;
    SDL_Thread *thread = SDL_CreateThread(feed, "feed", &f);

    FILE *log = log_path ? fopen(log_path, "w") : NULL;
    gs_stats stats = { 0 };
    uint64_t start = 0, cpu_start = 0, last_log = 0;
    double ram_sum = 0, ram_max = 0, cpu_sum = 0;
    int samples = 0;
    for (bool quit = false; !quit;) {
        for (SDL_Event e; SDL_PollEvent(&e);) quit |= e.type == SDL_EVENT_QUIT;
        gs_stats_frame_begin(&stats);
        uint64_t now = SDL_GetTicksNS();
        gs_video_info info = gs_video_get_info(f.video);
        if (!start && info.queued) start = now, cpu_start = gs_process_cpu_ns(), last_log = now;  // the clock starts at the first frame
        double clock = start ? (now - start) / 1e9 : -1;
        SDL_FRect src;
        SDL_Texture *t = gs_video_frame(f.video, clock, &src);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        if (t) SDL_RenderTexture(r, t, &src, NULL);
        char note[160];
        info = gs_video_get_info(f.video);
        SDL_snprintf(note, sizeof note, "path %s %dx%d\nshown %lld dropped %lld", info.path, info.width, info.height, info.shown, info.dropped);
        gs_stats_frame_end(&stats);
        if (shot && clock >= seconds) {  // the last frame, without the overlay
            SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
            if (!s || !SDL_SaveBMP(s, shot)) fprintf(stderr, "--shot: %s\n", SDL_GetError());
            SDL_DestroySurface(s);
            shot = NULL;
        }
        gs_stats_draw(&stats, r, 8, 8, note);
        SDL_RenderPresent(r);
        gs_pace_wait(&pace);
        // A sample a second, after the first two seconds of start-up.
        if (start && now - last_log >= 1000000000u) {
            last_log = now;
            if (clock > 2 && stats.cpu_percent >= 0) {
                double ram = gs_process_ram() / 1048576.0;
                ram_sum += ram, cpu_sum += stats.cpu_percent, samples++;
                if (ram > ram_max) ram_max = ram;
                if (log) fprintf(log, "%.0f s cpu %.1f%% ram %.1f MB shown %lld dropped %lld\n", clock, stats.cpu_percent, ram, info.shown, info.dropped), fflush(log);
            }
            quit |= clock >= seconds;
        }
    }
    gs_video_info info = gs_video_get_info(f.video);
    double wall = (SDL_GetTicksNS() - start) / 1e9;
    char summary[400];
    SDL_snprintf(summary, sizeof summary,
                 "file %s\npath %s, renderer %s, %dx%d\nseconds %.1f, shown %lld (%.1f fps), dropped %lld\ncpu %.1f%% of one core over the run (sampled average %.1f%%)\nmemory average %.1f MB, peak %.1f MB\n",
                 file, info.path, SDL_GetRendererName(r), info.width, info.height, wall, info.shown, info.shown / wall, info.dropped,
                 (gs_process_cpu_ns() - cpu_start) / 1e7 / wall, samples ? cpu_sum / samples : -1, samples ? ram_sum / samples : -1, ram_max);
    fputs(summary, stdout);
    if (log) fputs(summary, log), fclose(log);
    SDL_SetAtomicInt(&f.stop, 1);
    gs_video_stop(f.video);
    SDL_WaitThread(thread, NULL);
    gs_video_free(f.video);
    SDL_Quit();
    return 0;
}
