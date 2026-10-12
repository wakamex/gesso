// Linux: hotkeys through an X connection of gesso's own (libX11 is opened at run time, so an app
// builds and starts without it), and the paste keystroke through the X server's test extension, or
// under Wayland through wtype or ydotool.
#define _DEFAULT_SOURCE
#include <SDL3/SDL.h>
#include <poll.h>
#include <unistd.h>
#include "gs_desk.h"

// ---- Hotkeys ----

// The few Xlib types used, as Xlib lays them out, so no X header is needed to build.
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window, root, subwindow, time;
    int x, y, x_root, y_root;
    unsigned state, keycode;
    int same_screen;
} x_key_event;
typedef union { int type; x_key_event key; long pad[24]; } x_event;
typedef struct { int type; void *display; unsigned long resourceid, serial; unsigned char error_code, request_code, minor_code; } x_error_event;

#define X_KEY_PRESS 2
#define X_SHIFT 1u
#define X_LOCK 2u       // Caps Lock
#define X_CONTROL 4u
#define X_MOD1 8u       // Alt
#define X_MOD2 16u      // Num Lock
#define X_MOD4 64u      // Super
#define MAX_HOTKEYS 8

static struct {
    SDL_SharedObject *lib;
    void *display;
    unsigned long root;
    SDL_Mutex *lock;
    void (*fired)(int id);
    struct { int id; unsigned keycode, mods; } keys[MAX_HOTKEYS];
    SDL_AtomicInt refused;  // an X error arrived while grabbing
    void *(*open)(const char *);
    unsigned long (*default_root)(void *);
    unsigned char (*keysym_to_keycode)(void *, unsigned long);
    int (*grab)(void *, int, unsigned, unsigned long, int, int, int);
    int (*ungrab)(void *, int, unsigned, unsigned long);
    int (*sync)(void *, int);
    int (*pending)(void *);
    int (*next_event)(void *, x_event *);
    int (*connection)(void *);
    void *(*set_error_handler)(int (*)(void *, x_error_event *));
    int (*query_keymap)(void *, char *);
    unsigned long (*intern_atom)(void *, const char *, int);
    int (*change_property)(void *, unsigned long, unsigned long, unsigned long, int, int, const unsigned char *, int);
    SDL_SharedObject *test_lib;
    int (*fake_key)(void *, unsigned, int, unsigned long);  // XTestFakeKeyEvent
    bool test_tried;
} X;

static int on_x_error(void *display, x_error_event *e) {
    SDL_SetAtomicInt(&X.refused, 1);
    return 0;
}

static int hotkey_thread(void *user) {
    struct pollfd fd = { X.connection(X.display), POLLIN, 0 };
    for (;;) {
        poll(&fd, 1, 500);
        int fired[MAX_HOTKEYS], count = 0;
        SDL_LockMutex(X.lock);
        while (X.pending(X.display)) {
            x_event e;
            X.next_event(X.display, &e);
            if (e.type != X_KEY_PRESS) continue;
            unsigned mods = e.key.state & (X_SHIFT | X_CONTROL | X_MOD1 | X_MOD4);
            for (int i = 0; i < MAX_HOTKEYS; i++)
                if (X.keys[i].id && X.keys[i].keycode == e.key.keycode && X.keys[i].mods == mods && count < MAX_HOTKEYS) fired[count++] = X.keys[i].id;
        }
        SDL_UnlockMutex(X.lock);
        for (int i = 0; i < count; i++) X.fired(fired[i]);
    }
    return 0;
}

// Xlib's functions, once. False without the library.
static bool x_load(void) {
    static bool tried, ok;
    if (tried) return ok;
    tried = true;
    X.lib = SDL_LoadObject("libX11.so.6");
    struct { void *fn; const char *name; } syms[] = {
        { &X.open, "XOpenDisplay" }, { &X.default_root, "XDefaultRootWindow" }, { &X.keysym_to_keycode, "XKeysymToKeycode" },
        { &X.grab, "XGrabKey" }, { &X.ungrab, "XUngrabKey" }, { &X.sync, "XSync" }, { &X.pending, "XPending" },
        { &X.next_event, "XNextEvent" }, { &X.connection, "XConnectionNumber" }, { &X.set_error_handler, "XSetErrorHandler" },
        { &X.query_keymap, "XQueryKeymap" }, { &X.intern_atom, "XInternAtom" }, { &X.change_property, "XChangeProperty" },
    };
    ok = X.lib != NULL;
    for (size_t i = 0; ok && i < SDL_arraysize(syms); i++) ok = (*(SDL_FunctionPointer *)syms[i].fn = SDL_LoadFunction(X.lib, syms[i].name)) != NULL;
    int (*init_threads)(void) = ok ? (int (*)(void))SDL_LoadFunction(X.lib, "XInitThreads") : NULL;
    if (init_threads) init_threads();
    return ok;
}

