#include "gs_update.h"

#include <SDL3/SDL.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "gs_ed25519.h"
#include "gs_http.h"
#include "gs_sha256.h"

enum { MAX_FILES = 32 };

typedef struct {
    char name[64], hash[65], url[512];
    long long size;
} file;

// What is kept between runs, in the state file: the user's answer, the build on trial and how often it
// ended uncleanly, the build running now (cleared on a clean exit), a build gone back from, and when
// the last update restarted the program.
typedef struct {
    int consent;
    char trial[128], running[128], skip[128];
    int failures;
    long long restarted;  // seconds since 1970
} kept;

struct gs_update {
    char build[128], manifest[512], folder[1024], program[64], agent[64], state[1100];
    int idle_ms, gap_ms, trial_ms;
    kept k;
    bool rolled_back;
    bool handed_over;  // gs_update_restart_as started the next build: the state file is its now
    uint8_t key[32];
    int every_ms;
    void (*ready_fn)(void *user);
    void *ready_user;
    SDL_Semaphore *wake;
    SDL_AtomicInt stopping, ready;
    SDL_Mutex *lock;
    char error[300];
    SDL_Thread *thread;
};

static bool unhex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < 2 * n; i++) {
        char c = hex[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] << 4 | d : d);
    }
    return true;
}

// A URL the manifest gives, made whole: absolute as it is, else relative to the manifest's.
static void resolve(const gs_update *u, const char *url, char *out, size_t size) {
    if (strstr(url, "://")) { SDL_strlcpy(out, url, size); return; }
    size_t base = strcspn(u->manifest, "?#");
    while (base && u->manifest[base - 1] != '/') base--;
    SDL_snprintf(out, size, "%.*s%s", (int)base, u->manifest, url);
}

// Reads a manifest, given its signature in hex: its build and files, or false with the reason.
static bool read_manifest(const gs_update *u, const char *text, size_t len, const char *signature, char *build, size_t build_size, file *files, int *count, char *error, size_t error_size) {
    uint8_t sig[64];
    while (*signature == ' ' || *signature == '\n' || *signature == '\r') signature++;
    if (strspn(signature, "0123456789abcdefABCDEF") != 128 || !unhex(signature, sig, 64)) return SDL_strlcpy(error, "the signature is not 64 bytes in hex", error_size), false;
    if (!gs_ed25519_verify(sig, u->key, text, len)) return SDL_strlcpy(error, "the signature does not match the manifest", error_size), false;
    build[0] = 0, *count = 0;
    for (const char *line = text; line < text + len;) {
        const char *end = memchr(line, '\n', (size_t)(text + len - line));
        if (!end) end = text + len;
        char l[1024], name[256], hash[256], url[600] = "";
        long long size;
        SDL_strlcpy(l, line, SDL_min((size_t)(end - line) + 1, sizeof l));
        l[strcspn(l, "\r")] = 0;
        int fields = SDL_sscanf(l, "file %255s %255s %lld %599s", name, hash, &size, url);
        if (!SDL_strncmp(l, "build ", 6)) SDL_strlcpy(build, l + 6, build_size);
        else if (fields >= 3) {
            // (a name is a file beside the program, never a path elsewhere)
            if (!name[0] || strpbrk(name, "/\\:") || name[0] == '.' || SDL_strlen(name) >= sizeof files->name || SDL_strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 || size < 0)
                return SDL_snprintf(error, error_size, "the manifest names a file it may not: %s", name), false;
            if (*count == MAX_FILES) return SDL_strlcpy(error, "the manifest names too many files", error_size), false;
            file *f = &files[(*count)++];
            SDL_strlcpy(f->name, name, sizeof f->name), SDL_strlcpy(f->hash, hash, sizeof f->hash), f->size = size;
            if (fields < 4) SDL_snprintf(url, sizeof url, "files/%s", hash);
            resolve(u, url, f->url, sizeof f->url);
        }
        line = end + 1;
    }
    if (!build[0]) return SDL_strlcpy(error, "the manifest names no build", error_size), false;
    return true;
}

