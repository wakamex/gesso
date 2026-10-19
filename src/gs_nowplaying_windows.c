// Windows: the window's System Media Transport Controls, through the Windows Runtime, from C.
// combase.dll is opened at run time and the interfaces used are declared as their tables of
// functions, in the order windows.media.h (Windows SDK 10.0.26100) gives them.
#include <SDL3/SDL.h>
#include <windows.h>

#include "gs_nowplaying.h"

typedef void *hstring;
typedef struct { void **v; } obj;
typedef struct { __int64 value; } token;

enum { QUERY = 0, ADDREF = 1, RELEASE = 2, FIRST = 6 };
enum { INTEROP_GET_FOR_WINDOW = FIRST };  // ISystemMediaTransportControlsInterop
enum {                                    // ISystemMediaTransportControls
    SMTC_PUT_STATUS = FIRST + 1, SMTC_GET_UPDATER = FIRST + 2, SMTC_PUT_ENABLED = FIRST + 5, SMTC_PUT_PLAY = FIRST + 7, SMTC_PUT_STOP = FIRST + 9,
    SMTC_PUT_PAUSE = FIRST + 11, SMTC_PUT_PREVIOUS = FIRST + 19, SMTC_PUT_NEXT = FIRST + 21, SMTC_ADD_BUTTON = FIRST + 26, SMTC_REMOVE_BUTTON = FIRST + 27,
};
enum { UPDATER_PUT_TYPE = FIRST + 1, UPDATER_GET_MUSIC = FIRST + 6, UPDATER_CLEAR = FIRST + 10, UPDATER_UPDATE = FIRST + 11 };  // ...DisplayUpdater
enum { MUSIC_PUT_TITLE = FIRST + 1, MUSIC_PUT_ARTIST = FIRST + 5 };                                                          // IMusicDisplayProperties
enum { ARGS_GET_BUTTON = FIRST };                                                                                             // ...ButtonPressedEventArgs
enum { STATUS_CLOSED = 0, STATUS_STOPPED = 2, STATUS_PLAYING = 3, STATUS_PAUSED = 4, TYPE_MUSIC = 1 };
enum { BUTTON_PLAY = 0, BUTTON_PAUSE = 1, BUTTON_STOP = 2, BUTTON_NEXT = 6, BUTTON_PREVIOUS = 7 };

static const GUID IID_INTEROP = { 0xddb0472d, 0xc911, 0x4a1f, { 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a } };
static const GUID IID_SMTC = { 0x99fa3ff4, 0x1742, 0x42a6, { 0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec } };
static const GUID IID_BUTTON_HANDLER = { 0x0557e996, 0x7b23, 0x5bae, { 0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4 } };  // TypedEventHandler<SystemMediaTransportControls, ...ButtonPressedEventArgs>
static const GUID IID_UNKNOWN = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_AGILE = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

#define CALL(o, slot, type) ((type)((obj *)(o))->v[slot])
static void release(void *o) { if (o) CALL(o, RELEASE, ULONG (WINAPI *)(void *))(o); }
#define PUT_BOOL(o, slot, b) CALL(o, slot, HRESULT (WINAPI *)(void *, boolean))(o, b)

static struct {
    HMODULE combase;
    HRESULT (WINAPI *create_string)(const WCHAR *, UINT32, hstring *);
    HRESULT (WINAPI *delete_string)(hstring);
    void *smtc, *updater;
    token pressed;
    Uint32 event;
} W;

// The object the controls call when a button is pressed. It lives as long as the program.
static HRESULT WINAPI handler_query(void *h, const GUID *iid, void **out) {
    if (!IsEqualGUID(iid, &IID_UNKNOWN) && !IsEqualGUID(iid, &IID_AGILE) && !IsEqualGUID(iid, &IID_BUTTON_HANDLER)) return *out = NULL, E_NOINTERFACE;
    return *out = h, S_OK;
}
static ULONG WINAPI handler_addref(void *h) { return 2; }
static ULONG WINAPI handler_release(void *h) { return 1; }
static HRESULT WINAPI handler_invoke(void *h, void *sender, void *args) {  // (on a thread of the system's)
    int button = -1;
    if (args) CALL(args, ARGS_GET_BUTTON, HRESULT (WINAPI *)(void *, int *))(args, &button);
    int command = button == BUTTON_PLAY ? GS_NOWPLAYING_PLAY : button == BUTTON_PAUSE ? GS_NOWPLAYING_PAUSE : button == BUTTON_STOP ? GS_NOWPLAYING_STOP
                : button == BUTTON_NEXT ? GS_NOWPLAYING_NEXT : button == BUTTON_PREVIOUS ? GS_NOWPLAYING_PREVIOUS : 0;
    if (command) {
        SDL_Event e = { .user = { .type = W.event, .code = command } };
        SDL_PushEvent(&e);
    }
    return S_OK;
}
static void *handler_table[] = { handler_query, handler_addref, handler_release, handler_invoke };
static obj handler = { handler_table };