// Opens the X connection, once. False where there is no X server to ask: a Wayland desktop without
// XWayland, or no desktop at all.
static bool x_connect(void) {
    static bool tried;
    if (tried) return X.display != NULL;
    tried = true;
    X.display = x_load() ? X.open(NULL) : NULL;
    if (!X.display) return false;
    X.root = X.default_root(X.display);
    X.lock = SDL_CreateMutex();
    X.set_error_handler(on_x_error);
    return true;
}

bool gs_hotkeys_init(void (*fired)(int id), char *error, size_t error_size) {
    if (X.fired) return true;
    if (!x_connect()) { SDL_strlcpy(error, "your desktop blocks global shortcuts", error_size); return false; }
    X.fired = fired;
    SDL_DetachThread(SDL_CreateThread(hotkey_thread, "hotkeys", NULL));
    return true;
}

static unsigned long keysym_of(const char *key) {
    static const struct { const char *name; unsigned long sym; } named[] = {
        { "space", 0x20 }, { "tab", 0xff09 }, { "enter", 0xff0d }, { "return", 0xff0d }, { "escape", 0xff1b }, { "esc", 0xff1b },
        { "backspace", 0xff08 }, { "delete", 0xffff }, { "insert", 0xff63 }, { "home", 0xff50 }, { "end", 0xff57 },
        { "pageup", 0xff55 }, { "pagedown", 0xff56 }, { "up", 0xff52 }, { "down", 0xff54 }, { "left", 0xff51 }, { "right", 0xff53 },
        { "plus", 0x2b }, { "capslock", 0xffe5 }, { "numlock", 0xff7f }, { "scrolllock", 0xff14 }, { "printscreen", 0xff61 },
        { "numdec", 0xffae }, { "numadd", 0xffab }, { "numsub", 0xffad }, { "nummult", 0xffaa }, { "numdiv", 0xffaf },
    };
    size_t n = SDL_strlen(key);
    if (n == 1) return (unsigned char)key[0];  // letters (already lowercase), digits and punctuation are their own keysyms
    if (key[0] == 'f' && SDL_isdigit((unsigned char)key[1])) return 0xffbe + (unsigned long)SDL_atoi(key + 1) - 1;
    if (!SDL_strncmp(key, "num", 3) && SDL_isdigit((unsigned char)key[3]) && n == 4) return 0xffb0 + (unsigned long)(key[3] - '0');
    for (size_t i = 0; i < SDL_arraysize(named); i++)
        if (!SDL_strcmp(key, named[i].name)) return named[i].sym;
    return 0;
}

// The four ways Caps Lock and Num Lock can be set, each of which is a different grab to X.
static const unsigned LOCKS[] = { 0, X_LOCK, X_MOD2, X_LOCK | X_MOD2 };

static void ungrab(int id) {
    for (int i = 0; i < MAX_HOTKEYS; i++) {
        if (X.keys[i].id != id) continue;
        for (size_t k = 0; k < SDL_arraysize(LOCKS); k++) X.ungrab(X.display, (int)X.keys[i].keycode, X.keys[i].mods | LOCKS[k], X.root);
        X.keys[i].id = 0;
    }
    X.sync(X.display, 0);
}

void gs_hotkey_unregister(int id) {
    if (!X.fired) return;
    SDL_LockMutex(X.lock);
    ungrab(id);
    SDL_UnlockMutex(X.lock);
}

