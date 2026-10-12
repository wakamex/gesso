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
#ifndef _WIN32
        // A folder whose path is too long for a socket's address is guarded all the same.
        char deep[1400];
        SDL_snprintf(deep, sizeof deep, "%s%0150d/%0150d", pref, 1, 2);  // (the same folder each run, so one socket is left behind, not one a run)
        SDL_SetAtomicInt(&got_args, 0);
        CHECK(!gs_instance_forward(deep, 1, args) && gs_instance_acquire(deep, on_args) && !gs_instance_acquire(deep, on_args));
        CHECK(gs_instance_forward(deep, 1, args) && wait_for(&got_args, 2000) && !strcmp(first_arg, "--toggle"));
#endif
        SDL_free(pref);
    }

    // An overlay window is made hidden, on top and unable to take the keyboard.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Window *overlay = gs_overlay_window("overlay", 320, 80);
        if (overlay) {  // (macOS's offscreen driver makes no window without a graphics context of its own)
            SDL_WindowFlags f = SDL_GetWindowFlags(overlay);
            CHECK((f & SDL_WINDOW_HIDDEN) && (f & SDL_WINDOW_NOT_FOCUSABLE) && (f & SDL_WINDOW_ALWAYS_ON_TOP) && (f & SDL_WINDOW_BORDERLESS) && (f & SDL_WINDOW_UTILITY));
            gs_overlay_show(overlay, true);
            CHECK(!(SDL_GetWindowFlags(overlay) & SDL_WINDOW_HIDDEN));
            gs_overlay_show(overlay, false);
            CHECK(SDL_GetWindowFlags(overlay) & SDL_WINDOW_HIDDEN);
            SDL_DestroyWindow(overlay);
        }
        SDL_Quit();
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
