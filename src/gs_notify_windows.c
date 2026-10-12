// Windows: toast notifications through the Windows Runtime, called from C. A program without an
// installer may show toasts once its identifier is registered for the user, with the name and icon
// to show beside them. combase.dll is opened at run time and the few interfaces used are declared
// here as their tables of functions, in the order the system defines them.
#include <SDL3/SDL.h>
#define COBJMACROS
#include <windows.h>

#include "gs_notify.h"

#define LIVE 64  // toasts remembered, for taking them back

typedef void *hstring;  // an HSTRING
typedef struct { void **v; } obj;  // any Windows Runtime object: a pointer to its table of functions
typedef struct { __int64 value; } token;

// Positions in the tables. Every interface begins with IUnknown's three and IInspectable's three.
enum { QUERY = 0, ADDREF = 1, RELEASE = 2, FIRST = 6 };
enum { STATICS_CREATE_NOTIFIER_WITH_ID = FIRST + 1 };            // IToastNotificationManagerStatics
enum { NOTIFIER_SHOW = FIRST, NOTIFIER_HIDE = FIRST + 1 };        // IToastNotifier
enum { FACTORY_CREATE = FIRST };                                  // IToastNotificationFactory
enum { TOAST_ADD_ACTIVATED = FIRST + 5 };                         // IToastNotification
enum { TOAST2_PUT_TAG = FIRST };                                  // IToastNotification2
enum { XML_LOAD = FIRST };                                        // IXmlDocumentIO

static const GUID IID_STATICS = { 0x50ac103f, 0xd235, 0x4598, { 0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4 } };
static const GUID IID_FACTORY = { 0x04124b20, 0x82c6, 0x4229, { 0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53 } };
static const GUID IID_TOAST2 = { 0x9dfb9fd1, 0x143a, 0x490e, { 0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7 } };
static const GUID IID_XML_IO = { 0x6cd0e74e, 0xee65, 0x4489, { 0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37 } };
static const GUID IID_XML_DOCUMENT = { 0xf7f3a506, 0x1e87, 0x42d6, { 0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94 } };
static const GUID IID_ACTIVATED = { 0xab54de2d, 0x97d9, 0x5528, { 0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30 } };  // TypedEventHandler<ToastNotification, Object>
static const GUID IID_UNKNOWN = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_AGILE = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

#define CALL(o, slot, type) ((type)((obj *)(o))->v[slot])
static void release(void *o) { if (o) CALL(o, RELEASE, ULONG (WINAPI *)(void *))(o); }
static void *query(void *o, const GUID *iid) {
    void *out = NULL;
    return o && SUCCEEDED(CALL(o, QUERY, HRESULT (WINAPI *)(void *, const GUID *, void **))(o, iid, &out)) ? out : NULL;
}

// The object handed to a toast to be called when it is clicked: it carries the notification's tag.
typedef struct { void **v; LONG refs; char tag[128]; } handler;

static struct {
    HMODULE combase;
    HRESULT (WINAPI *create_string)(const WCHAR *, UINT32, hstring *);
    HRESULT (WINAPI *delete_string)(hstring);
    HRESULT (WINAPI *get_factory)(hstring, const GUID *, void **);
    HRESULT (WINAPI *activate)(hstring, void **);
    void *notifier, *factory;
    void (*clicked)(const char *tag);
    SDL_Mutex *lock;
    struct { char tag[128]; void *toast; } live[LIVE];
} W;

static HRESULT WINAPI handler_query(handler *h, const GUID *iid, void **out) {
    if (!IsEqualGUID(iid, &IID_UNKNOWN) && !IsEqualGUID(iid, &IID_AGILE) && !IsEqualGUID(iid, &IID_ACTIVATED)) return *out = NULL, E_NOINTERFACE;
    InterlockedIncrement(&h->refs);
    return *out = h, S_OK;
}
static ULONG WINAPI handler_addref(handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }
static ULONG WINAPI handler_release(handler *h) {
    LONG refs = InterlockedDecrement(&h->refs);
    if (!refs) SDL_free(h);
    return (ULONG)refs;
}
static HRESULT WINAPI handler_invoke(handler *h, void *toast, void *args) {  // (on a thread of the system's)
    if (W.clicked) W.clicked(h->tag);
    return S_OK;
}
static void *handler_table[] = { handler_query, handler_addref, handler_release, handler_invoke };

static hstring string(const char *utf8) {
    WCHAR *wide = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", utf8, SDL_strlen(utf8) + 1);
    hstring s = NULL;
    if (wide) W.create_string(wide, (UINT32)SDL_wcslen(wide), &s);
    SDL_free(wide);
    return s;
}

static void set_registry(HKEY key, const WCHAR *name, const char *utf8) {
    WCHAR *wide = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", utf8, SDL_strlen(utf8) + 1);
    if (wide) RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)wide, (DWORD)((SDL_wcslen(wide) + 1) * sizeof(WCHAR)));
    SDL_free(wide);
}