static void file_hash(const char *path, char hex[65]) {
    hex[0] = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    uint8_t *buf = io ? SDL_malloc(1 << 16) : NULL;
    if (!buf) { if (io) SDL_CloseIO(io); return; }
    gs_sha256 h;
    gs_sha256_init(&h);
    for (size_t n; (n = SDL_ReadIO(io, buf, 1 << 16)) > 0;) gs_sha256_update(&h, buf, n);
    SDL_CloseIO(io), SDL_free(buf);
    uint8_t digest[32];
    gs_sha256_final(&h, digest);
    gs_sha256_hex(digest, hex);
}

typedef struct {
    SDL_IOStream *io;
    gs_sha256 hash;
    long long have, size;
    bool failed;
} sink;

static bool take(void *user, const uint8_t *data, size_t len) {
    sink *s = user;
    if (s->have + (long long)len > s->size || SDL_WriteIO(s->io, data, len) != len) return s->failed = true, false;  // (no more than the manifest says)
    gs_sha256_update(&s->hash, data, len);
    s->have += (long long)len;
    return true;
}

// Downloads one file of the build to `to`, checked against the manifest.
static bool fetch(gs_update *u, const file *f, const char *to, char *error, size_t error_size) {
    sink s = { .io = SDL_IOFromFile(to, "wb"), .size = f->size };
    if (!s.io) return SDL_snprintf(error, error_size, "%s: %s", to, SDL_GetError()), false;
    gs_sha256_init(&s.hash);
    gs_http_request r = { .url = f->url, .agent = u->agent, .follow = true };
    int status = gs_http_stream(&r, 0, take, &s, &u->stopping);
    SDL_CloseIO(s.io);
    uint8_t digest[32];
    char hex[65];
    gs_sha256_final(&s.hash, digest);
    gs_sha256_hex(digest, hex);
    if (status == 200 && !s.failed && s.have == f->size && !SDL_strcmp(hex, f->hash)) return true;
    SDL_RemovePath(to);
    if (status != 200) SDL_snprintf(error, error_size, "%s: HTTP %d", f->name, status);
    else SDL_snprintf(error, error_size, "%s did not arrive as the manifest describes it", f->name);
    return false;
}

static void save(const gs_update *u) {
    if (u->handed_over) return;  // (the next run may already have written its own)
    char text[700];
    int n = SDL_snprintf(text, sizeof text, "consent %s\ntrial %s\nfailures %d\nrunning %s\nskip %s\nrestarted %lld\n",
                         u->k.consent == GS_UPDATE_YES ? "yes" : u->k.consent == GS_UPDATE_NO ? "no" : "ask", u->k.trial, u->k.failures, u->k.running, u->k.skip, u->k.restarted);
    if (u->state[0]) SDL_SaveFile(u->state, text, (size_t)n);
}

static void load(gs_update *u) {
    char *text = u->state[0] ? SDL_LoadFile(u->state, NULL) : NULL;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *value = strchr(line, ' ');
        if (!value) continue;
        *value++ = 0;
        value[strcspn(value, "\r")] = 0;  // (a file whose lines end as Windows' do)
        if (!strcmp(line, "consent")) u->k.consent = !strcmp(value, "yes") ? GS_UPDATE_YES : !strcmp(value, "no") ? GS_UPDATE_NO : GS_UPDATE_ASK;
        else if (!strcmp(line, "trial")) SDL_strlcpy(u->k.trial, value, sizeof u->k.trial);
        else if (!strcmp(line, "running")) SDL_strlcpy(u->k.running, value, sizeof u->k.running);
        else if (!strcmp(line, "skip")) SDL_strlcpy(u->k.skip, value, sizeof u->k.skip);
        else if (!strcmp(line, "failures")) u->k.failures = SDL_atoi(value);
        else if (!strcmp(line, "restarted")) u->k.restarted = SDL_strtoll(value, NULL, 10);
    }
    SDL_free(text);
}

// The files in the program's folder ending in `suffix`, removed.
static void sweep(const gs_update *u, const char *suffix) {
    int count = 0;
    char pattern[16], **found;
    SDL_snprintf(pattern, sizeof pattern, "*%s", suffix);
    found = SDL_GlobDirectory(u->folder, pattern, 0, &count);
    for (int i = 0; found && i < count; i++) {
        char path[1200];
        SDL_snprintf(path, sizeof path, "%s%s", u->folder, found[i]);
        SDL_RemovePath(path);
    }
    SDL_free(found);
}

