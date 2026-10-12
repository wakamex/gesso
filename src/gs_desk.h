// What an app does to the desktop around it: global hotkeys, a paste keystroke into whichever app
// has the keyboard, the clipboard's whole contents put aside and put back, a window that floats
// over everything without taking the keyboard, and being the one running instance. For a tray app that acts on other apps (a dictation tool, a clipboard manager,
// a launcher). Windows, macOS and Linux each have an implementation (gs_desk_windows.c,
// gs_desk_macos.c and gs_desk_macos.m, gs_desk_linux.c; the instance socket of the last two is in
// gs_desk_unix.c). On Linux the hotkeys need an X server (X11, or XWayland): under Wayland alone
// gs_hotkeys_init says so, and the app can offer a command for the desktop's own shortcuts instead.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

// ---- Accelerators ----

// A key combination as Electron writes it ("CommandOrControl+Shift+Space", "Alt+F5"), taken apart.
// gs_ui_hotkey (gs_ui.h) writes them from the keys a user presses.
enum { GS_MOD_CTRL = 1, GS_MOD_SHIFT = 2, GS_MOD_ALT = 4, GS_MOD_SUPER = 8 };  // SUPER: Cmd, the Windows key
typedef struct {
    int mods;
    char key[16];  // lowercase: a letter, a digit, "space", "f5", "enter", "up", "num3", a punctuation mark
} gs_accelerator;

// False with `error` set for an empty string, an unknown key, or no key at all.
bool gs_accelerator_parse(const char *text, gs_accelerator *out, char *error, size_t error_size);
// How the combination reads to a user on this system: "Ctrl+Shift+Space", "Cmd+Shift+Space" on
// macOS. "" for an empty one.
void gs_accelerator_label(const char *text, char *out, size_t size);

// ---- Global hotkeys ----

// `fired` is called with the id of a hotkey when it is pressed, whichever app has the keyboard, on a
// thread of the hotkeys' own (the main thread on macOS). False with `error` set where the desktop
// offers no way to grab a key (Wayland without X).
bool gs_hotkeys_init(void (*fired)(int id), char *error, size_t error_size);
// Takes the combination for `id` (a number of the app's choosing, above 0), replacing what that id
// held. False with `error` set when the system refuses it, most often because another app holds
// it; the id then holds nothing. Up to eight ids at once.
bool gs_hotkey_register(int id, const gs_accelerator *accelerator, char *error, size_t error_size);
void gs_hotkey_unregister(int id);

// ---- Paste ----

typedef enum {
    GS_PASTE_SENT,
    GS_PASTE_NOT_PERMITTED,  // macOS: Accessibility is off for this app
    GS_PASTE_NO_TOOL,        // Linux: no X server to send keys through, and none of wtype, ydotool and xdotool installed
    GS_PASTE_FAILED,
} gs_paste_result;

// Sends the paste chord (Ctrl+V; Cmd+V on macOS) to the app that has the keyboard: SendInput on
// Windows, posted key events on macOS, the X server's test extension on X11, and wtype or ydotool
// under Wayland (xdotool as a last resort). Modifier keys the user still holds from a hotkey are
// let go first on X11, so the chord is not Ctrl+Shift+V. It may wait for a tool to run; `detail`
// gets what failed, for a log.
gs_paste_result gs_paste_keystroke(char *detail, size_t detail_size);
bool gs_paste_permitted(void);  // false only on macOS without Accessibility
// macOS: has the system ask the user for Accessibility, which also lists the app in System
// Settings so the user has something to switch on. Does nothing elsewhere.
void gs_paste_ask_permission(void);

// ---- The clipboard's contents ----

// Everything the clipboard holds, in every format, to put back after the app has used the
// clipboard for a paste of its own: text alone would lose a screenshot, a file copy or rich text.
// SDL lists the formats of what it put there itself and, on macOS, of nothing else, so each system
// has its own way here: the pasteboard's items on macOS, every memory format on Windows, SDL's
// formats on Linux. Call these on the main thread.
typedef struct gs_clip gs_clip;
gs_clip *gs_clipboard_snapshot(void);     // NULL when there is nothing to capture
bool gs_clipboard_restore(gs_clip *snap); // puts it back and frees it
void gs_clipboard_free(gs_clip *snap);

// ---- An overlay window ----

// A small window that floats over the desktop and never takes the keyboard from the app the user is
// working in: without a frame, transparent where nothing is drawn, above other windows, absent from
// the taskbar and the Dock, and drawn at the display's full resolution. It is made hidden; show and
// hide it with gs_overlay_show, which also puts it on every workspace: on macOS it joins all Spaces
// and sits over full-screen apps, and on X11 it asks the window manager for all desktops. Windows
// has no call for all virtual desktops, so there it stays on the one it was shown on; above a
// full-screen game that owns the display, no window shows on any system. Under Wayland alone a
// window cannot be placed, so SDL_SetWindowPosition does nothing and the compositor chooses.
SDL_Window *gs_overlay_window(const char *title, int w, int h);
void gs_overlay_show(SDL_Window *win, bool shown);

// ---- One instance ----

// Becomes the running instance for `data_dir`, a folder of the app's own (one instance per
// folder). Arguments a later instance forwards arrive at `on_args`, on another thread. False when
// an instance is already running.
bool gs_instance_acquire(const char *data_dir, void (*on_args)(int argc, char **argv));
// Hands the arguments to the running instance. False when there is none.
bool gs_instance_forward(const char *data_dir, int argc, char **argv);