bool gs_notify_init(const char *name, const char *id, const char *icon, void (*clicked)(const char *tag)) {
    if (W.notifier) return true;
    W.combase = LoadLibraryW(L"combase.dll");
    HRESULT (WINAPI *initialize)(int) = W.combase ? (HRESULT (WINAPI *)(int))(void *)GetProcAddress(W.combase, "RoInitialize") : NULL;
    if (!initialize) return false;  // (before Windows 8)
    W.create_string = (HRESULT (WINAPI *)(const WCHAR *, UINT32, hstring *))(void *)GetProcAddress(W.combase, "WindowsCreateString");
    W.delete_string = (HRESULT (WINAPI *)(hstring))(void *)GetProcAddress(W.combase, "WindowsDeleteString");
    W.get_factory = (HRESULT (WINAPI *)(hstring, const GUID *, void **))(void *)GetProcAddress(W.combase, "RoGetActivationFactory");
    W.activate = (HRESULT (WINAPI *)(hstring, void **))(void *)GetProcAddress(W.combase, "RoActivateInstance");
    if (!W.create_string || !W.delete_string || !W.get_factory || !W.activate) return false;
    initialize(1 /* multithreaded; a thread already set up another way keeps its own */);

    // The identifier, with the name and icon shown beside the app's toasts, for this user.
    char path[300];
    SDL_snprintf(path, sizeof path, "Software\\Classes\\AppUserModelId\\%s", id);
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) return false;
    set_registry(key, L"DisplayName", name);
    if (icon && *icon) set_registry(key, L"IconUri", icon);
    RegCloseKey(key);

    hstring manager = string("Windows.UI.Notifications.ToastNotificationManager"), toast = string("Windows.UI.Notifications.ToastNotification"), app = string(id);
    void *statics = NULL;
    if (manager && toast && app && SUCCEEDED(W.get_factory(manager, &IID_STATICS, &statics)) && SUCCEEDED(W.get_factory(toast, &IID_FACTORY, &W.factory)))
        CALL(statics, STATICS_CREATE_NOTIFIER_WITH_ID, HRESULT (WINAPI *)(void *, hstring, void **))(statics, app, &W.notifier);
    release(statics);
    W.delete_string(manager), W.delete_string(toast), W.delete_string(app);
    if (!W.notifier) return false;
    W.clicked = clicked, W.lock = SDL_CreateMutex();
    return true;
}

static void put_escaped(char *out, size_t size, const char *text) {
    size_t n = SDL_strlen(out);
    for (const char *p = text; *p && n + 7 < size; p++) {
        const char *as = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '"' ? "&quot;" : NULL;
        if (as) n += SDL_strlcpy(out + n, as, size - n);
        else if ((unsigned char)*p >= 0x20 || *p == '\n') out[n++] = *p;
    }
    out[n] = 0;
}

static int find(const char *tag) {
    for (int i = 0; i < LIVE; i++)
        if (W.live[i].toast && !SDL_strcmp(W.live[i].tag, tag)) return i;
    return -1;
}

bool gs_notify_show(const char *tag, const char *title, const char *body) {
    if (!W.notifier || !tag || !*tag) return false;
    char xml[4096] = "<toast><visual><binding template=\"ToastGeneric\"><text>";
    put_escaped(xml, sizeof xml - 64, title);
    SDL_strlcat(xml, "</text><text>", sizeof xml);
    put_escaped(xml, sizeof xml - 40, body ? body : "");
    SDL_strlcat(xml, "</text></binding></visual></toast>", sizeof xml);

    // A toast is made from an XML document; one with the tag of an earlier toast takes its place.
    unsigned hash = 2166136261u;
    for (const char *p = tag; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    char short_tag[16];
    SDL_snprintf(short_tag, sizeof short_tag, "%08x", hash);  // (a toast's own tag is short on older systems)
    hstring doc_class = string("Windows.Data.Xml.Dom.XmlDocument"), text = string(xml), toast_tag = string(short_tag);
    void *inspectable = NULL, *io = NULL, *doc = NULL, *toast = NULL, *toast2 = NULL;
    bool ok = doc_class && text && toast_tag && SUCCEEDED(W.activate(doc_class, &inspectable)) && (io = query(inspectable, &IID_XML_IO)) &&
              SUCCEEDED(CALL(io, XML_LOAD, HRESULT (WINAPI *)(void *, hstring))(io, text)) && (doc = query(inspectable, &IID_XML_DOCUMENT)) &&
              SUCCEEDED(CALL(W.factory, FACTORY_CREATE, HRESULT (WINAPI *)(void *, void *, void **))(W.factory, doc, &toast)) && toast;
    if (ok && (toast2 = query(toast, &IID_TOAST2))) CALL(toast2, TOAST2_PUT_TAG, HRESULT (WINAPI *)(void *, hstring))(toast2, toast_tag);
    handler *h = ok ? SDL_calloc(1, sizeof *h) : NULL;
    if (h) {
        token t;
        h->v = handler_table, h->refs = 1;
        SDL_strlcpy(h->tag, tag, sizeof h->tag);
        CALL(toast, TOAST_ADD_ACTIVATED, HRESULT (WINAPI *)(void *, void *, token *))(toast, h, &t);
        handler_release(h);  // (the toast holds it now)
    }
    ok = ok && SUCCEEDED(CALL(W.notifier, NOTIFIER_SHOW, HRESULT (WINAPI *)(void *, void *))(W.notifier, toast));
    if (ok) {
        SDL_LockMutex(W.lock);
        int at = find(tag);
        for (int i = 0; at < 0 && i < LIVE; i++)
            if (!W.live[i].toast) at = i;
        if (at < 0) at = 0;
        release(W.live[at].toast);
        SDL_strlcpy(W.live[at].tag, tag, sizeof W.live[at].tag);
        W.live[at].toast = toast, toast = NULL;
        SDL_UnlockMutex(W.lock);
    }
    release(toast), release(toast2), release(doc), release(io), release(inspectable);
    W.delete_string(doc_class), W.delete_string(text), W.delete_string(toast_tag);
    return ok;
}

void gs_notify_close(const char *tag) {
    if (!W.notifier || !tag) return;
    SDL_LockMutex(W.lock);
    int at = find(tag);
    void *toast = at >= 0 ? W.live[at].toast : NULL;
    if (at >= 0) W.live[at].toast = NULL;
    SDL_UnlockMutex(W.lock);
    if (toast) CALL(W.notifier, NOTIFIER_HIDE, HRESULT (WINAPI *)(void *, void *))(W.notifier, toast), release(toast);
}