// Back to the build before this one: each file the update replaced is put aside as NAME.bad and the
// one it replaced (NAME.old) put back, and the build is not taken again.
static void roll_back(gs_update *u) {
    int count = 0;
    char **old = SDL_GlobDirectory(u->folder, "*.old", 0, &count);
    for (int i = 0; old && i < count; i++) {
        char from[1200], here[1200], bad[1210];
        SDL_snprintf(from, sizeof from, "%s%s", u->folder, old[i]);
        SDL_snprintf(here, sizeof here, "%s%.*s", u->folder, (int)strlen(old[i]) - 4, old[i]);
        SDL_snprintf(bad, sizeof bad, "%s.bad", here);
        SDL_RemovePath(bad);
        SDL_RenamePath(here, bad), SDL_RenamePath(from, here);
    }
    SDL_free(old);
    SDL_Log("%s: build %s ended uncleanly %d times on trial; the build before it is back", u->agent, u->build, u->k.failures);
    SDL_strlcpy(u->k.skip, u->build, sizeof u->k.skip), u->k.trial[0] = 0, u->k.failures = 0;
    u->rolled_back = true;
}

// Looks once. True when a newer build has been put in place.
static bool look(gs_update *u, char *error, size_t error_size) {
    char url[600], *text = NULL, *signature = NULL;
    size_t len = 0, sig_len = 0;
    gs_http_request r = { .url = u->manifest, .agent = u->agent, .follow = true };
    int status = gs_http_fetch(&r, &text, &len);
    size_t q = strcspn(u->manifest, "?#");
    SDL_snprintf(url, sizeof url, "%.*s.sig%s", (int)q, u->manifest, u->manifest + q);
    r.url = url;
    int sig_status = status == 200 ? gs_http_fetch(&r, &signature, &sig_len) : 0;
    char build[128];
    file files[MAX_FILES];
    int count = 0;
    bool ok = status == 200 && sig_status == 200;
    if (!ok) SDL_snprintf(error, error_size, "the update server answered %d", status == 200 ? sig_status : status);
    else ok = read_manifest(u, text, len, signature, build, sizeof build, files, &count, error, error_size);
    SDL_free(text), SDL_free(signature);
    if (!ok || !SDL_strcmp(build, u->build)) return false;
    if (!SDL_strcmp(build, u->k.skip)) return SDL_snprintf(error, error_size, "build %s was gone back from; waiting for a newer one", build), false;
    // Every file that differs is fetched and checked before any is put in place.
    bool fetched[MAX_FILES] = { 0 }, any = false;
    char here[1200], next[1210], old[1210];
    for (int i = 0; i < count; i++) {
        char hex[65];
        SDL_snprintf(here, sizeof here, "%s%s", u->folder, files[i].name);
        file_hash(here, hex);
        if (!SDL_strcmp(hex, files[i].hash)) continue;
        SDL_snprintf(next, sizeof next, "%s.new", here);
        if (!fetch(u, &files[i], next, error, error_size)) return false;
        fetched[i] = any = true;
    }
    if (!any) return false;  // (files that all match already are this build, whatever it calls itself)
    for (int i = 0; i < count; i++) {
        if (!fetched[i]) continue;
        SDL_snprintf(here, sizeof here, "%s%s", u->folder, files[i].name);
        SDL_snprintf(next, sizeof next, "%s.new", here);
        SDL_snprintf(old, sizeof old, "%s.old", here);
        SDL_RemovePath(old);
#ifndef _WIN32
        // (with the mode of the file it replaces, so that a program stays one that can be run)
        struct stat was;
        chmod(next, !stat(here, &was) ? was.st_mode & 07777 : !SDL_strcmp(files[i].name, u->program) ? 0755 : 0644);
#endif
        SDL_PathInfo info;
        if (SDL_GetPathInfo(here, &info) && !SDL_RenamePath(here, old)) return SDL_snprintf(error, error_size, "%s: %s", here, SDL_GetError()), false;
        if (!SDL_RenamePath(next, here)) return SDL_snprintf(error, error_size, "%s: %s", here, SDL_GetError()), false;
    }
    SDL_Log("%s: updated to build %s", u->agent, build);
    SDL_LockMutex(u->lock);
    SDL_strlcpy(u->k.trial, build, sizeof u->k.trial), u->k.failures = 0;
    save(u);
    SDL_UnlockMutex(u->lock);
    return true;
}

