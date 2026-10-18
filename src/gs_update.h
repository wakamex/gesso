// Keeping an installed program up to date by itself, from builds its publisher signs. Now and then
// it reads a manifest: the build it describes and the name, SHA-256 and size of each of its files,
// signed with the publisher's Ed25519 key (gs_ed25519). When the signature holds and the build is
// not this one, each file that differs from the one beside the program is downloaded and checked;
// once all have arrived, each running file is renamed aside to NAME.old (Windows lets a running
// program be renamed, not overwritten) and the new one put in its place. The app is then told (a
// callback, or gs_update_ready), and
// starts the new build when it chooses (gs_update_restart). The .old files go at the next start.
//
// The manifest may come over plain HTTP: the signature, made with a key that never leaves the
// publisher's machine, keeps a changed file out, whoever answers. Its lines:
//
//     build NAME
//     file NAME SHA256 SIZE [URL]
//
// A file's NAME is a file beside the program, never a path. Its URL is absolute, or relative to the
// manifest's (a release's assets beside its manifest are "gtube.exe"); without one it is
// files/SHA256 beside the manifest. The signature is the manifest's URL with ".sig" added: the 64
// bytes in hex, over the manifest's bytes as fetched. A build whose files all match already is
// this one, whatever it is called, and is not started again.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *build;     // this program's build, as manifests name them; NULL or "" for one that never updates itself (a build of one's own)
    const char *manifest;  // the manifest's URL
    const uint8_t *key;    // the publisher's public key, 32 bytes, built into the program
    const char *folder;    // where the program's files are, ending in a separator; NULL for SDL_GetBasePath()
    const char *program;   // the program's own file, made executable on Linux and macOS when it is new; other files keep the mode of the ones they replace
    const char *agent;     // the User-Agent, and what the log's lines begin with; NULL for "gesso"
    int every_ms;          // how often to look; 0 for once a minute
    // Called once, on the update's own thread, when a newer build is in place: for an app that sleeps
    // until an event comes (SDL_WaitEvent) to push one and wake. Or NULL, and the app asks gs_update_ready.
    void (*ready)(void *user);
    void *ready_user;
} gs_update_config;

typedef struct gs_update gs_update;

// Starts looking on a thread of its own, the first time at once; NULL for a build that does not
// update itself. The strings and key are copied.
gs_update *gs_update_start(const gs_update_config *c);
bool gs_update_ready(gs_update *u);  // a newer build is in place, to be started; then it stops looking
// What went wrong at the last look, or "" when nothing did; failures are logged once, not every look.
void gs_update_error(gs_update *u, char *error, size_t size);
void gs_update_free(gs_update *u);  // stops looking, and waits for a download under way to give up

// Starts `program` in `folder` (NULL for SDL_GetBasePath()) with argv[1] on, for the new build to
// take over as this one ends. It is not this process's child, so it outlives it; on Windows it starts
// without a console window. False, with SDL's error, when it could not be started.
bool gs_update_restart(const char *folder, const char *program, int argc, char **argv);
