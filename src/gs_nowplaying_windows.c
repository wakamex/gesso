// Windows: the window's System Media Transport Controls, through the Windows Runtime, from C.
// combase.dll is opened at run time and the interfaces used are declared as their tables of
// functions, in the order windows.media.h (Windows SDK 10.0.26100) gives them.
#include <SDL3/SDL.h>
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

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

// ---- Who is playing ----
// The system's media card names the app by its app ID, which it looks up among the Start menu's
// shortcuts, as it finds Google Chrome by the shortcut whose app ID is "Chrome"; a program with no
// such shortcut shows as an unknown app with no icon. So the process and its window take `id` as
// their app ID, and the current user's Start menu holds a shortcut named `name` to this program,
// with that app ID and the program's icon, written again when the program has moved.

static const PROPERTYKEY KEY_APP_ID = { { 0x9f4c2855, 0x9f79, 0x4b39, { 0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3 } }, 5 };  // PKEY_AppUserModel_ID
static const GUID CLSID_LINK = { 0x00021401, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };       // CLSID_ShellLink
static const GUID IID_LINK = { 0x000214f9, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };         // IShellLinkW
static const GUID IID_FILE = { 0x0000010b, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };         // IPersistFile
static const GUID IID_STORE = { 0x886d8eeb, 0x8cf2, 0x4446, { 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99 } };        // IPropertyStore
static const GUID FOLDER_PROGRAMS = { 0xa77f5d77, 0x2e2b, 0x44c3, { 0xa6, 0xa2, 0xab, 0xa6, 0x01, 0x05, 0x4a, 0x51 } };   // FOLDERID_Programs

static void set_app_id(IPropertyStore *store, WCHAR *id) {
    PROPVARIANT v = { 0 };
    v.vt = VT_LPWSTR, v.pwszVal = id;
    IPropertyStore_SetValue(store, &KEY_APP_ID, &v);
    IPropertyStore_Commit(store);
}

// Whether the shortcut at `path` already leads to `exe` with app ID `id`.
static bool shortcut_holds(const WCHAR *path, const WCHAR *exe, const WCHAR *id) {
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    IPropertyStore *store = NULL;
    bool same = false;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || FAILED(CoCreateInstance(&CLSID_LINK, NULL, CLSCTX_INPROC_SERVER, &IID_LINK, (void **)&link))) return false;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_FILE, (void **)&file)) && SUCCEEDED(IPersistFile_Load(file, path, STGM_READ))) {
        WCHAR target[MAX_PATH] = L"";
        PROPVARIANT v = { 0 };
        IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0);
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_STORE, (void **)&store)) && SUCCEEDED(IPropertyStore_GetValue(store, &KEY_APP_ID, &v)))
            same = !_wcsicmp(target, exe) && v.vt == VT_LPWSTR && v.pwszVal && !wcscmp(v.pwszVal, id), PropVariantClear(&v);
    }
    if (store) IPropertyStore_Release(store);
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
    return same;
}