bool gs_hotkey_register(int id, const gs_accelerator *a, char *error, size_t error_size) {
    if (!X.fired) { SDL_strlcpy(error, "your desktop blocks global shortcuts", error_size); return false; }
    SDL_LockMutex(X.lock);
    ungrab(id);
    unsigned keycode = X.keysym_to_keycode(X.display, keysym_of(a->key));
    unsigned mods = (a->mods & GS_MOD_CTRL ? X_CONTROL : 0) | (a->mods & GS_MOD_SHIFT ? X_SHIFT : 0) | (a->mods & GS_MOD_ALT ? X_MOD1 : 0) | (a->mods & GS_MOD_SUPER ? X_MOD4 : 0);
    int slot = -1;
    for (int i = 0; i < MAX_HOTKEYS && slot < 0; i++)
        if (!X.keys[i].id) slot = i;
    bool ok = keycode && slot >= 0;
    if (ok) {
        // A grab another client already holds comes back as an error, some time later: the
        // errors are collected by a sync.
        X.sync(X.display, 0);
        SDL_SetAtomicInt(&X.refused, 0);
        for (size_t k = 0; k < SDL_arraysize(LOCKS); k++) X.grab(X.display, (int)keycode, mods | LOCKS[k], X.root, 0, 1, 1);
        X.sync(X.display, 0);
        ok = !SDL_GetAtomicInt(&X.refused);
        X.keys[slot].id = id;
        X.keys[slot].keycode = keycode;
        X.keys[slot].mods = mods;
        if (!ok) ungrab(id);
    }
    SDL_UnlockMutex(X.lock);
    if (!ok) SDL_strlcpy(error, keycode ? "already in use, or your desktop blocks global shortcuts" : "this keyboard has no such key", error_size);
    return ok;
}

// ---- Paste ----

static bool command_exists(const char *cmd) {
    const char *path = SDL_getenv("PATH");
    char candidate[1024];
    for (const char *dir = path; dir && *dir;) {
        const char *end = SDL_strchr(dir, ':');
        size_t n = end ? (size_t)(end - dir) : SDL_strlen(dir);
        SDL_snprintf(candidate, sizeof candidate, "%.*s/%s", (int)n, dir, cmd);
        if (!access(candidate, X_OK)) return true;
        dir = end ? end + 1 : NULL;
    }
    return false;
}

bool gs_paste_permitted(void) { return true; }
void gs_paste_ask_permission(void) {}

// Ctrl+V through the X server's test extension, as if typed. Modifier keys held at the moment (the
// user's hotkey, not yet let go) are released first, or the app would get Ctrl+Shift+V; they are not
// pressed again afterwards, since the user may have let go meanwhile and the key would then stick.
static bool x_paste(void) {
    if (!x_connect()) return false;
    if (!X.test_tried) {
        X.test_tried = true;
        X.test_lib = SDL_LoadObject("libXtst.so.6");
        int (*query)(void *, int *, int *, int *, int *) = X.test_lib ? (int (*)(void *, int *, int *, int *, int *))SDL_LoadFunction(X.test_lib, "XTestQueryExtension") : NULL;
        int a, b, c, d;
        if (query && query(X.display, &a, &b, &c, &d)) X.fake_key = (int (*)(void *, unsigned, int, unsigned long))SDL_LoadFunction(X.test_lib, "XTestFakeKeyEvent");
    }
    if (!X.fake_key) return false;
    static const unsigned long modifiers[] = { 0xffe1, 0xffe2, 0xffe3, 0xffe4, 0xffe9, 0xffea, 0xffeb, 0xffec, 0xffe7, 0xffe8, 0xfe03 };  // Shift, Control, Alt, Super and Meta, left and right, and AltGr
    SDL_LockMutex(X.lock);
    unsigned ctrl = X.keysym_to_keycode(X.display, 0xffe3), v = X.keysym_to_keycode(X.display, 'v');
    char held[32] = { 0 };
    X.query_keymap(X.display, held);
    for (size_t i = 0; i < SDL_arraysize(modifiers); i++) {
        unsigned code = X.keysym_to_keycode(X.display, modifiers[i]);
        if (code && (held[code / 8] & (1 << (code % 8)))) X.fake_key(X.display, code, 0, 0);
    }
    bool ok = ctrl && v;
    if (ok) X.fake_key(X.display, ctrl, 1, 0), X.fake_key(X.display, v, 1, 0), X.fake_key(X.display, v, 0, 0), X.fake_key(X.display, ctrl, 0, 0);
    X.sync(X.display, 0);
    SDL_UnlockMutex(X.lock);
    return ok;
}

