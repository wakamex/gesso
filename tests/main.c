#include <SDL3/SDL.h>

#include "test.h"

int test_failures;
const char *test_streams;  // tests/streams, from the command line

static const struct { const char *name; void (*run)(void); } tests[] = {
    { "gs_abr", test_abr },
    { "gs_hls", test_hls },
    { "gs_http", test_http },
    { "gs_json", test_json },
    { "gs_live", test_live },
    { "gs_media", test_media },
    { "gs_oauth", test_oauth },
    { "gs_secret", test_secret },
    { "gs_stream", test_stream },
    { "gs_stream clock", test_stream_clock },
    { "gs_text", test_text },
    { "gs_ui", test_ui },
};

// The streams folder as an absolute path, since tests build file:// URLs from it (zig build passes it
// relative to the working directory from Zig 0.17 on).
static const char *absolute(const char *path) {
    static char out[4096];
    if (path[0] == '/' || (path[0] && path[1] == ':')) return path;
    char *cwd = SDL_GetCurrentDirectory();  // (ends with a separator)
    if (!cwd) return path;
    SDL_snprintf(out, sizeof out, "%s%s", cwd, SDL_strncmp(path, "./", 2) ? path : path + 2);
    SDL_free(cwd);
    return out;
}

int main(int argc, char **argv) {
    if (argc > 1) test_streams = absolute(argv[1]);
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        int before = test_failures;
        tests[i].run();
        printf("%-12s %s\n", tests[i].name, test_failures == before ? "ok" : "FAILED");
    }
    return test_failures != 0;
}
