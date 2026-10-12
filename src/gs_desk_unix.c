// Linux and macOS: one running instance, found through a socket in the app's data folder.
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "gs_desk.h"

static void (*on_args)(int argc, char **argv);

// The socket's place: in the data folder, or, for a folder whose path is too long for a socket's
// address (about 100 bytes), in the user's runtime folder or /tmp under a name made from the path.
static bool address(const char *data_dir, struct sockaddr_un *addr) {
    SDL_zerop(addr);
    addr->sun_family = AF_UNIX;
    if (SDL_snprintf(addr->sun_path, sizeof addr->sun_path, "%s/instance.sock", data_dir) < (int)sizeof addr->sun_path) return true;
    unsigned hash = 2166136261u;
    for (const char *p = data_dir; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    const char *runtime = SDL_getenv("XDG_RUNTIME_DIR");
    if (runtime && SDL_snprintf(addr->sun_path, sizeof addr->sun_path, "%s/gesso-instance-%08x.sock", runtime, hash) < (int)sizeof addr->sun_path) return true;
    return SDL_snprintf(addr->sun_path, sizeof addr->sun_path, "/tmp/gesso-instance-%u-%08x.sock", (unsigned)getuid(), hash) < (int)sizeof addr->sun_path;
}

// Arguments travel as their count, then each one ended by a zero byte.
static int listener(void *user) {
    int server = (int)(intptr_t)user;
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; return 0; }
        char buf[4096];
        size_t len = 0;
        for (ssize_t n; len < sizeof buf - 1 && (n = read(fd, buf + len, sizeof buf - 1 - len)) > 0;) len += (size_t)n;
        close(fd);
        buf[len] = 0;
        char *argv[32];
        int argc = 0;
        for (char *p = buf; p < buf + len && argc < 32; p += strlen(p) + 1) argv[argc++] = p;
        if (argc) on_args(argc, argv);
    }
}

bool gs_instance_forward(const char *data_dir, int argc, char **argv) {
    struct sockaddr_un addr;
    if (!address(data_dir, &addr)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool ok = !connect(fd, (struct sockaddr *)&addr, sizeof addr);
    for (int i = 0; ok && i < argc; i++) ok = write(fd, argv[i], strlen(argv[i]) + 1) == (ssize_t)strlen(argv[i]) + 1;
    close(fd);
    return ok;
}

bool gs_instance_acquire(const char *data_dir, void (*args)(int argc, char **argv)) {
    struct sockaddr_un addr;
    if (!address(data_dir, &addr)) return true;  // nowhere for a socket: run without the guard
    // A socket that answers is a running instance. One that does not was left by a crash.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    bool running = probe >= 0 && !connect(probe, (struct sockaddr *)&addr, sizeof addr);
    if (probe >= 0) close(probe);
    if (running) return false;
    unlink(addr.sun_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) || listen(server, 8)) return true;
    on_args = args;
    SDL_DetachThread(SDL_CreateThread(listener, "instance", (void *)(intptr_t)server));
    return true;
}