gs_paste_result gs_paste_keystroke(char *detail, size_t detail_size) {
    static const char *const wtype[] = { "wtype", "-M", "ctrl", "-k", "v", "-m", "ctrl", NULL };
    // ydotool works anywhere its daemon runs: 29 is the left Ctrl key and 47 is V.
    static const char *const ydotool[] = { "ydotool", "key", "29:1", "47:1", "47:0", "29:0", NULL };
    static const char *const xdotool[] = { "xdotool", "key", "--clearmodifiers", "ctrl+v", NULL };
    const char *session = SDL_getenv("XDG_SESSION_TYPE"), *display = SDL_getenv("WAYLAND_DISPLAY");
    bool wayland = (session && !SDL_strcmp(session, "wayland")) || (display && *display);
    // Under Wayland the X server reaches only the apps that run through it, so the tools go first there.
    const char *const *candidates[3];
    int count = 0;
    if (wayland) candidates[count++] = wtype, candidates[count++] = ydotool;
    else if (x_paste()) return GS_PASTE_SENT;
    candidates[count++] = xdotool;
    bool any = false;
    SDL_strlcpy(detail, "", detail_size);
    for (int i = 0; i < count; i++) {
        if (!command_exists(candidates[i][0])) continue;
        any = true;
        SDL_Process *p = SDL_CreateProcess(candidates[i], false);
        int code = -1;
        if (p && SDL_WaitProcess(p, true, &code) && code == 0) { SDL_DestroyProcess(p); return GS_PASTE_SENT; }
        SDL_snprintf(detail, detail_size, "%s exited with code %d", candidates[i][0], code);
        if (p) SDL_DestroyProcess(p);
    }
    if (wayland && x_paste()) return GS_PASTE_SENT;
    if (!any) SDL_strlcpy(detail, "No way to send keys (install wtype or ydotool)", detail_size);
    return any ? GS_PASTE_FAILED : GS_PASTE_NO_TOOL;
}

// ---- The clipboard's contents ----

struct gs_clip { char **types; void **data; size_t *sizes; int count; };

void gs_clipboard_free(gs_clip *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) { SDL_free(s->types[i]); SDL_free(s->data[i]); }
    SDL_free(s->types);
    SDL_free(s->data);
    SDL_free(s->sizes);
    SDL_free(s);
}

gs_clip *gs_clipboard_snapshot(void) {
    size_t n = 0;
    char **types = SDL_GetClipboardMimeTypes(&n);
    gs_clip *s = SDL_calloc(1, sizeof *s);
    s->types = SDL_calloc(n + 1, sizeof *s->types);
    s->data = SDL_calloc(n + 1, sizeof *s->data);
    s->sizes = SDL_calloc(n + 1, sizeof *s->sizes);
    for (size_t i = 0; types && i < n; i++) {
        size_t size = 0;
        void *data = SDL_GetClipboardData(types[i], &size);
        if (!data || !size) { SDL_free(data); continue; }
        s->types[s->count] = SDL_strdup(types[i]);
        s->data[s->count] = data;
        s->sizes[s->count++] = size;
    }
    SDL_free(types);
    if (!s->count) { gs_clipboard_free(s); return NULL; }
    return s;
}

static const void *offer(void *user, const char *mime, size_t *size) {
    gs_clip *s = user;
    for (int i = 0; i < s->count; i++)
        if (!SDL_strcmp(s->types[i], mime)) { *size = s->sizes[i]; return s->data[i]; }
    *size = 0;
    return NULL;
}

static void offer_done(void *user) { gs_clipboard_free(user); }

// The data is handed over when another program asks for it, so the snapshot lives on with SDL.
bool gs_clipboard_restore(gs_clip *s) {
    if (SDL_SetClipboardData(offer, offer_done, s, (const char **)s->types, (size_t)s->count)) return true;
    gs_clipboard_free(s);
    return false;
}

// ---- An overlay window ----

// On X11 a window says which desktop it is on with _NET_WM_DESKTOP, and all of them with 0xFFFFFFFF,
// which the window manager reads as the window appears (and forgets when it is hidden).
void gs_desk_overlay_native(SDL_Window *win) {
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    unsigned long window = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!display || !window || !x_load()) return;
    unsigned long all = 0xFFFFFFFFul, desktop = X.intern_atom(display, "_NET_WM_DESKTOP", 0);
    X.change_property(display, window, desktop, 6 /* CARDINAL */, 32, 0 /* replace */, (const unsigned char *)&all, 1);
}
