// gs_desk on Android, where an app acts on nothing outside itself: no global hotkeys, no keystrokes
// into other apps, and no windows over them. The clipboard holds text, put aside and back through
// SDL. The one-instance socket is gs_desk_unix.c's, in the app's own folder.
#include <SDL3/SDL.h>
#include "gs_desk.h"

bool gs_hotkeys_init(void (*fired)(int id), char *error, size_t error_size) {
    SDL_strlcpy(error, "Android has no global hotkeys", error_size);
    return false;
}

bool gs_hotkey_register(int id, const gs_accelerator *accelerator, char *error, size_t error_size) {
    SDL_strlcpy(error, "Android has no global hotkeys", error_size);
    return false;
}

void gs_hotkey_unregister(int id) {}

bool gs_paste_permitted(void) { return true; }
void gs_paste_ask_permission(void) {}

gs_paste_result gs_paste_keystroke(char *detail, size_t detail_size) {
    SDL_strlcpy(detail, "Android lets no app type into another", detail_size);
    return GS_PASTE_FAILED;
}

struct gs_clip { char *text; };

void gs_clipboard_free(gs_clip *s) {
    if (!s) return;
    SDL_free(s->text);
    SDL_free(s);
}

gs_clip *gs_clipboard_snapshot(void) {
    if (!SDL_HasClipboardText()) return NULL;
    gs_clip *s = SDL_calloc(1, sizeof *s);
    if (s) s->text = SDL_GetClipboardText();
    if (!s || !s->text || !*s->text) { gs_clipboard_free(s); return NULL; }
    return s;
}

bool gs_clipboard_restore(gs_clip *s) {
    bool ok = SDL_SetClipboardText(s->text);
    gs_clipboard_free(s);
    return ok;
}

void gs_desk_overlay_native(SDL_Window *win) {}
