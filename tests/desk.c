// gs_desk: accelerators taken apart, one instance with its arguments forwarded, and, where the
// desktop allows them, a global hotkey taken, refused when held twice, and fired by the paste keystroke.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_desk.h"
#include "test.h"

static SDL_AtomicInt got_args, fired_id;
static char first_arg[64], last_arg[64];

static void on_args(int argc, char **argv) {
    SDL_strlcpy(first_arg, argv[0], sizeof first_arg);
    SDL_strlcpy(last_arg, argv[argc - 1], sizeof last_arg);
    SDL_SetAtomicInt(&got_args, argc);
}

static void on_fired(int id) { SDL_SetAtomicInt(&fired_id, id); }

static bool wait_for(SDL_AtomicInt *value, int ms) {
    for (int i = 0; i < ms / 5 && !SDL_GetAtomicInt(value); i++) SDL_Delay(5);
    return SDL_GetAtomicInt(value) != 0;
}

void test_desk(void) {
    char error[200] = "", label[64];
    gs_accelerator a;
    CHECK(gs_accelerator_parse("CommandOrControl+Shift+Space", &a, error, sizeof error) && !strcmp(a.key, "space") && (a.mods & GS_MOD_SHIFT));
#ifdef __APPLE__
    CHECK(a.mods == (GS_MOD_SUPER | GS_MOD_SHIFT));
#else
    CHECK(a.mods == (GS_MOD_CTRL | GS_MOD_SHIFT));
#endif
    CHECK(gs_accelerator_parse(" alt + F5 ", &a, error, sizeof error) && !strcmp(a.key, "f5") && a.mods == GS_MOD_ALT);
    CHECK(gs_accelerator_parse("Ctrl++", &a, error, sizeof error) && !strcmp(a.key, "plus") && a.mods == GS_MOD_CTRL);
    CHECK(gs_accelerator_parse("Super+num3", &a, error, sizeof error) && !strcmp(a.key, "num3") && a.mods == GS_MOD_SUPER);
    CHECK(!gs_accelerator_parse("", &a, error, sizeof error) && error[0]);
    CHECK(!gs_accelerator_parse("Ctrl+Shift", &a, error, sizeof error) && !gs_accelerator_parse("Ctrl+Nope", &a, error, sizeof error) && !gs_accelerator_parse("A+B", &a, error, sizeof error));
    gs_accelerator_label("CommandOrControl+Shift+Space", label, sizeof label);
#ifdef __APPLE__
    CHECK(!strcmp(label, "Cmd+Shift+Space"));
#else
    CHECK(!strcmp(label, "Ctrl+Shift+Space"));
#endif
    gs_accelerator_label("Alt+num3", label, sizeof label);
    CHECK(strstr(label, "Numpad 3") != NULL);

    // One instance per folder: the second to ask is refused, and what it forwards reaches the first.
    char *pref = SDL_GetPrefPath("gesso", "tests");
    if (pref) {
        char dir[1024];
        SDL_snprintf(dir, sizeof dir, "%si%x", pref, (unsigned)(SDL_GetTicksNS() & 0xffffff));
        SDL_CreateDirectory(dir);
        char *args[] = { "--toggle", "second word", "last" };
        CHECK(!gs_instance_forward(dir, 3, args));  // nothing is running yet
        CHECK(gs_instance_acquire(dir, on_args));
        CHECK(!gs_instance_acquire(dir, on_args));
        CHECK(gs_instance_forward(dir, 3, args));
        CHECK(wait_for(&got_args, 2000) && SDL_GetAtomicInt(&got_args) == 3 && !strcmp(first_arg, "--toggle") && !strcmp(last_arg, "last"));
        SDL_SetAtomicInt(&got_args, 0);
        char *again[] = { "--again" };  // and a second forward straight after the first
        CHECK(gs_instance_forward(dir, 1, again) && wait_for(&got_args, 2000) && !strcmp(first_arg, "--again"));
        SDL_free(pref);
    }

    // Hotkeys, where there is a desktop to grab keys on (not in a headless run, nor under Wayland alone).
    if (!gs_hotkeys_init(on_fired, error, sizeof error)) { CHECK(error[0] && !SDL_getenv("GS_TEST_DESKTOP")); return; }
    gs_accelerator paste, other;
    gs_accelerator_parse("CommandOrControl+V", &paste, error, sizeof error);
    gs_accelerator_parse("Alt+Shift+F9", &other, error, sizeof error);
    CHECK(gs_hotkey_register(2, &other, error, sizeof error));
    CHECK(gs_hotkey_register(2, &other, error, sizeof error));  // the same id again replaces its own
#ifdef _WIN32
    CHECK(!gs_hotkey_register(3, &other, error, sizeof error) && error[0]);  // held already, here by this app
#endif
    gs_hotkey_unregister(2);
    gs_hotkey_unregister(3);
    // The paste keystroke, caught by a hotkey on the same keys so that no other app sees it.
    if (gs_hotkey_register(1, &paste, error, sizeof error)) {
        char detail[200] = "";
        bool sent = gs_paste_permitted() && gs_paste_keystroke(detail, sizeof detail) == GS_PASTE_SENT, fired = sent && wait_for(&fired_id, 1000);
        if (fired) CHECK(SDL_GetAtomicInt(&fired_id) == 1);
        // With GS_TEST_DESKTOP set the run has a desktop that takes sent keys (an X display, a signed-in
        // Windows session, a Mac with Accessibility granted), and the hotkey must fire.
        if (SDL_getenv("GS_TEST_DESKTOP")) CHECK(sent && fired);
        gs_hotkey_unregister(1);
    }
}