static hstring string(const char *utf8) {
    WCHAR *wide = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", utf8, SDL_strlen(utf8) + 1);
    hstring s = NULL;
    if (wide) W.create_string(wide, (UINT32)SDL_wcslen(wide), &s);
    SDL_free(wide);
    return s;
}

Uint32 gs_nowplaying_init(SDL_Window *win, const char *name, const char *id) {
    if (W.smtc) return W.event;
    HWND hwnd = win ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
    W.combase = hwnd ? LoadLibraryW(L"combase.dll") : NULL;
    HRESULT (WINAPI *initialize)(int) = W.combase ? (HRESULT (WINAPI *)(int))(void *)GetProcAddress(W.combase, "RoInitialize") : NULL;
    HRESULT (WINAPI *get_factory)(hstring, const GUID *, void **) = W.combase ? (HRESULT (WINAPI *)(hstring, const GUID *, void **))(void *)GetProcAddress(W.combase, "RoGetActivationFactory") : NULL;
    W.create_string = W.combase ? (HRESULT (WINAPI *)(const WCHAR *, UINT32, hstring *))(void *)GetProcAddress(W.combase, "WindowsCreateString") : NULL;
    W.delete_string = W.combase ? (HRESULT (WINAPI *)(hstring))(void *)GetProcAddress(W.combase, "WindowsDeleteString") : NULL;
    if (!initialize || !get_factory || !W.create_string || !W.delete_string) return 0;
    initialize(1 /* multithreaded; a thread already set up another way keeps its own */);
    hstring cls = string("Windows.Media.SystemMediaTransportControls");
    void *interop = NULL;
    if (cls && SUCCEEDED(get_factory(cls, &IID_INTEROP, &interop)))
        CALL(interop, INTEROP_GET_FOR_WINDOW, HRESULT (WINAPI *)(void *, HWND, const GUID *, void **))(interop, hwnd, &IID_SMTC, &W.smtc);
    release(interop);
    W.delete_string(cls);
    if (!W.smtc) return 0;
    W.event = SDL_RegisterEvents(1);
    PUT_BOOL(W.smtc, SMTC_PUT_PLAY, 1), PUT_BOOL(W.smtc, SMTC_PUT_PAUSE, 1), PUT_BOOL(W.smtc, SMTC_PUT_STOP, 1);
    PUT_BOOL(W.smtc, SMTC_PUT_NEXT, 1), PUT_BOOL(W.smtc, SMTC_PUT_PREVIOUS, 1);
    CALL(W.smtc, SMTC_ADD_BUTTON, HRESULT (WINAPI *)(void *, void *, token *))(W.smtc, &handler, &W.pressed);
    CALL(W.smtc, SMTC_GET_UPDATER, HRESULT (WINAPI *)(void *, void **))(W.smtc, &W.updater);
    if (W.updater) CALL(W.updater, UPDATER_PUT_TYPE, HRESULT (WINAPI *)(void *, int))(W.updater, TYPE_MUSIC);
    PUT_BOOL(W.smtc, SMTC_PUT_ENABLED, 0);  // (shown once something plays)
    return W.event;
}

void gs_nowplaying_set(const char *title, const char *artist, int state) {
    if (!W.smtc) return;
    void *music = NULL;
    if (W.updater && SUCCEEDED(CALL(W.updater, UPDATER_GET_MUSIC, HRESULT (WINAPI *)(void *, void **))(W.updater, &music)) && music) {
        hstring t = string(title ? title : ""), a = string(artist ? artist : "");
        CALL(music, MUSIC_PUT_TITLE, HRESULT (WINAPI *)(void *, hstring))(music, t);
        CALL(music, MUSIC_PUT_ARTIST, HRESULT (WINAPI *)(void *, hstring))(music, a);
        W.delete_string(t), W.delete_string(a), release(music);
        CALL(W.updater, UPDATER_UPDATE, HRESULT (WINAPI *)(void *))(W.updater);
    }
    CALL(W.smtc, SMTC_PUT_STATUS, HRESULT (WINAPI *)(void *, int))(W.smtc, state == GS_NOWPLAYING_PLAYING ? STATUS_PLAYING : state == GS_NOWPLAYING_PAUSED ? STATUS_PAUSED : STATUS_STOPPED);
    PUT_BOOL(W.smtc, SMTC_PUT_ENABLED, state != GS_NOWPLAYING_STOPPED);
}

void gs_nowplaying_quit(void) {
    if (!W.smtc) return;
    PUT_BOOL(W.smtc, SMTC_PUT_ENABLED, 0);
    CALL(W.smtc, SMTC_REMOVE_BUTTON, HRESULT (WINAPI *)(void *, token))(W.smtc, W.pressed);
    release(W.updater), release(W.smtc);
    W.updater = W.smtc = NULL;
}
