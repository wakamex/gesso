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

static SDL_AtomicInt told;
static void tell(void *user) { SDL_AddAtomicInt(user, 1); }

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

    char state[1200];
    SDL_snprintf(state, sizeof state, "%supdate.txt", dir);
    char error[300];

    // Until its user has said yes, a test build does not look.
    c = (gs_update_config){ .build = "1", .manifest = url, .key = key, .folder = dir, .program = "prog", .agent = "test", .every_ms = 50, .state = state, .idle_ms = 50 };
    gs_update *u = gs_update_start(&c);
    CHECK(gs_update_consent(u) == GS_UPDATE_ASK && !ready_within(u, 300) && holds(dir, "data.txt", "data of build 1\n") && holds(dir, "left.old", NULL));  // (the .old file went at the start)
    gs_update_set_consent(u, false);
    gs_update_free(u);
    u = gs_update_start(&c);
    CHECK(gs_update_consent(u) == GS_UPDATE_NO && !ready_within(u, 300));  // (the answer is kept)
    gs_update_set_consent(u, true);
    gs_update_free(u);

    // A forged manifest is refused, and nothing changes.
    c.manifest = forged;
    u = gs_update_start(&c);
    CHECK(gs_update_consent(u) == GS_UPDATE_YES && !ready_within(u, 300) && holds(dir, "data.txt", "data of build 1\n"));
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
    c.build = "1", c.ready = tell, c.ready_user = &told;
    u = gs_update_start(&c);
    CHECK(ready_within(u, 3000));
    // It is due only when the app is not busy, input has stopped for idle_ms, and no restart was lately.
    SDL_Delay(60);
    CHECK(!gs_update_due(u, true, 0) && !gs_update_due(u, false, SDL_GetTicks()) && gs_update_due(u, false, 0));
    gs_update_free(u);
    CHECK(SDL_GetAtomicInt(&told) == 1);  // (the app was told, once)
    CHECK(holds(dir, "prog", "#!/bin/sh\necho build 2\n") && holds(dir, "prog.old", "#!/bin/sh\necho build 1\n"));
    CHECK(holds(dir, "data.txt", "data of build 2\n") && holds(dir, "data.txt.old", "data of build 1\n"));
    CHECK(holds(dir, "same.txt", "the same in both builds\n") && holds(dir, "same.txt.old", NULL));
    struct stat st;
    CHECK(!stat(prog, &st) && (st.st_mode & 0777) == 0750);

    // Build 2 on trial: it keeps the files it replaced, and a clean exit does not count against it.
    c.build = "2", c.ready = NULL, c.trial_ms = 60000;
    u = gs_update_start(&c);
    CHECK(!gs_update_ready(u) && holds(dir, "prog.old", "#!/bin/sh\necho build 1\n"));
    gs_update_free(u);
    char *kept = SDL_LoadFile(state, NULL);
    CHECK(kept && strstr(kept, "trial 2\n") && strstr(kept, "failures 0\n") && strstr(kept, "running \n"));
    SDL_free(kept);
    // Ended uncleanly twice already, and a third time: the next start goes back to build 1.
    const char *crashed = "consent yes\ntrial 2\nfailures 2\nrunning 2\nskip \nrestarted 0\n";
    SDL_SaveFile(state, crashed, strlen(crashed));
    u = gs_update_start(&c);
    CHECK(gs_update_rolled_back(u) && gs_update_ready(u) && gs_update_due(u, false, 0));
    CHECK(holds(dir, "prog", "#!/bin/sh\necho build 1\n") && holds(dir, "prog.bad", "#!/bin/sh\necho build 2\n") && holds(dir, "data.txt", "data of build 1\n"));
    gs_update_free(u);
    // Build 1 again, which does not take build 2 again until a newer one is published.
    c.build = "1", c.trial_ms = 0;
    u = gs_update_start(&c);
    CHECK(!ready_within(u, 400) && holds(dir, "prog", "#!/bin/sh\necho build 1\n") && holds(dir, "prog.bad", NULL));
    gs_update_error(u, error, sizeof error);
    CHECK(strstr(error, "gone back from"));
    // A restart for an update starts the program with --restarted, and the next is not due within gap_ms.
    char *args[] = { "prog", "--keep", NULL };
    CHECK(gs_update_restart_as(u, 2, args));
    gs_update_free(u);
    const char *cleared = "consent yes\ntrial \nfailures 0\nrunning \nskip \nrestarted 0\n";
    SDL_SaveFile(state, cleared, strlen(cleared));
    u = gs_update_start(&c);
    CHECK(ready_within(u, 3000));  // (build 2 once more, now that nothing is skipped)
    CHECK(gs_update_due(u, false, 0));
    CHECK(gs_update_restart_as(u, 2, args) && !gs_update_due(u, false, 0));
    gs_update_free(u);
    // Its trial passes after trial_ms, and the files it replaced go.
    c.build = "2", c.trial_ms = 100;
    u = gs_update_start(&c);
    for (Uint64 t = SDL_GetTicks(); SDL_GetTicks() - t < 2000 && !holds(dir, "prog.old", NULL);) SDL_Delay(20);
    CHECK(holds(dir, "prog.old", NULL) && holds(dir, "data.txt.old", NULL));
    gs_update_free(u);
    char *restarted_args[] = { "prog", "--restarted", NULL }, *plain_args[] = { "prog", NULL };
    CHECK(gs_update_restarted(2, restarted_args) && !gs_update_restarted(1, plain_args));
    SDL_RemovePath(state);
    const char *names[] = { "prog", "data.txt", "same.txt", "prog.old", "data.txt.old", "prog.bad", "data.txt.bad" };
    for (int i = 0; i < 7; i++) {
        char path[1200];
        SDL_snprintf(path, sizeof path, "%s%s", dir, names[i]);
        SDL_RemovePath(path);
    }
    SDL_RemovePath(dir);
#endif
}