static void identify(HWND hwnd, const char *name, const char *id) {
    WCHAR *wid = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", id, SDL_strlen(id) + 1), exe[MAX_PATH], *programs = NULL, path[MAX_PATH + 300];
    if (!wid || !wid[0] || !GetModuleFileNameW(NULL, exe, MAX_PATH)) { SDL_free(wid); return; }
    SetCurrentProcessExplicitAppUserModelID(wid);
    IPropertyStore *store = NULL;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, &IID_STORE, (void **)&store))) set_app_id(store, wid), IPropertyStore_Release(store), store = NULL;
    // The shortcut's file name: the app's name, without what a file name cannot hold.
    char file_name[200];
    size_t n = 0;
    for (const char *c = name && *name ? name : id; *c && n + 1 < sizeof file_name; c++)
        if (!strchr("\\/:*?\"<>|", *c) && (unsigned char)*c >= 32) file_name[n++] = *c;
    file_name[n] = 0;
    WCHAR *wname = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", file_name, n + 1);
    if (wname && wname[0] && SUCCEEDED(SHGetKnownFolderPath(&FOLDER_PROGRAMS, 0, NULL, &programs)) && _snwprintf(path, MAX_PATH + 300, L"%ls\\%ls.lnk", programs, wname) > 0 &&
        !shortcut_holds(path, exe, wid)) {
        IShellLinkW *link = NULL;
        IPersistFile *file = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_LINK, NULL, CLSCTX_INPROC_SERVER, &IID_LINK, (void **)&link))) {
            WCHAR dir[MAX_PATH];
            wcscpy(dir, exe);
            WCHAR *slash = wcsrchr(dir, L'\\');
            if (slash) *slash = 0;
            IShellLinkW_SetPath(link, exe), IShellLinkW_SetWorkingDirectory(link, dir), IShellLinkW_SetIconLocation(link, exe, 0);
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_STORE, (void **)&store))) set_app_id(store, wid), IPropertyStore_Release(store);
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_FILE, (void **)&file))) IPersistFile_Save(file, path, TRUE), IPersistFile_Release(file);
            IShellLinkW_Release(link);
        }
    }
    CoTaskMemFree(programs);
    SDL_free(wname), SDL_free(wid);
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
    identify(hwnd, name, id);
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

// Logs a call that failed, once for each call and result, so that what the system refused is in the log.
static void check(HRESULT hr, const char *what) {
    static char said[8][64];
    if (SUCCEEDED(hr)) return;
    char line[64];
    SDL_snprintf(line, sizeof line, "%s %08lx", what, (unsigned long)hr);
    for (int i = 0; i < 8; i++)
        if (!SDL_strcmp(said[i], line)) return;
    for (int i = 0; i < 8; i++)
        if (!said[i][0]) { SDL_strlcpy(said[i], line, sizeof said[i]); break; }
    SDL_Log("gs_nowplaying: %s failed: 0x%08lx", what, (unsigned long)hr);
}

void gs_nowplaying_set(const char *title, const char *artist, int state) {
    if (!W.smtc) return;
    void *music = NULL;
    HRESULT hr = W.updater ? CALL(W.updater, UPDATER_GET_MUSIC, HRESULT (WINAPI *)(void *, void **))(W.updater, &music) : E_POINTER;
    check(hr, "get_MusicProperties");
    if (SUCCEEDED(hr) && music) {
        hstring t = string(title ? title : ""), a = string(artist ? artist : "");
        check(CALL(music, MUSIC_PUT_TITLE, HRESULT (WINAPI *)(void *, hstring))(music, t), "put_Title");
        check(CALL(music, MUSIC_PUT_ARTIST, HRESULT (WINAPI *)(void *, hstring))(music, a), "put_Artist");
        W.delete_string(t), W.delete_string(a), release(music);
        // (the type again before each update, as Chromium does: without it the title and artist may not show)
        check(CALL(W.updater, UPDATER_PUT_TYPE, HRESULT (WINAPI *)(void *, int))(W.updater, TYPE_MUSIC), "put_Type");
        check(CALL(W.updater, UPDATER_UPDATE, HRESULT (WINAPI *)(void *))(W.updater), "Update");
    }
    check(CALL(W.smtc, SMTC_PUT_STATUS, HRESULT (WINAPI *)(void *, int))(W.smtc, state == GS_NOWPLAYING_PLAYING ? STATUS_PLAYING : state == GS_NOWPLAYING_PAUSED ? STATUS_PAUSED : STATUS_STOPPED), "put_PlaybackStatus");
    check(PUT_BOOL(W.smtc, SMTC_PUT_ENABLED, state != GS_NOWPLAYING_STOPPED), "put_IsEnabled");
}

void gs_nowplaying_quit(void) {
    if (!W.smtc) return;
    PUT_BOOL(W.smtc, SMTC_PUT_ENABLED, 0);
    CALL(W.smtc, SMTC_REMOVE_BUTTON, HRESULT (WINAPI *)(void *, token))(W.smtc, W.pressed);
    release(W.updater), release(W.smtc);
    W.updater = W.smtc = NULL;
}
