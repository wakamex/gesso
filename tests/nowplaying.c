// gs_nowplaying where it can be driven from here: on Linux with a session D-Bus, gdbus plays the
// media panel's part, reads the player's properties and presses its buttons. Elsewhere, and without
// a bus, it only checks that the calls are safe when the controls are off.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_nowplaying.h"
#include "test.h"

#ifdef __linux__
// Runs gdbus with these arguments after "call --session -d NAME -o PATH -m", and returns what it printed.
static char *gdbus(const char *method, const char *arg1, const char *arg2) {
    const char *args[] = { "gdbus", "call", "--session", "-d", "org.mpris.MediaPlayer2.gessotest", "-o", "/org/mpris/MediaPlayer2", "-m", method, arg1, arg2, NULL };
    SDL_Process *p = SDL_CreateProcess(args, true);
    char *out = p ? SDL_ReadProcess(p, NULL, NULL) : NULL;
    SDL_DestroyProcess(p);
    return out;
}
#endif

void test_nowplaying(void) {
    gs_nowplaying_set("a title", "an artist", GS_NOWPLAYING_PLAYING);  // (off: nothing happens)
    gs_nowplaying_quit();
#ifdef __linux__
    const char *bus = SDL_getenv("DBUS_SESSION_BUS_ADDRESS");
    char *path = SDL_getenv("PATH") ? SDL_strdup(SDL_getenv("PATH")) : NULL;
    bool have_gdbus = false;
    char *rest = NULL;
    for (char *dir = path ? SDL_strtok_r(path, ":", &rest) : NULL; dir && !have_gdbus; dir = SDL_strtok_r(NULL, ":", &rest)) {
        char file[1024];
        SDL_snprintf(file, sizeof file, "%s/gdbus", dir);
        have_gdbus = SDL_GetPathInfo(file, NULL);
    }
    SDL_free(path);
    if (!bus || !have_gdbus || !SDL_InitSubSystem(SDL_INIT_EVENTS)) return;
    Uint32 type = gs_nowplaying_init(NULL, "gesso test", "gessotest");
    CHECK(type);
    if (!type) return;
    gs_nowplaying_set("A title", "An artist", GS_NOWPLAYING_PAUSED);
    char *all = gdbus("org.freedesktop.DBus.Properties.GetAll", "org.mpris.MediaPlayer2.Player", NULL);
    CHECK(all && strstr(all, "'PlaybackStatus': <'Paused'>") && strstr(all, "'xesam:title': <'A title'>") && strstr(all, "['An artist']"));
    SDL_free(all);
    SDL_free(gdbus("org.mpris.MediaPlayer2.Player.Next", NULL, NULL));
    SDL_free(gdbus("org.mpris.MediaPlayer2.Player.PlayPause", NULL, NULL));
    int got[2] = { 0 }, n = 0;
    SDL_Event e;
    for (Uint64 t = SDL_GetTicks(); n < 2 && SDL_GetTicks() - t < 3000;)
        if (SDL_WaitEventTimeout(&e, 100) && e.type == type) got[n++] = e.user.code;
    CHECK(got[0] == GS_NOWPLAYING_NEXT && got[1] == GS_NOWPLAYING_TOGGLE);
    gs_nowplaying_quit();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
#endif
}