static int run(void *user) {
    gs_update *u = user;
    char said[300] = "";
    const Uint64 began = SDL_GetTicks();
    do {
        // On trial, this build is not replaced; once it has run its trial time, it has passed, and the
        // build it replaced goes.
        SDL_LockMutex(u->lock);
        bool trial = !SDL_strcmp(u->k.trial, u->build);
        if (trial && SDL_GetTicks() - began >= (Uint64)u->trial_ms) u->k.trial[0] = 0, u->k.failures = 0, save(u), sweep(u, ".old"), trial = false;
        bool asked = u->k.consent == GS_UPDATE_YES;
        SDL_UnlockMutex(u->lock);
        if (trial || !asked) continue;  // (and wait to look again)
        char error[300] = "";
        bool done = look(u, error, sizeof error);
        SDL_LockMutex(u->lock), SDL_strlcpy(u->error, error, sizeof u->error), SDL_UnlockMutex(u->lock);
        if (done) {
            SDL_SetAtomicInt(&u->ready, 1);
            if (u->ready_fn) u->ready_fn(u->ready_user);
            return 0;
        }
        if (error[0] && SDL_strcmp(error, said)) SDL_Log("%s: looking for an update: %s", u->agent, error);  // (when it first happens, not every look)
        SDL_strlcpy(said, error, sizeof said);
    } while (!SDL_GetAtomicInt(&u->stopping) && !SDL_WaitSemaphoreTimeout(u->wake, SDL_min(u->every_ms, u->trial_ms)));
    return 0;
}

gs_update *gs_update_start(const gs_update_config *c) {
    if (!c->build || !c->build[0] || !c->manifest || !c->key) return NULL;
    gs_update *u = SDL_calloc(1, sizeof *u);
    if (!u) return NULL;
    SDL_strlcpy(u->build, c->build, sizeof u->build), SDL_strlcpy(u->manifest, c->manifest, sizeof u->manifest);
    SDL_strlcpy(u->folder, c->folder ? c->folder : SDL_GetBasePath(), sizeof u->folder);
    SDL_strlcpy(u->program, c->program ? c->program : "", sizeof u->program), SDL_strlcpy(u->agent, c->agent ? c->agent : "gesso", sizeof u->agent);
    memcpy(u->key, c->key, 32);
    u->every_ms = c->every_ms > 0 ? c->every_ms : 60000;
    u->ready_fn = c->ready, u->ready_user = c->ready_user;
    u->idle_ms = c->idle_ms > 0 ? c->idle_ms : 120000, u->gap_ms = c->gap_ms > 0 ? c->gap_ms : 15 * 60000, u->trial_ms = c->trial_ms > 0 ? c->trial_ms : 3 * 60000;
    if (c->state) SDL_strlcpy(u->state, c->state, sizeof u->state);
    else {  // update.txt in the app's data folder
        char app[256];
        SDL_strlcpy(app, SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), sizeof app);
        char *dot = SDL_strrchr(app, '.');
        if (dot && !SDL_strcasecmp(dot, ".exe")) *dot = 0;
        char *dir = SDL_GetPrefPath(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING), app);
        if (dir) SDL_snprintf(u->state, sizeof u->state, "%supdate.txt", dir), SDL_free(dir);
    }
    load(u);
    sweep(u, ".bad");  // (what an earlier going-back put aside)
    // A build on trial that last ended without gs_update_free has failed once more; at three it is given up.
    if (!SDL_strcmp(u->k.trial, u->build) && !SDL_strcmp(u->k.running, u->build)) u->k.failures++;
    if (!SDL_strcmp(u->k.trial, u->build) && u->k.failures >= 3) roll_back(u);
    SDL_strlcpy(u->k.running, u->build, sizeof u->k.running);
    save(u);
    if (SDL_strcmp(u->k.trial, u->build)) sweep(u, ".old");  // (what a passed update renamed aside is no longer wanted)
    u->wake = SDL_CreateSemaphore(0), u->lock = SDL_CreateMutex();
    if (u->rolled_back) SDL_SetAtomicInt(&u->ready, 1);
    u->thread = SDL_CreateThread(run, "gs-update", u);
    return u;
}

