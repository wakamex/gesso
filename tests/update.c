// gs_update against tests/update/server (made by tests/update/make.py), read through file: URLs: a
// folder holding build 1 of a made-up program is brought to build 2. (WinHTTP reads no file: URLs,
// so on Windows this checks only that a build of one's own does not look.)
#include <SDL3/SDL.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "gs_update.h"
#include "test.h"

static const uint8_t key[32] = { 0x59, 0x76, 0x27, 0xd3, 0x04, 0xb4, 0xc3, 0x61, 0x55, 0x1d, 0xb8, 0x56, 0x23, 0x82, 0xe1, 0xc8,
                                 0x94, 0x2a, 0x18, 0x97, 0x89, 0x35, 0x93, 0xfb, 0x6b, 0x8f, 0xd4, 0xc2, 0x66, 0x22, 0xca, 0x53 };

static void put(const char *dir, const char *name, const char *text) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    SDL_SaveFile(path, text, strlen(text));
}

static bool holds(const char *dir, const char *name, const char *text) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);
    size_t n = 0;
    char *got = SDL_LoadFile(path, &n);
    bool same = got && text && n == strlen(text) && !memcmp(got, text, n);
    SDL_free(got);
    return text ? same : !SDL_GetPathInfo(path, NULL);  // (NULL: that it is not there)
}

// Waits up to `ms` for the update to be in place.
static bool ready_within(gs_update *u, int ms) {
    for (Uint64 t = SDL_GetTicks(); SDL_GetTicks() - t < (Uint64)ms; SDL_Delay(10))
        if (gs_update_ready(u)) return true;
    return false;
}

void test_update(void) {
    gs_update_config c = { .build = NULL, .manifest = "file:///nowhere/manifest.txt", .key = key };
    CHECK(!gs_update_start(&c));  // (a build of one's own never looks)
#ifndef _WIN32
    char *cwd = SDL_GetCurrentDirectory(), dir[1024], url[1200], forged[1200];
    SDL_snprintf(dir, sizeof dir, "%sgesso-test-update-%d/", cwd, (int)SDL_GetTicks());
    SDL_snprintf(url, sizeof url, "file://%stests/update/server/manifest.txt", cwd);
    SDL_snprintf(forged, sizeof forged, "file://%stests/update/server/forged/manifest.txt", cwd);
    SDL_free(cwd);
    SDL_CreateDirectory(dir);
    put(dir, "prog", "#!/bin/sh\necho build 1\n"), put(dir, "data.txt", "data of build 1\n"), put(dir, "same.txt", "the same in both builds\n");
    put(dir, "left.old", "renamed aside by the last update\n");
    char prog[1200];
    SDL_snprintf(prog, sizeof prog, "%sprog", dir);
    chmod(prog, 0750);

    // A forged manifest is refused, and nothing changes.
    c = (gs_update_config){ .build = "1", .manifest = forged, .key = key, .folder = dir, .program = "prog", .agent = "test", .every_ms = 50 };
    gs_update *u = gs_update_start(&c);
    CHECK(!ready_within(u, 300) && holds(dir, "data.txt", "data of build 1\n") && holds(dir, "left.old", NULL));  // (the .old file went at the start)
    char error[300];
    gs_update_error(u, error, sizeof error);
    CHECK(strstr(error, "signature"));
    gs_update_free(u);
    // This build is the one published: nothing to do.
    c.manifest = url, c.build = "2";
    u = gs_update_start(&c);
    CHECK(!ready_within(u, 300) && holds(dir, "prog", "#!/bin/sh\necho build 1\n"));
    gs_update_free(u);
    // Build 1 is brought to build 2: the files that differ are fetched, from files/SHA256 and from
    // the place the manifest names, the old ones renamed aside, and the program keeps its mode.
    c.build = "1";
    u = gs_update_start(&c);
    CHECK(ready_within(u, 3000));
    gs_update_free(u);
    CHECK(holds(dir, "prog", "#!/bin/sh\necho build 2\n") && holds(dir, "prog.old", "#!/bin/sh\necho build 1\n"));
    CHECK(holds(dir, "data.txt", "data of build 2\n") && holds(dir, "data.txt.old", "data of build 1\n"));
    CHECK(holds(dir, "same.txt", "the same in both builds\n") && holds(dir, "same.txt.old", NULL));
    struct stat st;
    CHECK(!stat(prog, &st) && (st.st_mode & 0777) == 0750);
    // Its files all match build 2 now: whatever this build calls itself, nothing is started again.
    u = gs_update_start(&c);
    CHECK(!ready_within(u, 300) && holds(dir, "prog.old", NULL));
    gs_update_free(u);
    const char *names[] = { "prog", "data.txt", "same.txt", "prog.old", "data.txt.old" };
    for (int i = 0; i < 5; i++) {
        char path[1200];
        SDL_snprintf(path, sizeof path, "%s%s", dir, names[i]);
        SDL_RemovePath(path);
    }
    SDL_RemovePath(dir);
#endif
}