bool gs_update_ready(gs_update *u) { return u && SDL_GetAtomicInt(&u->ready); }

void gs_update_error(gs_update *u, char *error, size_t size) {
    if (!u) { if (size) error[0] = 0; return; }
    SDL_LockMutex(u->lock), SDL_strlcpy(error, u->error, size), SDL_UnlockMutex(u->lock);
}

void gs_update_free(gs_update *u) {
    if (!u) return;
    SDL_SetAtomicInt(&u->stopping, 1);
    SDL_SignalSemaphore(u->wake);
    SDL_WaitThread(u->thread, NULL);
    if (!u->rolled_back) u->k.running[0] = 0, save(u);  // (a clean exit: this run is not counted against the build)
    SDL_DestroySemaphore(u->wake), SDL_DestroyMutex(u->lock);
    SDL_free(u);
}

int gs_update_consent(gs_update *u) {
    if (!u) return GS_UPDATE_NO;
    SDL_LockMutex(u->lock);
    int consent = u->k.consent;
    SDL_UnlockMutex(u->lock);
    return consent;
}

void gs_update_set_consent(gs_update *u, bool yes) {
    if (!u) return;
    SDL_LockMutex(u->lock);
    u->k.consent = yes ? GS_UPDATE_YES : GS_UPDATE_NO;
    save(u);
    SDL_UnlockMutex(u->lock);
    if (yes) SDL_SignalSemaphore(u->wake);  // (look now)
}

bool gs_update_rolled_back(gs_update *u) { return u && u->rolled_back; }

bool gs_update_due(gs_update *u, bool busy, Uint64 input_at) {
    if (!gs_update_ready(u) || busy || SDL_GetTicks() - input_at < (Uint64)u->idle_ms) return false;
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_LockMutex(u->lock);
    bool soon = (long long)(now / SDL_NS_PER_SECOND) - u->k.restarted < u->gap_ms / 1000;
    SDL_UnlockMutex(u->lock);
    return u->rolled_back || !soon;  // (going back from a failing build waits for no gap)
}

bool gs_update_restart_as(gs_update *u, int argc, char **argv) {
    if (!u) return false;
    const char *args[72];
    int n = 0;
    args[n++] = argv && argc ? argv[0] : u->program;
    bool flagged = false;
    for (int i = 1; argv && i < argc && n < 70; i++) args[n++] = argv[i], flagged |= !strcmp(argv[i], "--restarted");
    if (!flagged) args[n++] = "--restarted";
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_LockMutex(u->lock);
    u->k.restarted = (long long)(now / SDL_NS_PER_SECOND);
    u->k.running[0] = 0;  // (this run ends cleanly)
    save(u);
    bool started = gs_update_restart(u->folder, u->program, n, (char **)args);
    u->handed_over = started;
    SDL_UnlockMutex(u->lock);
    return started;
}

bool gs_update_restarted(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--restarted")) {
            SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");  // (the new build does not take the keyboard from what has it)
            return true;
        }
    return false;
}

bool gs_update_restart(const char *folder, const char *program, int argc, char **argv) {
    char path[1200];
    SDL_snprintf(path, sizeof path, "%s%s", folder ? folder : SDL_GetBasePath(), program);
    const char *args[72];
    int n = 0;
    args[n++] = path;
    for (int i = 1; i < argc && n < 71; i++) args[n++] = argv[i];
    args[n] = NULL;
    // (SDL's own process, not gs_http_spawn's, whose Windows job would end it with this one; in the
    // background, which on Windows starts a console program without a console window of its own)
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *next = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (next) SDL_DestroyProcess(next);
    return next != NULL;
}
